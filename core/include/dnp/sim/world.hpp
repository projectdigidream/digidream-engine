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

// Everything in the world that is not a player.
//
// Keys, pickups, decorations, and eventually enemies. They share ONE index
// space with agents -- slots [0, MAX_AGENTS) are agents, the rest are props --
// because the wire already identifies an entity by `index + 1` and the client
// already keys its entity views by index. A separate handle domain would mean a
// bit on every handle and a branch at every use of one, to distinguish things
// the protocol never needed to distinguish.
inline constexpr u32 MAX_PROPS    = 192;

// The top of the prop range, which spawn_prop never allocates from.
//
// A client mirrors the OTHER BODIES into its own world as solid cylinders, so
// that its prediction stops where the server will stop it. Those bodies are
// agents on the wire, with indices in [0, MAX_AGENTS) -- which is a range this
// world already uses for something else, and the mirrors have to go somewhere
// that cannot collide with a real prop.
//
// Reserving the band here rather than agreeing a convention in the client is
// the difference between a guarantee and a hope: the server's allocator is what
// would collide, so the server's allocator is what has to know. It costs 64
// prop slots no map has come close to using.
inline constexpr u32 MIRROR_SLOTS   = MAX_AGENTS;
inline constexpr u32 SPAWNABLE_PROPS = MAX_PROPS - MIRROR_SLOTS;
inline constexpr u32 MAX_ENTITIES = MAX_AGENTS + MAX_PROPS;

// What a prop IS, as far as the simulation is concerned: a number the map and
// the scripts agree on. The engine attaches no meaning to it at all -- the
// CLIENT maps it to a sprite sheet or a mesh, and the SCRIPT maps it to
// behaviour. That is what lets the same infrastructure carry a Doom key and a
// monster without the engine learning what either one is.
using PropKind = u16;

namespace prop_flag {
// A body cannot walk through it. A barrel, a crate, a pillar -- the things a
// map wants in the world rather than in the geometry, because they move or
// disappear and a sector cannot do either cheaply.
inline constexpr u8 SOLID  = 1u << 0;
// Pressing use while facing it is reported. A lever, a terminal, a door you
// open by its handle rather than by its frame.
inline constexpr u8 USABLE = 1u << 1;
}  // namespace prop_flag

struct PropState {
    fx3  pos{};
    brad facing{};
    PropKind kind = 0;

    // Which animation the client should be playing. The server declares the
    // STATE and never the timing (design §4): a sprite renderer swaps frames on
    // its own clock and a mesh renderer cross-fades on its own, and neither
    // decision is something the simulation should be able to get wrong.
    u16  animation_state = 0;

    // The cylinder an agent has to be inside to touch it. Not a collision
    // volume -- nothing is pushed out of a prop -- it is the pickup radius, and
    // a key you have to stand exactly on is not a key anyone picks up.
    fx   radius = fx::from_frac(1, 2);
    fx   height = fx::from_int(1);

    // What it DOES, structurally: whether you can walk through it and whether
    // you can press it. Declared per kind in the catalogue, so a map author
    // adds a solid barrel by writing `solid` in a text file.
    u8   flags = 0;

    bool active = false;
};

// A transient visual the server asked for: a tracer, a muzzle flash, an impact.
//
// FIRE AND FORGET, off the entity replication path (docs/protocol.md §9.2).
// A tracer is not an entity -- it has no identity, nothing refers to it, and a
// client that missed one has missed nothing it needs to stay consistent. Making
// it an entity would mean spawning and destroying a thing sixty times a second
// to draw a line that lasts a tenth of a second.
//
// The engine attaches no meaning to `kind` or `param`; the catalogue and the
// renderer agree on what they mean, exactly as they do for a prop.
struct PendingVfx {
    u16  kind = 0;
    fx3  pos{};
    brad dir{};
    u32  param = 0;
};

inline constexpr u32 MAX_PENDING_VFX = 32;

// An agent swung at something and connected.
//
// The engine answers "what was in the arc"; whether that hurts, breaks, or does
// nothing at all is gameplay. Targets are ENTITY indices, so an agent and a prop
// are reported the same way and a script that damages both writes it once.
struct AttackEvent {
    u32 attacker = 0;
    u32 target = 0;      // an entity index: an agent below MAX_AGENTS, else a prop
};

inline constexpr u32 MAX_ATTACK_EVENTS = 64;

class AttackLog {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }
    void push(const AttackEvent& e) noexcept {
        if (count_ >= MAX_ATTACK_EVENTS) { ++dropped_; return; }
        events_[count_++] = e;
    }
    u32 count() const noexcept { return count_; }
    const AttackEvent& at(u32 i) const noexcept { return events_[i]; }
    u32 dropped() const noexcept { return dropped_; }
    void hash_into(StateHasher& h) const noexcept;

private:
    AttackEvent events_[MAX_ATTACK_EVENTS]{};
    u32 count_ = 0;
    u32 dropped_ = 0;
};

enum class PropEventKind : u8 {
    // An agent is standing inside the prop's cylinder. Reported every tick it
    // holds -- see detect_prop_events for why that is not an edge trigger.
    Touch = 0,
    // An agent pressed use while facing it, on the press edge.
    Use = 1,
};

// Something happened between an agent and a prop.
//
// Reported, not acted on -- the same division crushing and interaction draw.
// What a key does when touched, or a lever when pulled, is gameplay, and
// gameplay is script.
struct PropEvent {
    u32 agent = 0;
    u32 prop  = 0;      // an ENTITY index, so >= MAX_AGENTS
    PropEventKind kind = PropEventKind::Touch;
};

inline constexpr u32 MAX_PROP_EVENTS = 64;

class PropEventLog {
public:
    void clear() noexcept { count_ = 0; dropped_ = 0; }
    void push(const PropEvent& e) noexcept {
        if (count_ >= MAX_PROP_EVENTS) { ++dropped_; return; }
        events_[count_++] = e;
    }
    u32 count() const noexcept { return count_; }
    const PropEvent& at(u32 i) const noexcept { return events_[i]; }
    u32 dropped() const noexcept { return dropped_; }
    void hash_into(StateHasher& h) const noexcept;

private:
    PropEvent events_[MAX_PROP_EVENTS]{};
    u32 count_ = 0;
    u32 dropped_ = 0;
};

struct AgentState {
    fx3  pos{};
    fx3  vel{};
    brad facing{};
    bool active = false;

    // What this agent IS, for anything that is not a player.
    //
    // Zero means a player, which the client draws as a body. Anything else is a
    // catalogue kind and is drawn from the same sprite records a prop uses --
    // so an enemy needs no second rendering path, and a prop that grows legs
    // needs no new one either.
    //
    // An enemy is an AGENT and not a prop because it MOVES: it walks, falls,
    // steps up, slides along walls and is stopped by doors, and all of that
    // already exists in the controller a player runs. Giving props a mover
    // instead would be a second implementation of the thing the physics
    // contract exists to keep singular.
    PropKind kind = 0;

    // Which animation the client should play. The server declares the STATE and
    // never the timing, exactly as for props (design §4).
    u16  animation_state = 0;

    // Set by a script for an agent it drives. A player's input arrives from its
    // session; an enemy's is written here, so both run the same controller and
    // an enemy cannot do anything a player could not.
    bool scripted = false;

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

    // An agent nobody is sitting at: an enemy, driven by a script through
    // set_input. Returns MAX_AGENTS when full.
    u32  spawn_scripted(PropKind kind, fx3 pos) noexcept;

    // Props, addressed by ENTITY index (>= MAX_AGENTS). Returns MAX_ENTITIES
    // when the world is full.
    u32  spawn_prop(PropKind kind, fx3 pos) noexcept;
    void despawn_prop(u32 entity) noexcept;

    // Put a prop in a SPECIFIC entity slot, with its physical facts supplied.
    //
    // For a client mirroring what the server replicated. A solid prop is
    // physics -- the mover pushes bodies out of it -- so a client that did not
    // have one would walk through what the server stops it against and
    // mispredict every tick it was touching. Which slot matters: the entity
    // index is the identity on the wire, and a client that put the same barrel
    // somewhere else would be simulating a different world.
    //
    // The radius, height and flags come from the catalogue, which both sides
    // read; the engine does not know them and does not look them up.
    bool place_prop(u32 entity, PropKind kind, fx3 pos,
                    fx radius, fx height, u8 flags) noexcept;
    static bool is_prop(u32 entity) noexcept {
        return entity >= MAX_AGENTS && entity < MAX_ENTITIES;
    }
    const PropState& prop(u32 entity) const noexcept { return props_[entity - MAX_AGENTS]; }
    PropState&       prop_mut(u32 entity) noexcept { return props_[entity - MAX_AGENTS]; }

    // What happened between agents and props this tick.
    const PropEventLog& prop_events() const noexcept { return prop_events_; }

    // Bumped whenever the set of props changes: one appears, one goes, or one
    // is moved by something other than its own simulation.
    //
    // The server uses it to label the snapshot, because a prop appearing or
    // vanishing is a scripted change the client could not have predicted -- the
    // same category as a door opening, and it needs the same flag or it reads
    // as a determinism fault every time a barrel explodes under someone.
    u32 prop_epoch() const noexcept { return prop_epoch_; }

    // Everything an agent's swing connected with this tick.
    const AttackLog& attacks() const noexcept { return attacks_; }

    // Transient visuals queued this tick, drained by the server into snapshots.
    //
    // NOT hashed and not part of the simulation: a tracer changes nothing, and
    // a client that draws one has not diverged from a client that dropped it.
    // Cleared every tick, so a dropped datagram loses the effect rather than
    // replaying it late.
    // Emptied by the HOST at the start of its tick, not by step().
    //
    // Everything else step() clears is something step() FILLS -- crushes,
    // interactions, attacks. The transient queue is filled by SCRIPT, which
    // runs before the step, so clearing it in step() threw away every effect
    // queued that tick before anything could send it. A queue is cleared by
    // whoever writes to it.
    void clear_vfx() noexcept { vfx_count_ = 0; }
    u32 vfx_count() const noexcept { return vfx_count_; }
    const PendingVfx& vfx(u32 i) const noexcept { return vfx_[i]; }
    bool push_vfx(const PendingVfx& v) noexcept {
        if (vfx_count_ >= MAX_PENDING_VFX) return false;
        vfx_[vfx_count_++] = v;
        return true;
    }

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

    // Entities that reached for a side this tick (Doom's use key). Rebuilt every
    // step like the crush log; the server drains it and hands it to Lua.
    const InteractLog& interactions() const noexcept { return interacts_; }

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
    void detect_prop_events() noexcept;
    void resolve_prop_collisions(u32 agent) noexcept;
    // `was` is every agent's position at the start of the tick -- see the
    // definition for why the resolution reads that rather than live positions.
    void resolve_agent_collisions(const fx3* was) noexcept;
    i32  find_usable_prop(u32 agent) const noexcept;
    void resolve_attack(u32 agent) noexcept;
    void report_crossings(u32 i, fx2 from, fx2 to, i32 from_sector) noexcept;

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
    InteractLog          interacts_{};

    AgentState           agents_[MAX_AGENTS]{};
    PropState            props_[MAX_PROPS]{};
    PropEventLog         prop_events_{};
    u32                  prop_epoch_ = 0;
    AttackLog            attacks_{};
    PendingVfx           vfx_[MAX_PENDING_VFX]{};
    u32                  vfx_count_ = 0;
    // One generation table over the whole index space, so a handle to slot 70
    // is validated exactly the way a handle to slot 3 is.
    u8                   generation_[MAX_ENTITIES]{};
    physics::MoveParams  params_[MAX_AGENTS]{};
    proto::InputFrame    inputs_[MAX_AGENTS]{};
    u32                  tick_ = 0;
};

}  // namespace dnp::sim
