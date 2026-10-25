//
// The Lua runtime: trust profiles, budgets, and the limits that keep the tick
// on time.
//
// The trust model here is that the OPERATOR confines the server process, not
// that the interpreter confines the script. So most of this file is not about
// security at all -- it is about the failure modes that make a Lua bug look
// like an engine bug:
//
//   a runaway loop stalls the 60 Hz tick, main.cpp drops the missed ticks past
//   250 ms, and every client sees a burst of corrections.
//
// The strict profile is still tested, because it is a supported flag and an
// allowlist nobody exercises is an allowlist that has already rotted.
//
#include <cstdio>
#include <cstring>

#include "lua/vm.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

using namespace dnp;
using namespace dnp::server::lua;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

// Run a chunk; true if it completed without error.
static bool run(Vm& vm, const char* src) {
    lua_State* L = vm.state();
    vm.begin_tick();
    if (luaL_loadbuffer(L, src, std::strlen(src), "=probe") != 0) { lua_pop(L, 1); return false; }
    const bool ok = lua_pcall(L, 0, 0, 0) == 0;
    if (!ok) lua_pop(L, 1);
    return ok;
}

// True if the expression is non-nil -- i.e. the capability is reachable.
static bool reachable(Vm& vm, const char* expr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "if (%s) == nil then error('absent') end", expr);
    return run(vm, buf);
}

// ------------------------------------------------------------- trust profiles

static void test_full_trust_reaches_the_machine() {
    std::printf("lua: Trust::Full reaches disk, os and native libraries\n");

    Vm vm;
    VmConfig cfg;
    cfg.trust = Trust::Full;
    CHECK(vm.open(cfg));

    // The decided model: scripts may reach the filesystem and databases. `ffi`
    // is how LuaJIT talks to sqlite and libpq, so allowing databases while
    // forbidding ffi would be incoherent.
    CHECK(reachable(vm, "io"));
    CHECK(reachable(vm, "io.open"));
    CHECK(reachable(vm, "os"));
    CHECK(reachable(vm, "os.time"));
    CHECK(reachable(vm, "require"));
#if __has_include("luajit.h")
    CHECK(reachable(vm, "require('ffi')"));   // PUC Lua 5.1 (the web build) has no ffi
#endif
    CHECK(reachable(vm, "package"));

    // Actually touch the disk, rather than merely finding the symbol.
    CHECK(run(vm, "local f = io.open('/dev/null', 'r'); if not f then error('no open') end; f:close()"));

    std::printf("       io, os, require and ffi all live\n");
}

static void test_strict_trust_is_still_closed() {
    std::printf("lua: Trust::Strict still closes the allowlist\n");

    Vm vm;
    VmConfig cfg;
    cfg.trust = Trust::Strict;
    CHECK(vm.open(cfg));

    const char* const gone[] = {
        "io", "os", "package", "require", "dofile", "loadfile",
        "load", "loadstring", "debug", "coroutine",
    };
    for (const char* g : gone) CHECK(!reachable(vm, g));

    // `print` survives both profiles, because it is not the stock one: it
    // writes to stderr with the script name prefixed, so script output lands on
    // the same stream as the engine's logs and survives a server that is killed
    // rather than exited cleanly. Removing it would only push authors toward
    // io.write, which strict mode does not have either.
    CHECK(reachable(vm, "print"));

    // The loaded-module table holds a SECOND reference to every module, so
    // nil-ing the global alone leaves io reachable as package.loaded.io. This
    // chunk raises only if such a leak exists, so success is the pass.
    CHECK(run(vm, "local m = package and package.loaded; if m and m.io then error('leak') end"));
}

// THE control for the two tests above.
//
// Every "X is absent" assertion is worthless without proof the probe would have
// found X had it been there: a typo in the probe name passes the strict suite
// perfectly. So the same probes run against a Full VM, where each must succeed.
static void test_the_absence_probes_can_detect_presence() {
    std::printf("lua: CONTROL -- the strict probes detect presence when it is there\n");

    Vm strict, full;
    VmConfig sc; sc.trust = Trust::Strict;
    VmConfig fc; fc.trust = Trust::Full;
    CHECK(strict.open(sc));
    CHECK(full.open(fc));

    const char* const names[] = {"io", "os", "package", "require", "debug", "coroutine"};
    int absent_in_strict = 0;
    int present_in_full  = 0;
    for (const char* n : names) {
        if (!reachable(strict, n)) ++absent_in_strict;
        if (reachable(full, n))    ++present_in_full;
    }
    const int total = static_cast<int>(sizeof(names) / sizeof(names[0]));
    CHECK(absent_in_strict == total);
    CHECK(present_in_full == total);
    std::printf("       %d/%d absent under strict, %d/%d present under full\n",
                absent_in_strict, total, present_in_full, total);
}

// ------------------------------------------------------------------- budgets

static void test_a_runaway_is_killed() {
    std::printf("lua: a runaway loop is killed, and cannot swallow its own kill\n");

    Vm vm;
    VmConfig cfg;
    cfg.budget_per_call = 200000;
    cfg.budget_per_tick = 400000;
    CHECK(vm.open(cfg));

    CHECK(!run(vm, "while true do end"));
    CHECK(vm.stats().budget_overruns > 0);

    // A script just under budget still completes, or the budget is simply
    // "Lua does not work".
    Vm ok_vm;
    CHECK(ok_vm.open(cfg));
    CHECK(run(ok_vm, "local n = 0 for i = 1, 1000 do n = n + i end"));
    CHECK(ok_vm.stats().budget_overruns == 0);

    // And the kill cannot be caught. After the hook trips it is re-armed at 1
    // instruction, so a script that wraps everything in pcall makes no further
    // progress instead of carrying on burning the frame.
    Vm p;
    CHECK(p.open(cfg));
    CHECK(!run(p, "pcall(function() while true do end end) ; while true do end"));
    CHECK(p.stats().budget_overruns > 0);
    std::printf("       killed, and a pcall-wrapped runaway is killed too\n");
}

static void test_memory_is_capped() {
    std::printf("lua: memory is capped -- the instruction hook cannot see an allocation\n");

    Vm vm;
    VmConfig cfg;
    cfg.memory_cap = 8u * 1024u * 1024u;
    CHECK(vm.open(cfg));

    // ONE VM instruction. The count hook will never fire on it, which is
    // exactly why a counting allocator is not optional.
    CHECK(!run(vm, "local s = ('x'):rep(64 * 1024 * 1024)"));
    CHECK(vm.stats().alloc_refusals > 0);

    // The control: the same allocation with room to spare must succeed, or the
    // test above only proves that rep() is broken.
    Vm big;
    VmConfig bc;
    bc.memory_cap = 512u * 1024u * 1024u;
    CHECK(big.open(bc));
    CHECK(run(big, "local s = ('x'):rep(64 * 1024 * 1024) if #s ~= 64*1024*1024 then error('short') end"));
    CHECK(big.stats().alloc_refusals == 0);
    std::printf("       refused at 8 MB, succeeded at 512 MB\n");
}

static void test_the_budget_hook_cannot_be_removed() {
    std::printf("lua: debug.sethook cannot clear the budget hook\n");

    Vm vm;
    VmConfig cfg;
    cfg.budget_per_call = 200000;
    cfg.budget_per_tick = 400000;
    CHECK(vm.open(cfg));

    // debug survives under Full trust -- it is useful and reaches nothing the
    // filesystem does not -- but sethook does not, because clearing the hook
    // makes a runaway unkillable.
    CHECK(reachable(vm, "debug"));
    CHECK(!reachable(vm, "debug.sethook"));

    // The attempt is wrapped in pcall deliberately. Without it the chunk dies
    // on "attempt to call nil" before ever reaching the loop, and the test
    // passes for entirely the wrong reason -- it would still pass with the
    // budget removed. What is being checked is that a script which TRIES to
    // clear the hook, and carries on regardless, is still killed.
    CHECK(!run(vm, "pcall(function() debug.sethook(function() end, '') end) "
                   "while true do end"));
    CHECK(vm.stats().budget_overruns > 0);
}

static void test_bytecode_is_refused() {
    std::printf("lua: precompiled bytecode is refused whatever the trust setting\n");

    // LuaJIT's bytecode loader is not memory-safe on malformed input, and it
    // fails by crashing inside the interpreter rather than raising a Lua error.
    // No amount of operator confinement makes that debuggable.
    const char* path = "/tmp/dnp_bytecode_probe.luac";
    std::FILE* f = std::fopen(path, "wb");
    if (f) {
        const unsigned char header[] = {0x1B, 'L', 'J', 0x02, 0x00};
        std::fwrite(header, 1, sizeof(header), f);
        std::fclose(f);
    }

    Vm vm;
    VmConfig cfg;
    CHECK(vm.open(cfg));
    CHECK(!vm.run_file(path, "probe"));
    CHECK(std::strstr(vm.last_error(), "bytecode") != nullptr);
    std::printf("       refused: %s\n", vm.last_error());
    std::remove(path);
}

static void test_string_metatable_is_locked() {
    std::printf("lua: the shared string metatable cannot be repointed\n");

    Vm vm;
    VmConfig cfg;
    CHECK(vm.open(cfg));
    // One script rewriting it changes string behaviour for every other script
    // in the VM -- action at a distance that is miserable to trace.
    CHECK(!run(vm, "setmetatable('', {})"));
    CHECK(run(vm, "if getmetatable('') ~= 'locked' then error('not locked') end"));
}

static void test_collectgarbage_is_the_hosts_job() {
    std::printf("lua: collectgarbage is the host's, so a pause cannot land mid-tick\n");

    Vm vm;
    VmConfig cfg;
    CHECK(vm.open(cfg));
    // A script calling collect() per tick moves GC off the host's fixed
    // schedule and onto an arbitrary tick; the symptom is a periodic hitch
    // indistinguishable from a network problem.
    CHECK(!reachable(vm, "collectgarbage"));
    vm.gc_step();   // the host's own step still works
}

// The property the whole budget rests on.
//
// LuaJIT does not call instruction hooks from inside compiled traces, so with
// the JIT enabled a hot loop is unkillable and the budget silently stops
// existing. It does not fail loudly -- it just is not there, which is how the
// first version of this suite came to hang instead of fail.
//
// Pinned as its own check so that turning the JIT back on fails HERE, with an
// explanation, rather than by making some future runaway-loop test hang.
static void test_the_jit_is_off_because_the_budget_requires_it() {
    std::printf("lua: the JIT is off, because an instruction budget cannot survive it\n");

    Vm vm;
    VmConfig cfg;
    CHECK(vm.open(cfg));
    // PUC Lua 5.1 (DNP_LUA=lua51, the web build) has no JIT at all.
    CHECK(run(vm, "if jit and jit.status() then error('JIT is on: the instruction budget "
                  "is unenforceable inside compiled traces') end"));
    std::printf("       jit.status() == false, or no jit\n");
}

// Two scripts, one hook, and both must run.
//
// Hooks used to be plain globals, so the second script to write
// `function on_tick()` silently replaced the first -- a map loading a door
// library and a monster library lost every door the moment it gained a monster,
// with nothing anywhere reporting it. Scripts could chain by capturing the
// previous value, but a convention every author has to remember and no test can
// check is not a mechanism.
static void test_two_scripts_can_share_a_hook() {
    std::printf("lua: two scripts registering the same hook both run, in load order\n");

    const char* a_path = "/tmp/dnp_hook_a.lua";
    const char* b_path = "/tmp/dnp_hook_b.lua";
    const char* a_src = "ORDER = {}\nfunction on_tick() ORDER[#ORDER + 1] = 'a' end\n";
    const char* b_src = "function on_tick() ORDER[#ORDER + 1] = 'b' end\n";
    const char* paths[] = {a_path, b_path};
    const char* srcs[] = {a_src, b_src};
    for (int i = 0; i < 2; ++i) {
        std::FILE* f = std::fopen(paths[i], "wb");
        if (f) { std::fwrite(srcs[i], 1, std::strlen(srcs[i]), f); std::fclose(f); }
    }

    Vm vm;
    VmConfig cfg;
    CHECK(vm.open(cfg));
    CHECK(vm.run_file(a_path, "a"));
    CHECK(vm.run_file(b_path, "b"));

    CHECK(vm.hook_count("on_tick") == 2);
    // A hook nobody registered has no handlers, rather than one that happens to
    // be nil.
    CHECK(vm.hook_count("on_crush") == 0);

    vm.begin_tick();
    vm.call_hook("on_tick", 1);
    vm.end_tick(1);

    // Both ran, and in LOAD ORDER -- which is the order a map declared its
    // scripts, and the only order an author can reason about.
    CHECK(run(vm, "assert(#ORDER == 2 and ORDER[1] == 'a' and ORDER[2] == 'b')"));

    // CONTROL: the global is cleared once harvested, so the old
    // capture-the-previous-value idiom finds nothing and cannot double-call.
    // Without this the mechanism and the convention would both be live.
    CHECK(run(vm, "assert(on_tick == nil)"));

    std::remove(a_path);
    std::remove(b_path);
}

int main() {
    std::printf("== server/lua vm suite ==\n");
    test_full_trust_reaches_the_machine();
    test_strict_trust_is_still_closed();
    test_the_absence_probes_can_detect_presence();
    test_a_runaway_is_killed();
    test_memory_is_capped();
    test_the_budget_hook_cannot_be_removed();
    test_bytecode_is_refused();
    test_string_metatable_is_locked();
    test_collectgarbage_is_the_hosts_job();
    test_the_jit_is_off_because_the_budget_requires_it();
    test_two_scripts_can_share_a_hook();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
