//
// Emscripten bindings: the client's window onto the simulation core.
//
// This is deliberately thin. The wasm module owns exactly the code that MUST
// produce the same answer as the server -- the simulation step -- and nothing
// else. Rendering, camera, UI and input capture stay in TypeScript, where a
// wrong last bit is a sub-pixel difference nobody can see.
//
// State crosses the boundary through caller-supplied buffers rather than as i64
// return values. That avoids depending on -sWASM_BIGINT for the ABI, and it
// keeps the raw Q32.32 values intact: JS reads them as BigInt via BigInt64Array,
// so the client's copy of the authoritative state is bit-exact. Rounding to a
// double here would silently break the exact-match reconciliation contract
// (docs/protocol.md R2/R3) at the very boundary it depends on.
//
#include <emscripten/emscripten.h>

#include "dnp/sim/world.hpp"

using namespace dnp;

namespace {

// Layout of the i64 buffer used by read/write_agent.
//   [0..2] pos.xyz   [3..5] vel.xyz   [6] agent hash
constexpr int BUF64_POS = 0;
constexpr int BUF64_VEL = 3;
constexpr int BUF64_HASH = 6;
constexpr int BUF64_LEN = 7;

// Layout of the u32 buffer.
//   [0] facing  [1] state_flags  [2] jumps_used  [3] jump_held  [4] active
constexpr int BUF32_FACING = 0;
constexpr int BUF32_FLAGS = 1;
constexpr int BUF32_JUMPS = 2;
constexpr int BUF32_JUMP_HELD = 3;
constexpr int BUF32_ACTIVE = 4;

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int dnp_buf64_len() { return BUF64_LEN; }

EMSCRIPTEN_KEEPALIVE
sim::World* dnp_world_create() { return new sim::World(); }

EMSCRIPTEN_KEEPALIVE
void dnp_world_destroy(sim::World* w) { delete w; }

EMSCRIPTEN_KEEPALIVE
void dnp_world_reset(sim::World* w) { w->reset(); }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_spawn(sim::World* w, const long long* pos64) {
    return w->spawn(fx3{fx::from_raw(pos64[0]), fx::from_raw(pos64[1]), fx::from_raw(pos64[2])});
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_set_input(sim::World* w, unsigned idx, unsigned buttons, int mx, int my, unsigned aim) {
    proto::InputFrame f;
    f.buttons = static_cast<u16>(buttons);
    f.move_x  = static_cast<i16>(mx);
    f.move_y  = static_cast<i16>(my);
    f.aim     = brad(aim);
    w->set_input(idx, f);
}

EMSCRIPTEN_KEEPALIVE
void dnp_world_step(sim::World* w) { w->step(); }

EMSCRIPTEN_KEEPALIVE
unsigned dnp_world_tick(sim::World* w) { return w->tick(); }

EMSCRIPTEN_KEEPALIVE
void dnp_world_read_agent(sim::World* w, unsigned idx, long long* out64, unsigned* out32) {
    const sim::AgentState& a = w->agent(idx);
    out64[BUF64_POS + 0] = a.pos.x.raw;
    out64[BUF64_POS + 1] = a.pos.y.raw;
    out64[BUF64_POS + 2] = a.pos.z.raw;
    out64[BUF64_VEL + 0] = a.vel.x.raw;
    out64[BUF64_VEL + 1] = a.vel.y.raw;
    out64[BUF64_VEL + 2] = a.vel.z.raw;
    out64[BUF64_HASH]    = static_cast<long long>(w->agent_hash(idx));
    out32[BUF32_FACING]    = a.facing.raw;
    out32[BUF32_FLAGS]     = a.state_flags;
    out32[BUF32_JUMPS]     = a.jumps_used;
    out32[BUF32_JUMP_HELD] = a.jump_held ? 1u : 0u;
    out32[BUF32_ACTIVE]    = a.active ? 1u : 0u;
}

// Adopt authoritative state. This is step 2 of reconciliation
// (docs/protocol.md 10): overwrite local state with the server's, then replay.
//
// jump_held is part of what gets adopted, and that is not incidental: it is
// simulation state, not input state, and a replay that started from the wrong
// edge-detect flag would re-trigger or swallow a jump and diverge from the
// server for reasons no amount of position correction would fix.
EMSCRIPTEN_KEEPALIVE
void dnp_world_write_agent(sim::World* w, unsigned idx, const long long* in64, const unsigned* in32) {
    sim::AgentState& a = w->agent_mut(idx);
    a.pos = fx3{fx::from_raw(in64[BUF64_POS + 0]), fx::from_raw(in64[BUF64_POS + 1]),
                fx::from_raw(in64[BUF64_POS + 2])};
    a.vel = fx3{fx::from_raw(in64[BUF64_VEL + 0]), fx::from_raw(in64[BUF64_VEL + 1]),
                fx::from_raw(in64[BUF64_VEL + 2])};
    a.facing      = brad(in32[BUF32_FACING]);
    a.state_flags = static_cast<u16>(in32[BUF32_FLAGS]);
    a.jumps_used  = static_cast<u8>(in32[BUF32_JUMPS]);
    a.jump_held   = in32[BUF32_JUMP_HELD] != 0;
    a.active      = in32[BUF32_ACTIVE] != 0;
}

// Input quantisation, exposed so the client uses the SAME function the server
// decodes with (protocol R1). Reimplementing it in JS would be one more place
// for the two sides to disagree, and the disagreement would be one ulp wide and
// present on every single tick.
//
// Takes the value through a buffer rather than as a `long long` parameter:
// without -sWASM_BIGINT an i64 argument is legalised into two i32s, and the
// resulting call signature is an ABI detail no caller should have to know.
EMSCRIPTEN_KEEPALIVE
int dnp_axis_from_buf(const long long* p) {
    return proto::axis_from_fx(fx::from_raw(p[0]));
}

}  // extern "C"
