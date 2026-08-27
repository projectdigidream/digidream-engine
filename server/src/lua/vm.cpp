#include "lua/vm.hpp"

#include "lua/fx_binding.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "luajit.h"
#include "lualib.h"
}

namespace dnp::server::lua {

namespace {

u64 now_us() {
    using namespace std::chrono;
    return static_cast<u64>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Granularity of the count hook. Fine enough that a runaway is caught in well
// under a frame, coarse enough that the hook itself is not the cost.
constexpr int HOOK_GRANULARITY = 10000;

// Names removed even under Trust::Full.
//
// Not a security list -- the operator's container is the security boundary.
// These are the ones whose failure mode is "the engine looks broken":
//
//   collectgarbage  a script calling "collect" per tick, or altering the pause,
//                   moves GC off the host's fixed schedule and back onto an
//                   arbitrary tick. The symptom is a periodic hitch that looks
//                   like a network problem.
//   debug.sethook   clears the budget hook, so a runaway becomes unkillable.
//                   The rest of `debug` stays: it is genuinely useful for
//                   script authors and reaches nothing the filesystem does not.
const char* const REMOVED_ALWAYS[] = {"collectgarbage"};

// Additionally removed under Trust::Strict.
const char* const REMOVED_STRICT[] = {
    "io", "os", "package", "require", "module", "dofile", "loadfile",
    "load", "loadstring", "ffi", "debug", "coroutine",
};

// Replacement for `print`.
//
// Two reasons it is not the stock one. Script output belongs on the SAME stream
// as the engine's own logs, or the two cannot be correlated on a timeline --
// and stdout is block-buffered to a pipe, so a server killed rather than exited
// cleanly loses every line a script printed. That is exactly how this was
// found: the chunk ran, and said nothing.
//
// The script name is prefixed so that output from several scripts is
// attributable, which matters the moment a map has more than one.
int lua_print_to_stderr(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "dnp_vm");
    const Vm* vm = static_cast<const Vm*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    std::fprintf(stderr, "[lua:%s] ", vm ? vm->current_script() : "?");
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) std::fputc('\t', stderr);
        // Through tostring(), so __tostring metamethods are honoured and a
        // userdata prints as its type rather than as a heap address -- an
        // ASLR-derived value is both an infoleak and a source of output that
        // differs between runs for no reason.
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char* s = lua_tostring(L, -1);
        std::fputs(s ? s : "?", stderr);
        lua_pop(L, 1);
    }
    std::fputc('\n', stderr);
    return 0;
}

}  // namespace

Vm::~Vm() { close(); }

void* Vm::alloc_hook(void* ud, void* ptr, size_t osize, size_t nsize) {
    Vm* vm = static_cast<Vm*>(ud);

    // In Lua 5.1 -- and therefore LuaJIT -- `osize` is only a size when `ptr` is
    // non-null. For a fresh allocation it carries the TYPE TAG of the object
    // being created instead. Subtracting it unconditionally underflows the
    // running total on the very first allocation, and the cap then either
    // refuses everything or nothing depending on where the wrap lands. Found by
    // the suite hanging rather than failing, which is the tell for a size_t
    // that went backwards.
    const size_t old_bytes = ptr ? osize : 0;

    if (nsize == 0) {
        if (ptr) {
            vm->bytes_ -= old_bytes;
            std::free(ptr);
        }
        return nullptr;
    }

    // Refuse rather than abort. An out-of-memory abort takes the whole server
    // down for every player because one script asked for too much; a refused
    // allocation surfaces as an ordinary Lua error naming that script.
    const size_t after = vm->bytes_ - old_bytes + nsize;
    if (after > vm->cfg_.memory_cap) {
        ++vm->stats_.alloc_refusals;
        return nullptr;
    }

    void* np = std::realloc(ptr, nsize);
    if (!np) return nullptr;
    vm->bytes_ = after;
    if (vm->bytes_ > vm->stats_.peak_bytes) vm->stats_.peak_bytes = vm->bytes_;
    return np;
}

void Vm::count_hook(lua_State* L, lua_Debug* /*ar*/) {
    // A hook has no upvalues, so the Vm is found through the registry. That is
    // also why it must be removed from there on close: a stale pointer here is
    // reached from inside the interpreter, where a crash carries no useful
    // stack.
    lua_getfield(L, LUA_REGISTRYINDEX, "dnp_vm");
    Vm* vm = static_cast<Vm*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!vm) return;

    vm->used_this_call_ += HOOK_GRANULARITY;
    vm->used_this_tick_ += HOOK_GRANULARITY;

    const bool over_call = vm->used_this_call_ > vm->cfg_.budget_per_call;
    const bool over_tick = vm->used_this_tick_ > vm->cfg_.budget_per_tick;
    if (!over_call && !over_tick) return;

    if (!vm->killed_) {
        vm->killed_ = true;
        ++vm->stats_.budget_overruns;
        // Re-arm at 1 so the script makes no further progress even if it
        // catches the error: a script that wraps everything in pcall would
        // otherwise swallow its own kill and carry on burning the frame.
        lua_sethook(L, count_hook, LUA_MASKCOUNT, 1);
    }

    luaL_error(L, "instruction budget exceeded in '%s' (%u per call / %u per tick)",
               vm->current_, vm->cfg_.budget_per_call, vm->cfg_.budget_per_tick);
}

bool Vm::open(const VmConfig& cfg) {
    close();
    cfg_ = cfg;
    bytes_ = 0;
    stats_ = VmStats{};

    L_ = lua_newstate(&Vm::alloc_hook, this);
    if (!L_) {
        std::snprintf(err_, sizeof(err_), "lua_newstate failed (memory cap too low?)");
        return false;
    }

    luaL_openlibs(L_);

    // The hook needs to find this Vm from a bare lua_State.
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "dnp_vm");

    install_environment();

    // The JIT is turned OFF, and this is a correctness requirement rather than
    // a tuning choice.
    //
    // LuaJIT does not call instruction hooks from inside compiled traces. A hot
    // loop gets compiled, the count hook stops firing, and the instruction
    // budget silently stops existing -- `while true do end` becomes literally
    // unkillable and takes the tick loop with it. This was found by the test
    // suite hanging rather than failing, which is the only way it presents:
    // with the JIT on the budget does not fail loudly, it just is not there.
    //
    // The cost is real and worth naming: LuaJIT's headline feature is forfeited.
    // Its interpreter is still substantially faster than PUC Lua's, and the fx
    // arithmetic these scripts spend their time in lives in C functions the JIT
    // would not have accelerated much anyway -- but an unenforceable budget was
    // never an option, so this is not a close call.
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, HOOK_GRANULARITY);
    return true;
}

void Vm::install_environment() {
    for (const char* n : REMOVED_ALWAYS) {
        lua_pushnil(L_);
        lua_setglobal(L_, n);
    }

    if (cfg_.trust == Trust::Strict) {
        for (const char* n : REMOVED_STRICT) {
            lua_pushnil(L_);
            lua_setglobal(L_, n);
        }
        // package.loaded still holds live references to the modules just
        // nil'd from the global table, and require() is reachable through it.
        lua_getfield(L_, LUA_REGISTRYINDEX, "_LOADED");
        if (lua_istable(L_, -1)) {
            for (const char* n : REMOVED_STRICT) {
                lua_pushnil(L_);
                lua_setfield(L_, -2, n);
            }
        }
        lua_pop(L_, 1);
    } else {
        // debug.sethook would clear the budget hook, making a runaway
        // unkillable. Everything else in `debug` stays.
        lua_getglobal(L_, "debug");
        if (lua_istable(L_, -1)) {
            lua_pushnil(L_);
            lua_setfield(L_, -2, "sethook");
        }
        lua_pop(L_, 1);
    }

    // The fx type. Installed for both trust profiles: it is not a capability,
    // it is the only exact way to name a number the simulation will accept.
    open_fx(L_);

    // Script output goes to stderr, alongside the engine's own logs.
    lua_pushcfunction(L_, lua_print_to_stderr);
    lua_setglobal(L_, "print");

    // The string metatable is shared by every string in the VM, so a script
    // writing to it changes string behaviour for every other script. Making it
    // opaque costs nothing and removes a whole class of confusing action at a
    // distance.
    lua_pushliteral(L_, "");
    if (lua_getmetatable(L_, -1)) {
        lua_pushliteral(L_, "locked");
        lua_setfield(L_, -2, "__metatable");
        lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
}

void Vm::close() noexcept {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
    }
    bytes_ = 0;
}

bool Vm::run_file(const char* path, const char* name) {
    if (!L_) return false;

    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::snprintf(err_, sizeof(err_), "cannot open script %s", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }

    std::vector<char> src(static_cast<size_t>(n));
    const size_t got = src.empty() ? 0 : std::fread(src.data(), 1, src.size(), f);
    std::fclose(f);
    if (got != src.size()) {
        std::snprintf(err_, sizeof(err_), "short read on %s", path);
        return false;
    }

    // Precompiled bytecode is refused whatever the trust setting. LuaJIT's
    // loader is not memory-safe on malformed input, and the failure is a crash
    // inside the interpreter rather than a Lua error -- which no amount of
    // operator confinement makes debuggable. Lua 5.1 has no `mode` argument to
    // ask the loader to do this, so it is checked here.
    if (!src.empty() && static_cast<unsigned char>(src[0]) == 0x1B) {
        std::snprintf(err_, sizeof(err_), "%s is precompiled bytecode; source only", path);
        return false;
    }

    set_current(name);
    used_this_call_ = 0;
    killed_ = false;

    if (luaL_loadbuffer(L_, src.data(), src.size(), name) != 0 ||
        lua_pcall(L_, 0, 0, 0) != 0) {
        std::snprintf(err_, sizeof(err_), "%s", lua_tostring(L_, -1) ? lua_tostring(L_, -1)
                                                                     : "unknown error");
        lua_pop(L_, 1);
        ++stats_.errors;
        return false;
    }
    // Collect whatever hooks this chunk defined before the next one runs.
    harvest_hooks();
    return true;
}

void Vm::set_current(const char* name) noexcept {
    std::snprintf(current_, sizeof(current_), "%s", name ? name : "");
}

void Vm::begin_tick() {
    used_this_tick_ = 0;
    used_this_call_ = 0;
    killed_ = false;
    tick_start_us_ = now_us();
    if (L_) lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, HOOK_GRANULARITY);
}

void Vm::end_tick(u32 tick) {
    if (!L_) return;
    const u64 spent = now_us() - tick_start_us_;
    if (spent > stats_.worst_tick_us) stats_.worst_tick_us = spent;
    if (spent >= cfg_.stall_warn_us) {
        ++stats_.stalls;
        // Named, because the alternative is an operator staring at "fell 300 ms
        // behind" with no idea which script did it. Blocking I/O is allowed
        // here; being anonymous about it is not.
        std::fprintf(stderr,
                     "[lua] tick %u: %llu us in script '%s' (budget warn %u us)\n",
                     tick, static_cast<unsigned long long>(spent), current_,
                     cfg_.stall_warn_us);
    }
}

// The registry table holding every hook's handler list.
static const char* const HOOKS_KEY = "dnp.hooks";

// And, parallel to it, the name of the script each handler came from.
//
// Without this the stall warning always named the LAST script loaded, whatever
// was actually running -- `current_` was set at load time and never touched
// again. So the one line whose entire purpose is "which script did this" was
// answering a different question, confidently. A handler's owner is known at
// harvest time and nowhere else, so it is recorded there.
static const char* const OWNERS_KEY = "dnp.hookowners";

// Every hook the engine dispatches. Named in one place so harvesting and
// calling cannot disagree about what a hook is.
static const char* const HOOK_NAMES[] = {
    "on_tick", "on_post_tick", "on_interact", "on_cross",
    "on_touch", "on_prop_use", "on_attack", "on_crush",
};

// Push the handler list for `fn_name`, creating it if absent. Leaves the table
// on the stack.
static void push_list(lua_State* L, const char* key, const char* fn_name) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
    lua_getfield(L, -1, fn_name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, fn_name);
    }
    lua_remove(L, -2);   // drop the outer hooks table
}

static void push_hook_list(lua_State* L, const char* fn_name) {
    push_list(L, HOOKS_KEY, fn_name);
}
static void push_owner_list(lua_State* L, const char* fn_name) {
    push_list(L, OWNERS_KEY, fn_name);
}

void Vm::harvest_hooks() {
    if (!L_) return;
    for (const char* name : HOOK_NAMES) {
        lua_getglobal(L_, name);
        if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); continue; }

        push_hook_list(L_, name);
        const int n = static_cast<int>(lua_objlen(L_, -1));
        lua_pushvalue(L_, -2);          // the function
        lua_rawseti(L_, -2, n + 1);
        lua_pop(L_, 2);                 // list, function

        // Same slot in the parallel list: who registered it. current_ is the
        // script being loaded right now, which is exactly the answer.
        push_owner_list(L_, name);
        lua_pushstring(L_, current_);
        lua_rawseti(L_, -2, n + 1);
        lua_pop(L_, 1);

        // Cleared, so the NEXT script starts from nothing. Without this a
        // script would see the previous one's handler as a global and the old
        // "capture and chain" idiom would call it twice.
        lua_pushnil(L_);
        lua_setglobal(L_, name);
    }
}

u32 Vm::hook_count(const char* fn_name) const {
    if (!L_) return 0;
    push_hook_list(L_, fn_name);
    const u32 n = static_cast<u32>(lua_objlen(L_, -1));
    lua_pop(L_, 1);
    return n;
}

bool Vm::begin_hook_at(const char* fn_name, u32 i) {
    if (!L_) return false;
    // Blame the script that registered THIS handler, for as long as it runs.
    push_owner_list(L_, fn_name);
    lua_rawgeti(L_, -1, static_cast<int>(i) + 1);
    if (const char* owner = lua_tostring(L_, -1)) set_current(owner);
    lua_pop(L_, 2);

    push_hook_list(L_, fn_name);
    lua_rawgeti(L_, -1, static_cast<int>(i) + 1);
    lua_remove(L_, -2);
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 1); return false; }
    return true;
}

bool Vm::begin_hook(const char* fn_name) { return begin_hook_at(fn_name, 0); }

void Vm::end_hook(const char* fn_name, u32 tick, int nargs) {
    if (!L_) return;

    // Each callback gets its own per-call budget; the per-tick total keeps
    // accumulating across them, so a thousand individually cheap callbacks
    // still cannot blow the frame.
    used_this_call_ = 0;
    killed_ = false;
    lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, HOOK_GRANULARITY);

    ++stats_.callbacks_run;
    if (lua_pcall(L_, nargs, 0, 0) != 0) {
        ++stats_.errors;
        const char* msg = lua_tostring(L_, -1);
        // Rate-limited by tick rather than silenced: an error every frame would
        // bury the log, and an error nobody sees is a script that quietly does
        // nothing.
        if (stats_.errors <= 3 || (tick % 300) == 0) {
            std::fprintf(stderr, "[lua] %s error (tick %u): %s\n", fn_name, tick,
                         msg ? msg : "unknown");
        }
        lua_pop(L_, 1);
    }
}

void Vm::call_hook(const char* fn_name, u32 tick) {
    // Every registered handler, in load order. Each gets its own per-call
    // budget; the per-tick total keeps accumulating across them, so ten cheap
    // scripts still cannot blow the frame between them.
    const u32 n = hook_count(fn_name);
    for (u32 i = 0; i < n; ++i) {
        if (!begin_hook_at(fn_name, i)) continue;
        end_hook(fn_name, tick, 0);
    }
}

void Vm::gc_step() {
    if (L_) lua_gc(L_, LUA_GCSTEP, 4);
}

}  // namespace dnp::server::lua
