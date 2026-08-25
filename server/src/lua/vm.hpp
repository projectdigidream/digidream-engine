#pragma once
//
// The server's Lua runtime.
//
// Server-only, always. `core/` never sees this header, the wasm client never
// links it, and the determinism gate never measures a binary containing it --
// scripts/check_source_lists.sh and the nm check in scripts/check_determinism.sh
// enforce that mechanically rather than by convention, because the client
// predicts by running the same physics and Lua is the one thing it must not run.
//
// TRUST MODEL
// -----------
// The default profile is `Full`: scripts may reach the filesystem, databases and
// native libraries. The boundary is the container or VM the operator runs the
// server in, not an in-process allowlist. `Strict` keeps the locked-down
// environment for operators who load third-party scripts.
//
// What survives the permissive default, and why, because none of it is about
// security:
//
//   instruction budget  a runaway loop stalls the 60 Hz tick, main.cpp drops
//                       the missed ticks past 250 ms, and every client sees a
//                       burst of corrections. A Lua bug presenting as a netcode
//                       bug is the most expensive misdiagnosis available here.
//   memory cap          ("x"):rep(1e9) is ONE VM instruction, so the count hook
//                       never sees it. Only a counting allocator does.
//   host-driven GC      a collection landing on an arbitrary tick is the same
//                       stall by another route.
//   stall attribution   when the loop does fall behind, say which script and
//                       which callback, rather than leaving it to be guessed.
//
#include <cstddef>

#include "dnp/math/fixed.hpp"

struct lua_State;
struct lua_Debug;

namespace dnp::server::lua {

using namespace dnp;

enum class Trust : u8 {
    // Filesystem, databases, native libraries. The operator confines the
    // process; the interpreter does not confine the script.
    Full = 0,
    // The allowlist: no io, os, package/require, ffi, debug, or loaders.
    Strict = 1,
};

struct VmConfig {
    Trust  trust = Trust::Full;

    // Bytes. Refusing an allocation turns an OOM abort into an ordinary Lua
    // error the host can catch and report against the offending script.
    size_t memory_cap = 256u * 1024u * 1024u;

    // VM instructions before the hook fires within one callback invocation.
    u32 budget_per_call = 2000000;

    // ...and across every callback in one tick. A thousand individually cheap
    // callbacks blow the frame while every per-call check reports clean.
    u32 budget_per_tick = 8000000;

    // Microseconds of wall time in one tick past which Lua is named in the log.
    // Not a kill: I/O is allowed to be slow, but it must be attributable.
    u32 stall_warn_us = 4000;
};

struct VmStats {
    u64 callbacks_run = 0;
    u64 budget_overruns = 0;
    u64 callbacks_disabled = 0;
    u64 errors = 0;
    u64 alloc_refusals = 0;
    u64 stalls = 0;
    u64 peak_bytes = 0;
    u64 worst_tick_us = 0;
};

// NOTE: almost nothing here is noexcept, deliberately.
//
// LuaJIT raises errors by unwinding, on this platform through the C++ exception
// machinery. Marking a function that can call into Lua `noexcept` does not stop
// the throw -- it turns it into std::terminate. The fx binding was written that
// way first and died with "terminating due to uncaught foreign exception" the
// moment a script passed a bad argument, which in production is the server
// crashing on a script typo. close() keeps noexcept because lua_close is
// documented not to raise.
class Vm {
public:
    Vm() noexcept = default;
    ~Vm();
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    // Creates the state and installs the environment for `cfg.trust`.
    bool open(const VmConfig& cfg);
    void close() noexcept;

    bool is_open() const noexcept { return L_ != nullptr; }
    lua_State* state() const noexcept { return L_; }
    const VmConfig& config() const noexcept { return cfg_; }
    const VmStats& stats() const noexcept { return stats_; }

    // Load and run a script file. `name` is what appears in errors and in the
    // per-callback budget accounting.
    //
    // Chunks whose first byte is 0x1B are refused even under Trust::Full:
    // LuaJIT's bytecode loader is not memory-safe on malformed input, and the
    // failure is a crash inside the interpreter rather than a Lua error, which
    // no amount of operator confinement makes debuggable.
    bool run_file(const char* path, const char* name);

    // Called once per tick before any callback: resets the tick budget and
    // starts wall-clock attribution.
    void begin_tick();

    // Called once per tick after the last callback. Logs a stall if Lua spent
    // more than `stall_warn_us`, naming the script that was running.
    void end_tick(u32 tick);

    // Drive the collector a fixed amount per tick rather than letting it choose
    // its own moment.
    void gc_step();

    // Call a global function of no arguments, if it exists. Errors are caught,
    // counted and logged once -- a script that throws must not take the tick
    // with it, and must not be silently ignored either.
    void call_hook(const char* fn_name, u32 tick);

    const char* last_error() const noexcept { return err_; }

    // Which script is running, for log attribution.
    const char* current_script() const noexcept { return current_; }

private:
    static void* alloc_hook(void* ud, void* ptr, size_t osize, size_t nsize);
    static void count_hook(lua_State* L, lua_Debug* ar);

    void install_environment();

    lua_State* L_ = nullptr;
    VmConfig   cfg_{};
    VmStats    stats_{};
    char       err_[512]{};

    // Allocator accounting. Members rather than statics so a second Vm in a
    // test does not inherit the first one's usage.
    size_t bytes_ = 0;

    // Budget state, read by the hook.
    u32  used_this_tick_ = 0;
    u32  used_this_call_ = 0;
    bool killed_ = false;
    const char* current_ = "";

    u64 tick_start_us_ = 0;
};

}  // namespace dnp::server::lua
