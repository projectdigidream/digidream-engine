#pragma once
//
// The simulation world.
//
// It lives in core/ rather than server/ because the client predicts by running
// THIS code, compiled to wasm, and getting the same answer. Putting it anywhere
// else would mean writing prediction twice and discovering the divergence later.
//
// Everything here obeys the physics contract: integer-only, fixed timestep, no
// ambient inputs, deterministic iteration order.
//
#include "dnp/geom/map.hpp"
#include "dnp/physics/controller.hpp"
#include "dnp/proto/messages.hpp"
#include "dnp/sim/constants.hpp"
#include "dnp/sim/sectormotion.hpp"
#include "dnp/sim/state_hash.hpp"

namespace dnp::sim {

using namespace dnp;

inline constexpr u32 MAX_AGENTS = 64;

struct AgentState {
    fx3  pos{};
    fx3  vel{};
    brad facing{};
    bool active = false;

    physics::MoveState mv{};
};

class World {
public:
    World() noexcept { reset(); }

    void reset() noexcept;

    // The map every agent collides against. A null map means no geometry, in
    // which case agents free-fall forever -- deliberately not a hidden flat
    // floor, because a silently-substituted floor is how a map-loading failure
    // turns into a physics mystery.
    void set_map(const geom::MapData* map) noexcept { map_ = map; }
    const geom::MapData* map() const noexcept { return map_; }

    // Attach mutable map storage, which sector motion needs in order to write
    // the moved geometry back. Without it the world is read-only and no sector
    // can move.
    void set_map_storage(geom::MapStorage* store) noexcept { store_ = store; }
    SectorMotionSet& motions() noexcept { return motions_; }
    const SectorMotionSet& motions() const noexcept { return motions_; }

    u32  spawn(fx3 pos) noexcept;
    void despawn(u32 index) noexcept;

    // Input for the tick about to be stepped. An agent with no input this tick
    // keeps its previous frame -- the server must simulate SOMETHING, and
    // repeating the last frame is what the client's own replay assumes when it
    // predicts past the inputs the server has acknowledged.
    void set_input(u32 index, const proto::InputFrame& f) noexcept;

    void step() noexcept;

    u32 tick() const noexcept { return tick_; }

    // Align the simulation clock to the server's.
    //
    // This matters the moment any sector moves. Parametric motion is a pure
    // function of the TICK, so a client evaluating it against its own
    // zero-based counter computes the platform's position for the wrong moment
    // in the schedule -- and the disagreement grows with the offset between the
    // two clocks. Before sector motion existed the tick only fed the hash and
    // nothing depended on the two sides agreeing about its value.
    void set_tick(u32 t) noexcept { tick_ = t; }
    u32 capacity() const noexcept { return MAX_AGENTS; }

    // Entity generation, incremented every time a slot is reused. This is what
    // makes protocol R8 real: without it a recycled index looks like the same
    // entity to a client, which inherits the previous occupant's interpolation
    // history, animation state and replication baseline. The visible result is
    // an enemy appearing to teleport across the map at the moment it dies.
    u8 generation(u32 i) const noexcept { return generation_[i]; }

    const AgentState&           agent(u32 i) const noexcept { return agents_[i]; }
    AgentState&                 agent_mut(u32 i) noexcept { return agents_[i]; }
    const physics::MoveParams&  params(u32 i) const noexcept { return params_[i]; }
    physics::MoveParams&        params_mut(u32 i) noexcept { return params_[i]; }
    const proto::InputFrame&    input(u32 i) const noexcept { return inputs_[i]; }

    // Wire-format state flags for an agent, assembled in one place so the
    // server and the wasm client cannot disagree about what "grounded" means.
    u16 state_flags(u32 i) const noexcept;

    // Full-world hash, for the determinism gate.
    u64 hash() const noexcept;

    // Per-agent hash, sent in SelfBlock.state_hash so the client can tell a
    // genuine correction from a determinism regression (protocol R3).
    u64 agent_hash(u32 i) const noexcept;

private:
    void step_agent(u32 i) noexcept;

    const geom::MapData* map_ = nullptr;
    geom::MapStorage*    store_ = nullptr;
    SectorMotionSet      motions_{};

    AgentState           agents_[MAX_AGENTS]{};
    u8                   generation_[MAX_AGENTS]{};
    physics::MoveParams  params_[MAX_AGENTS]{};
    proto::InputFrame    inputs_[MAX_AGENTS]{};
    u32                  tick_ = 0;
};

}  // namespace dnp::sim
