#pragma once
//
// Placeholder simulation for the Phase 1 netcode harness.
//
// This is a trivial point-mass controller, not the real physics -- Phase 2
// replaces step_agent() with the actual one (step up/down, sliding, soft push,
// CCD, the movement abilities). It lives in core/ rather than server/ from the
// start for one reason: the client predicts by running THIS code, compiled to
// wasm, and getting the same answer. Putting the harness simulation anywhere
// else would mean writing prediction twice and discovering the divergence later.
//
// Everything here obeys the physics contract: integer-only, fixed timestep, no
// ambient inputs, deterministic iteration order.
//
#include "dnp/proto/messages.hpp"
#include "dnp/sim/constants.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

using namespace dnp;

inline constexpr u32 MAX_AGENTS = 64;

// The per-entity parameter table of design 7.2, in stub form.
//
// A fixed-schema struct, deliberately not a hash map: it has to be hashed every
// tick for the state hash, replicated, and bound to Lua, and a map's iteration
// order is not something the simulation may depend on.
struct AgentParams {
    fx  move_speed    = fx::from_int(8);
    fx  gravity_scale = fx::one();
    fx  jump_impulse  = fx::from_int(9);
    u8  max_jumps     = 2;
};

struct AgentState {
    fx3  pos{};
    fx3  vel{};
    brad facing{};
    u16  state_flags = 0;
    u8   jumps_used  = 0;
    bool active      = false;

    // Edge-detect for jump: holding the button must not re-trigger every tick.
    // Part of simulation state, not input state, because a prediction replay
    // has to reproduce it exactly.
    bool jump_held = false;
};

class World {
public:
    World() noexcept { reset(); }

    void reset() noexcept;

    // Returns MAX_AGENTS on failure. Index is stable for the agent's lifetime.
    u32  spawn(fx3 pos) noexcept;
    void despawn(u32 index) noexcept;

    // Input for the tick about to be stepped. An agent with no input this tick
    // keeps its previous frame -- the server must simulate SOMETHING, and
    // repeating the last frame is what the client's own replay assumes when it
    // predicts past the inputs the server has acknowledged.
    void set_input(u32 index, const proto::InputFrame& f) noexcept;

    void step() noexcept;

    u32 tick() const noexcept { return tick_; }
    u32 capacity() const noexcept { return MAX_AGENTS; }

    // Entity generation, incremented every time a slot is reused. This is what
    // makes protocol R8 real: without it a recycled index looks like the same
    // entity to a client, which inherits the previous occupant's interpolation
    // history, animation state -- and, once delta compression is on, its
    // replication baseline. The visible result is an enemy appearing to
    // teleport across the map at the moment it dies.
    u8 generation(u32 i) const noexcept { return generation_[i]; }

    const AgentState&  agent(u32 i) const noexcept { return agents_[i]; }
    AgentState&        agent_mut(u32 i) noexcept { return agents_[i]; }
    const AgentParams& params(u32 i) const noexcept { return params_[i]; }
    AgentParams&       params_mut(u32 i) noexcept { return params_[i]; }
    const proto::InputFrame& input(u32 i) const noexcept { return inputs_[i]; }

    // Full-world hash, for the determinism gate.
    u64 hash() const noexcept;

    // Per-agent hash, sent in SelfBlock.state_hash so the client can tell a
    // genuine correction from a determinism regression (protocol R3).
    u64 agent_hash(u32 i) const noexcept;

private:
    void step_agent(u32 i) noexcept;

    AgentState        agents_[MAX_AGENTS]{};
    u8                generation_[MAX_AGENTS]{};
    AgentParams       params_[MAX_AGENTS]{};
    proto::InputFrame inputs_[MAX_AGENTS]{};
    u32               tick_ = 0;
};

}  // namespace dnp::sim
