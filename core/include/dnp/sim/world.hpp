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
#include "dnp/sim/crush.hpp"
#include "dnp/sim/crush.hpp"
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

    // --- invariant-preserving mutators -------------------------------------
    //
    // The ONLY way anything outside the simulation may move an entity. They
    // exist because `agent_mut(i).pos = p` is not a teleport: it leaves
    // mv.sector naming the sector the entity used to be in, so the next tick
    // resolves collision against the wrong geometry, and it leaves the entity
    // anchored to a platform it is no longer standing on. Both failures present
    // as physics bugs somewhere else entirely.
    //
    // Keeping them in core/ rather than in the binding layer means the script
    // API physically cannot leave the world in a shape the engine would not
    // produce itself -- and it keeps them integer-only, under the no-floats
    // guard, where every other rule of the simulation lives.

    // Move an entity, re-deriving what its new position implies.
    void teleport(u32 index, fx3 pos) noexcept;

    // Add to velocity. RELATIVE on purpose: two effects that each want to push
    // an entity this tick should both push it, where two absolute writes would
    // silently mean "whichever ran last".
    void add_impulse(u32 index, fx3 dv) noexcept;

    // Absolute, for the cases where last-write-wins is the correct semantic.
    void set_velocity(u32 index, fx3 v) noexcept;

    // Crush and pin events raised by the tick that just ran (design §3.1,
    // §3.1.3). Rebuilt from scratch every step: this is what happened THIS
    // tick, not a backlog, so a caller that does not drain it loses it rather
    // than accumulating a queue nobody reads. The server hands these to Lua;
    // the client computes the identical list and ignores it, because the
    // consequence is Lua's and Lua is not predicted.
    const CrushLog& crushes() const noexcept { return crushes_; }

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

    // The frame positions are hashed in: the offset of an unpredictable anchor,
    // or zero. See the definition -- this is design §3.2.5's fallback.
    fx3 anchor_frame(const physics::MoveState& m) const noexcept;

    // Carry entities riding a sector that moved, then let moving footprints
    // shove the ones they ran into (design §3.1.3). Both happen after the
    // geometry has moved and before any agent runs its own physics.
    void carry_anchored() noexcept;
    void sweep_moving_sectors() noexcept;
    void sweep_agent(u32 i, const SectorMotion& m) noexcept;

    // Geometry closed on a body that no longer fits between its surfaces.
    void detect_vertical_crush() noexcept;

    const geom::MapData* map_ = nullptr;
    geom::MapStorage*    store_ = nullptr;
    SectorMotionSet      motions_{};
    CrushLog             crushes_{};

    AgentState           agents_[MAX_AGENTS]{};
    u8                   generation_[MAX_AGENTS]{};
    physics::MoveParams  params_[MAX_AGENTS]{};
    proto::InputFrame    inputs_[MAX_AGENTS]{};
    u32                  tick_ = 0;
};

}  // namespace dnp::sim
