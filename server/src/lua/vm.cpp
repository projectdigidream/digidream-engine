#include "lua/vm.hpp"

#include "lua/fx_binding.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
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

    vm->used_this_call_ += static_cast<u32>(vm->hook_step_);
    vm->used_this_tick_ += static_cast<u32>(vm->hook_step_);
    if (vm->profile_) vm->profile_sample(L);

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
    // The cost is real and worth naming: LuaJIT's headline feature is
    // forfeited. This used to add that the fx arithmetic these scripts spend
    // their time in "lives in C functions the JIT would not have accelerated
    // much anyway" -- which was a guess, and the sort that survives because it
    // is comfortable. It has now been measured, and it is right.
    //
    // Turning the JIT on and removing the hook, then running 1000 monsters on
    // maps/mobperf.map through tools/mobperf/run.js, three alternating runs of
    // each build:
    //
    //     JIT on    4.11  4.64  4.01   us of Lua per awake mob per tick
    //     JIT off   3.90  4.92  4.00
    //
    // Indistinguishable, and the control says that is a fact about the
    // workload rather than a JIT that failed to engage: in the SAME binary
    // `jit.status()` returns true and a 500k-iteration pure-Lua numeric loop
    // runs in 0.72 ms against the interpreter's 3.19 ms. The JIT works. It has
    // nothing to do here, because a mob think is a few dozen calls into C --
    // engine bindings and fx metamethods -- with almost no Lua between them.
    //
    // Which settles the question this comment used to leave open. Trading the
    // instruction budget for the JIT under Trust::Full was a planned step; it
    // would have bought nothing and cost the only control that can stop a
    // runaway script. Making those calls cheaper is a different piece of work
    // and does not start here.
    luaJIT_setmode(L_, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    const char* prof = std::getenv("DNP_LUA_PROFILE");
    profile_ = prof != nullptr && prof[0] == '1';
    hook_step_ = profile_ ? 1000 : HOOK_GRANULARITY;
    lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, hook_step_);
    return true;
}

// Where the script is, two frames deep, charged with the time since the last
// sample. Diagnostic only, behind DNP_LUA_PROFILE.
void Vm::profile_sample(lua_State* L) {
    const u64 now = now_us();
    const u64 dt = prof_last_us_ != 0 && now > prof_last_us_ ? now - prof_last_us_ : 0;
    prof_last_us_ = now;
    if (dt == 0) return;
    std::string key;
    lua_Debug ar;
    for (int level = 0; level < 3 && lua_getstack(L, level, &ar); ++level) {
        if (!lua_getinfo(L, "Sln", &ar)) break;
        const char* src = ar.short_src;
        const char* slash = std::strrchr(src, '/');
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s%s:%d(%s)", level ? " <- " : "", slash ? slash + 1 : src,
                      ar.currentline, ar.name ? ar.name : "?");
        key += buf;
    }
    prof_[key] += dt;
}

void Vm::profile_dump() {
    std::vector<std::pair<u64, const std::string*>> top;
    u64 total = 0;
    for (const auto& [k, v] : prof_) { top.push_back({v, &k}); total += v; }
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "[lua-profile] %llu us sampled\n", static_cast<unsigned long long>(total));
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
        std::fprintf(stderr, "[lua-profile]   %6llu us  %s\n",
                     static_cast<unsigned long long>(top[i].first), top[i].second->c_str());
    }
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
    worst_call_us_ = 0;
    worst_call_what_[0] = '\0';
    script_us_this_tick_ = 0;
    tick_start_us_ = now_us();
    prof_.clear();
    if (L_) lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, hook_step_);
}

void Vm::end_tick(u32 tick) {
    if (!L_) return;
    const u64 spent = now_us() - tick_start_us_;
    if (spent > stats_.worst_tick_us) stats_.worst_tick_us = spent;

    // Warn on TIME SPENT IN SCRIPTS, not on wall time across the tick.
    //
    // `spent` spans begin_tick to end_tick, which is the whole server tick:
    // world.step(), event collection, snapshot encoding, the network, and any
    // moment the OS chose to run something else. Reporting that as "us in
    // script X" made every slow frame look like a Lua problem -- and it was
    // believed, twice, and a monster script was optimised twice for a cost it
    // was not incurring. Measured properly, the callbacks on three loaded maps
    // come to single-digit microseconds while the tick they sit inside takes
    // sixteen milliseconds.
    //
    // The wall time is still printed, because a late tick is worth knowing
    // about. It is just no longer attributed to a script.
    if (script_us_this_tick_ >= cfg_.stall_warn_us) {
        ++stats_.stalls;
        // Named, because the alternative is an operator staring at "fell 300 ms
        // behind" with no idea which script did it. Blocking I/O is allowed
        // here; being anonymous about it is not.
        // The tick total, and the single worst CALLBACK inside it. The total
        // alone says a frame was late; it does not say what to look at, and
        // naming the last script to run actively misleads.
        std::fprintf(stderr,
                     "[lua] tick %u: %llu us in callbacks (worst %llu us in %s); "
                     "tick wall time %llu us (budget warn %u us)\n",
                     tick, static_cast<unsigned long long>(script_us_this_tick_),
                     static_cast<unsigned long long>(worst_call_us_),
                     worst_call_what_[0] ? worst_call_what_ : "(no callback)",
                     static_cast<unsigned long long>(spent), cfg_.stall_warn_us);
        if (profile_) profile_dump();
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
    // A player used a panel (docs/protocol.md section 11).
    "on_ui_event",
    // A player's body arrived in a world, and a player left the game. The game
    // VM's (CrossSink): its state is per player, not per body.
    "on_player_enter", "on_player_leave",
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
    lua_sethook(L_, &Vm::count_hook, LUA_MASKCOUNT, hook_step_);

    ++stats_.callbacks_run;
    // Timed per CALL, not per tick.
    //
    // end_tick measures the whole tick and blames `current_`, which is whatever
    // handler happened to run last -- so a slow doors script was reported as a
    // slow monster script, repeatedly and with total confidence, and two rounds
    // of optimising the monster script did not move the number. A diagnostic
    // that names the wrong thing is worse than none: it is a wrong answer that
    // reads like a measurement.
    const u64 call_start = now_us();
    prof_last_us_ = call_start;
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
    const u64 call_us = now_us() - call_start;
    script_us_this_tick_ += call_us;
    if (perf_on_) {
        const char* slash = std::strrchr(current_, '/');
        char key[192];
        std::snprintf(key, sizeof(key), "%s in %s", fn_name, slash ? slash + 1 : current_);
        perf_by_[key] += call_us;
    }
    if (call_us > worst_call_us_) {
        worst_call_us_ = call_us;
        std::snprintf(worst_call_what_, sizeof(worst_call_what_), "%s in %s",
                      fn_name, current_);
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

void Vm::take_perf_breakdown(std::unordered_map<std::string, u64>& into) {
    for (const auto& [k, v] : perf_by_) into[k] += v;
    perf_by_.clear();
}

void Vm::gc_step() {
    if (L_) lua_gc(L_, LUA_GCSTEP, 4);
}

}  // namespace dnp::server::lua
