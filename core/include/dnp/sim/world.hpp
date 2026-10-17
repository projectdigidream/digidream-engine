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
#include "dnp/sim/weapon.hpp"

namespace dnp::sim {

using namespace dnp;

// MAX_AGENTS lives in sim/constants.hpp, because the physics layer needs it for
// the swing's per-agent hit mask and cannot include this header.

// Everything in the world that is not a player.
//
// Keys, pickups, decorations, and eventually enemies. They share ONE index
// space with agents -- slots [0, MAX_AGENTS) are agents, the rest are props --
// because the wire already identifies an entity by `index + 1` and the client
// already keys its entity views by index. A separate handle domain would mean a
// bit on every handle and a branch at every use of one, to distinguish things
// the protocol never needed to distinguish.
// Raised alongside MAX_AGENTS, because the mirror band below is carved out of
// this range and a band the size of the agent space would otherwise leave
// nothing to spawn into.
//
// 2048 mirror slots plus 384 spawnable, which is the same number of spawnable
// props the 768/384 split gave -- the raises are for agents, and quietly halving
// what a map may place would be a different change wearing this one's clothes.
// SPAWNABLE_PROPS is checked below rather than left to underflow: it is
// unsigned, so MAX_PROPS < MIRROR_SLOTS would wrap to four billion and the
// allocator would hand out prop indices on top of the mirror band.
// 4096 SPAWNABLE: a region holds several DOOM levels in one world, and at 384
// doomworld's own levels could not place their items -- "PROP TABLE FULL" for
// half of them, and every monster's drop silently gone with them.
inline constexpr u32 MAX_PROPS    = 2048 + 4096;

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
static_assert(MAX_PROPS > MIRROR_SLOTS,
              "the mirror band fills the prop range: SPAWNABLE_PROPS would "
              "underflow and spawn_prop would hand out indices inside it");
inline constexpr u32 SPAWNABLE_PROPS = MAX_PROPS - MIRROR_SLOTS;
static_assert(ATTACK_HIT_PROP_WORDS * 64 >= SPAWNABLE_PROPS,
              "a swing's prop hit mask must have a bit for every spawnable prop slot");
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
    // Not sent to this agent (its index + 1; 0 sends to everyone): one its own
    // client already drew from its predicted weapon (docs/WEAPON_PREDICTION.md).
    u32  skip_for = 0;
};

// What one TICK may queue, and it matches proto::MAX_VFX -- what one snapshot
// may carry -- deliberately.
//
// The two have to agree, because they fail differently: the writer's overflow
// is logged ("N vfx dropped (budget)") and the engine's is a silent `false`
// nobody checks. An engine smaller than the wire throws events away before
// anything can report it.
//
// 32 stopped being enough the moment every hit also drew blood. A super shotgun
// is twenty pellets, and twenty tracers beside twenty sprays is forty events
// from one press -- so nine of them vanished, and which nine depended on the
// order the script happened to emit in. At 34 bytes apiece the whole buffer is
// two kilobytes; the cap is here to bound a burst, not to save memory.
inline constexpr u32 MAX_PENDING_VFX = 64;

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

// Room for a crowd: on DOOMWORLD a hundred monsters stand on items every tick,
// and at 64 the log was full before a player's own touch was reached -- a
// pickup that silently did not happen.
inline constexpr u32 MAX_PROP_EVENTS = 1024;

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
    // Where the body looks vertically, in 1/65536 of a turn, from this tick's
    // input (proto::InputFrame::pitch, clamped). NOT HASHED and not in a
    // snapshot: it is a function of the input alone, every tick, so both sides
    // already agree on it -- and nothing in the simulation reads it. Scripts do
    // (e:pitch(), a weapon's aim), and the renderer does (the view).
    i16  pitch = 0;
    bool active = false;

    // Whether other bodies collide with this one.
    //
    // True for anything alive. A script turns it off for a corpse: Doom's dead
    // do not block, and a body left solid in a doorway is a wall that appeared
    // where you were about to run. Simulation state and hashed, like everything
    // a client has to predict against -- a client that thought a corpse was
    // solid would be stopped by something the server walked through.
    bool solid = true;

    // Not stepped: no input, no physics, where it stands. Set by the server
    // for bodies in parts of the world nobody is near (a chunk with no player
    // within a cell), so a world holding several levels pays for the ones
    // somebody is in -- what an empty level not ticking used to buy.
    //
    // Still SOLID if it was: asleep is "not simulated", not "not there".
    // Server-side only; no client predicts a body that is not its own. Hashed
    // by World::hash for the gate and deliberately NOT by agent_hash.
    bool asleep = false;

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

    // A sector's NAME (geom::MapStorage::sector_name): what identifies it to a
    // holder of a different set of chunks. The index itself when no storage is
    // attached, and for a plain one-map store, so nothing that predates chunks
    // changes. NO_SECTOR stays -1.
    i32 sector_name(i32 sector) const noexcept {
        if (!store_ || sector < 0) return sector;
        const u32 n = store_->sector_name(sector);
        return n == geom::NO_NAME ? -1 : static_cast<i32>(n);
    }
    i32 sector_by_name(i32 name) const noexcept {
        if (!store_ || name < 0) return name;
        return store_->sector_by_name(static_cast<u32>(name));
    }
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

    /**
     * Advance the clock and the map, and nothing else.
     *
     * For a level nobody is in. Sector motion still runs -- it is what keeps a
     * lift where the arriving player's client will compute it to be -- but no
     * agent thinks, moves or collides, which is where essentially all of an
     * idle level's cost is.
     *
     * THE CLOCK STILL ADVANCES, and that is the point. Skipping the tick
     * entirely was tried and it broke player input: every level here has its
     * own clock, so a frozen level is thousands of ticks behind a busy one, and
     * a client crossing between them jumps its clock backwards by that much.
     * The server's input ring is seeded by whichever frame arrives first, so
     * one straggler stamped on the old clock made every subsequent real frame
     * look far too old to use -- the player went deaf and kept walking in the
     * direction they entered with. Keeping every level on the same count costs
     * nothing and removes that whole class of problem.
     */
    void step_idle() noexcept;

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

    // How many times this body has been teleported, mod 256: what a snapshot
    // compares against its baseline to tell a jump from a fast move
    // (ent_field::TELEPORTED), so a client draws it as one rather than a slide.
    // NOT in either hash: nothing simulated reads it, and a client's world never
    // teleports anything.
    u8 teleports(u32 index) const noexcept { return index < MAX_AGENTS ? teleports_[index] : 0; }

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

    // WHAT DOES A MOVING THING OF THIS SIZE REACH FIRST?
    //
    // A swept sphere along a segment, against entity cylinders AND the map, in
    // one answer -- because "did it hit a wall" and "did it hit somebody" asked
    // separately give a missile that bursts on a monster standing behind a
    // door. Whichever is nearer wins, and the caller gets the point.
    //
    // A QUERY: it reads the world and changes nothing, so it is not in the
    // hash and adding it does not move the determinism gate. That is worth
    // asserting rather than assuming, and core/tests/test_sweep.cpp does.
    //
    // TWO things to ignore, not one, and that is not an indulgence: a missile
    // must skip ITSELF -- it is an entity, sitting exactly at `from`, so a
    // single-ignore version stops at distance zero on the tick it is fired --
    // and it must skip the mob that fired it, which at launch is standing in
    // the same place. Either alone leaves the other. Pass -1 for unused.
    //
    // `sector` is where the ray starts, for the geometry cast; -1 means look
    // it up.
    //
    // Written because dnp.sweep answers geometry only, so the mob runtime was
    // walking every entity in Lua for every missile every tick. This is the
    // same scan with the cheap rejects the swing already uses, in the language
    // the positions are already in.
    struct SweepHit {
        bool hit = false;         // did it reach anything at all
        bool entity_hit = false;  // ... and was it an entity rather than a wall
        u32  entity = 0;          // index in the one entity index space
        fx3  point{};             // where it stopped
        fx   distance{};          // how far along `dir` that was
        // WHAT IT MET, when the map stopped it: a wall's normal (unit, out of
        // the sector it was in), or `plane` -1 for a floor and +1 for a
        // ceiling, with no normal. What a bouncing missile reflects off.
        fx2  normal{};
        i8   plane = 0;
    };
    // `slope`, when given, is a MISSILE's: it climbs `slope` per unit along
    // `dir`, and the map stops it where that line meets a step, a ceiling or
    // the floor -- `point.z` is then where it met it. Left out, the sweep is
    // flat and only walls stop it, as for every caller before missiles fell.
    SweepHit sweep_entities(fx3 from, fx2 dir, fx dist, fx radius,
                            i32 ignore_a, i32 ignore_b, i32 sector,
                            fx slope = physics::RAY_NO_HEIGHT) const noexcept;

    u32 tick() const noexcept { return tick_; }

    // A DIAGNOSTIC SEAM: called between the stages of step() with the stage
    // just finished (0 = start, then 1..8). The core has no clock and keeps
    // none; a host that wants to know where a slow step went times these.
    // Null is the default and costs a comparison per stage.
    using PhaseHook = void (*)(void* ctx, u32 phase);
    void set_phase_hook(PhaseHook hook, void* ctx) noexcept { phase_hook_ = hook; phase_ctx_ = ctx; }

    // Last tick's agent-vs-agent resolution: how many bodies the broadphase
    // served, and how many fell back to the full sweep. A report, not state --
    // neither is hashed, and neither affects the answer.
    u32 broadphase_fast() const noexcept { return agrid_fast_; }
    u32 broadphase_full() const noexcept { return agrid_full_; }

    // Test-only. See the comment on agrid_off_: the broadphase claims to be
    // indistinguishable from the sweep it replaces, and this is how a test runs
    // the other side of that comparison.
    void force_full_agent_sweep(bool on) noexcept { agrid_off_ = on; }

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

    // One past the highest agent slot ever used, for callers OUTSIDE core that
    // scan the agent space every tick.
    //
    // The server's Lua bindings do exactly that -- dnp.players() and
    // dnp.entities() walk it once a tick apiece -- and an AgentState is several
    // hundred bytes, so a full sweep of MAX_AGENTS touches hundreds of
    // kilobytes to find one player. That was tolerable at 384 slots and is not
    // at 1024.
    //
    // Safe for the same reason it is inside core: slots at or above the mark
    // have NEVER been active, and every such loop already skips `!active`. It
    // is a bound on the search, never on the answer. It does not shrink, and
    // hash() deliberately does not use it -- see agent_high_water_.
    u32 agent_high_water() const noexcept { return agent_high_water_; }

    // The same, for the prop half of the entity index space. A prop loop runs
    // from MAX_AGENTS to this rather than to MAX_ENTITIES.
    u32 prop_high_water() const noexcept { return MAX_AGENTS + prop_high_water_; }

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
    // The input AS THE RULES LET IT ACT: a body whose input_scale is 0 -- a
    // cinematic took its controls -- presses nothing and chooses no weapon.
    // Jump, dash, climb, grapple, use and both triggers read buttons, not the
    // stick input_scale scales, so without this a frozen player still jumped
    // and fired. The module's own keys (`actions`) pass: a graph's "skip the
    // cutscene" key still works. Shared, so the predicting client agrees.
    proto::InputFrame live_input(u32 i) const noexcept {
        proto::InputFrame f = inputs_[i];
        if (params_[i].input_scale == fx::zero()) { f.buttons = 0; f.weapon = NO_WEAPON; }
        return f;
    }

    // A player's weapon (docs/WEAPON_PREDICTION.md): the machine's state,
    // beside the body it fires from. Reset with the slot.
    const WeaponState& weapon(u32 i) const noexcept { return weapons_[i]; }
    WeaponState&       weapon_mut(u32 i) noexcept { return weapons_[i]; }

    // ONE TICK OF AGENT i's WEAPON, the one function both sides call: the
    // server from the weapon runtime's tick, a predicting client at the same
    // point of its own tick. Reads the body and this tick's buttons, writes
    // the recoil straight into the velocity and a set_anim into the
    // animation state, and pushes what the world must do to `log`. Returns
    // the phase changes made, which past WEAPON_HOPS means a loop was stopped.
    //
    // `stream` is whose random stream the spread is drawn from: the agent's
    // index ON THE SERVER, which a client's own world -- where the same body
    // sits at an index of its own -- has to be told. Defaults to `i`.
    u32 step_weapon(u32 i, const WeaponTable& table, u8 want, u32 tick, u64 seed,
                    WeaponEventLog& log, u32 stream = MAX_AGENTS) noexcept;

    // THE SHAPE OF AGENT i's SWING, as its collider sweeps: the attack_*
    // parameters, with the fields its running swing's own step gave laid over
    // them (a `swing` step with arguments, docs/SWINGS.md). What the sweep
    // reads every tick, and what a client draws its own swing with.
    struct SwingShape {
        fx reach{}, half_arc{}, sweep_from{}, sweep_to{}, min_reach{};
        fx pitch_from{}, pitch_to{}, half_pitch{};
        u8 ticks = 0, cooldown = 0;
    };
    SwingShape swing_shape(u32 i) const noexcept;

    // START A SWING, in the parameters' shape: a monster's `swing`,
    // a script's, a weapon's run in Lua. A weapon's own `swing` step comes the
    // same way from step_weapon. Taken by the next step ahead of the attack
    // button, by the swing step's rule (docs/SWINGS.md): a swing still
    // sweeping is REPLACED -- it stops where it is and the new one starts
    // from its own beginning, with a fresh list of who it has hit -- and one
    // asked for while the last one's cooldown runs does not happen. False for
    // an agent that is not there.
    bool request_swing(u32 i) noexcept;

    // FOR A PREDICTING CLIENT that has just adopted the server's weapon state:
    // the running swing's own shape again, from the step the state names.
    void adopt_swing(u32 i, const WeaponTable& table) noexcept;

    // A SHOT from agent i's middle, along `aim` (a heading) rising `slope` per
    // unit of run, out to `range`: the nearest body -- an agent other than i,
    // or a prop -- by the swing's cylinder test, unless the map stops it
    // first. dnp.hitscan's whole answer, here so a predicting client can ask
    // it of its own world, where the other bodies are mirrored props.
    struct ShotHit {
        i32 entity = -1;     // what it reached, -1 for nothing or a wall
        fx3 from{};          // the muzzle: the body's middle
        fx3 at{};            // where the ray stopped
    };
    ShotHit hitscan(u32 i, brad aim, fx slope, fx range) const noexcept;

    // AUTO-AIM (mobs.lua's M.autoaim, which a player's shot uses): the nearest
    // body inside `cone` brad either side of agent i's facing, within `range`
    // and in sight. On the server the bodies are the solid agents; on a client
    // they are the mirrored bodies (props in the mirror band), so both ask the
    // same question of what each can see. False when nothing qualifies.
    bool autoaim(u32 i, fx range, u32 cone, brad& heading, fx& slope, bool& slope_set) const noexcept;

    // LAG COMPENSATION (docs/WEAPON_PREDICTION.md, phase E): where the agents
    // were when a shooter saw them. While set, hitscan and autoaim test agent
    // j < count at pos[j] (skipping it if !live[j], a body that was not there
    // then) instead of where it is now. Nothing else reads these: the world is
    // never rewound, only these two questions. The caller's arrays; clear with
    // nulls when the shot is done.
    void set_query_poses(const fx3* pos, const u8* live, u32 count) noexcept {
        query_pos_ = pos;
        query_live_ = live;
        query_count_ = count;
    }

    // FOR A PREDICTING CLIENT: run agent `agent`'s weapon inside step(), with
    // this table, at the point the server runs it, drawing from the stream of
    // the server's agent `stream`. Null (the default, and the server's) runs
    // nothing. The table is the caller's and must outlive this.
    void predict_weapon(const WeaponTable* table, u64 seed, u32 agent, u32 stream) noexcept {
        weapon_table_ = table;
        weapon_seed_ = seed;
        weapon_agent_ = agent;
        weapon_stream_ = stream;
    }
    // What the predicted weapon asked for on the last step: tracers, flashes,
    // sounds to draw at once.
    const WeaponEventLog& weapon_events() const noexcept { return weapon_events_; }
    // And the effects it would have been sent for it, in the snapshot's own
    // terms -- a tracer per pellet, from World::hitscan in this world, and each
    // `vfx` step -- so the client draws its own shot through the same code that
    // draws everybody else's.
    u32 weapon_vfx_count() const noexcept { return weapon_vfx_count_; }
    const PendingVfx& weapon_vfx(u32 k) const noexcept { return weapon_vfx_[k]; }
    // And each missile it launched, where the server's launch() puts it: for a
    // client to fly its own rocket from the tick it fired (WMissile says how).
    struct WeaponLaunch {
        u8   weapon = 0, phase = 0, step = 0;
        fx3  pos{};
        fx2  dir{};
        brad aim{};
        fx   slope{};
    };
    u32 weapon_launch_count() const noexcept { return weapon_launch_count_; }
    const WeaponLaunch& weapon_launch(u32 k) const noexcept { return weapon_launches_[k]; }

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
    void resolve_agent_collisions() noexcept;
    void resolve_agent_against_all(u32 j, fx budget) noexcept;

    // Undo, or resolve, a push that put a body somewhere the map does not
    // allow. `before` is where it was before the push.
    void keep_in_map(u32 i, fx2 before) noexcept;

    // Every agent's position at the START of the current tick, before anything
    // moved. Two things read it, for two reasons that turn out to be the same
    // reason: agent-vs-agent resolution reads it so the answer does not depend
    // on which agent stepped first, and both resolutions read it to know how
    // far a body has travelled -- which is the budget for how far it may be
    // pushed. Not hashed; it is scratch, rebuilt every tick.
    fx3 tick_start_pos_[MAX_AGENTS]{};

    // --- the agent broadphase --------------------------------------------------
    //
    // resolve_agent_collisions was a flat double loop with no spatial index at
    // all: every solid agent tested against every other solid agent, every
    // tick. At 384 slots that is 147,000 pair tests; at 1024 it is a million,
    // and measured on tools/mobperf/run.js it was 68% of per-agent physics at
    // a thousand bodies.
    //
    // A UNIFORM HASH GRID over the TICK-START positions, rebuilt each tick.
    // Tick-start rather than current, because `tick_start_pos_` is what the
    // resolution reads for the other body and it does not move while the
    // resolution runs -- so the grid cannot go stale underneath the loop that
    // is querying it.
    //
    // WHY A HASH AND NOT A DENSE GRID. A dense grid has to be sized from the
    // map's bounds, which means either an allocation or a cap on how big a map
    // may be. A hash needs neither and costs one multiply per lookup. Bucket
    // collisions are harmless here by construction: a collision only adds
    // candidates, and a candidate that is not actually touching is a
    // push_out_of_cylinder that does nothing.
    //
    // THE ORDER IS THE CONTRACT. The inner loop mutates `b.pos` as it goes, so
    // the sequence of pushes is part of the answer -- two bodies resolved in
    // the other order end up somewhere else. Candidates are therefore sorted
    // back into ascending agent index before they are applied, which is what
    // makes this a pure acceleration: the same pairs, in the same order, with
    // the no-ops skipped. That is why the determinism hash must not move, and
    // it is the sharpest control in this piece of work.
    //
    // AND THE QUERY RADIUS IS CHECKED, NOT ASSUMED. Because `b.pos` moves
    // while the loop runs, the radius that was wide enough when the query was
    // made can stop being wide enough part way through -- a chain of pushes
    // through a crowd carries a body further than any single push. The
    // resolution therefore re-checks after every push that the body is still
    // inside the margin the query was made with, and if it is not, it throws
    // away what it did and runs the full sweep for that body. The fast path
    // never produces an answer it cannot justify.
    static constexpr u32 AGRID_BUCKETS = 2048;              // power of two
    static constexpr u32 AGRID_EMPTY   = 0xFFFFu;
    static_assert(MAX_AGENTS < AGRID_EMPTY,
                  "agent indices are stored as u16 in the broadphase chains, "
                  "with 0xFFFF reserved for the end of a chain");
    // Two world units. Bodies are about 0.4 in radius, so a cell holds a
    // handful of them and a query touches a few. Expressed as a shift on the
    // fixed-point raw value -- 1 unit is 2^32, so 2 units is 2^33 -- because an
    // arithmetic shift floors towards negative infinity on every target, which
    // a divide does not.
    static constexpr u32 AGRID_CELL_UNITS = 2;
    static constexpr int AGRID_SHIFT = 33;
    static_assert(fx::from_int(AGRID_CELL_UNITS).raw == (i64{1} << AGRID_SHIFT),
                  "the cell size and the shift are the same number said twice; "
                  "they have to agree or the query margin is wrong");
    // Above these, the query gives up and the caller runs the full sweep for
    // that body. Correct either way -- the full sweep IS the reference order --
    // so these only bound the work, never the answer.
    static constexpr u32 AGRID_MAX_CAND = 256;
    static constexpr i64 AGRID_MAX_CELLS = 256;

    u16 agrid_head_[AGRID_BUCKETS]{};
    u16 agrid_next_[MAX_AGENTS]{};

    // How the last tick's resolution went: bodies served by the grid, and
    // bodies that gave up on it. Not hashed -- it is a report about how the
    // answer was reached, never part of the answer.
    //
    // Counted rather than inferred, because the fast path and the slow path
    // agree BY DESIGN, so a test that compares them passes just as happily when
    // the fast path never ran at all. The count is what tells the difference
    // between "identical" and "never tried".
    u32 agrid_fast_ = 0;
    u32 agrid_full_ = 0;

    // Test-only: send every body down the full sweep.
    //
    // The broadphase's entire claim is that it is indistinguishable from the
    // sweep it replaces, and a claim with no way to run the other side is not a
    // claim anyone can check. One bool, read once per body rather than once per
    // pair, which is why it costs nothing worth measuring.
    bool agrid_off_ = false;

    PhaseHook phase_hook_ = nullptr;
    void*     phase_ctx_ = nullptr;
    void phase(u32 p) noexcept { if (phase_hook_) phase_hook_(phase_ctx_, p); }

    static u32 agrid_bucket(i64 cx, i64 cy) noexcept {
        // Unsigned throughout: signed overflow is undefined, and a hash that is
        // undefined behaviour is a hash that may differ between two builds of
        // the same simulation.
        const u64 h = static_cast<u64>(cx) * 73856093ull ^
                      static_cast<u64>(cy) * 19349663ull;
        return static_cast<u32>(h) & (AGRID_BUCKETS - 1);
    }

    void build_agent_grid() noexcept;

    // One past the highest agent slot ever used. Every per-tick sweep over the
    // agent space runs to this instead of to MAX_AGENTS.
    //
    // The agent space is 384 and the resolution is a pair test, so a fixed
    // bound is a hundred and forty thousand iterations a tick to serve a map
    // with two players on it. Never shrinks on despawn, which keeps it a bound
    // rather than something whose value depends on despawn order -- and a bound
    // is all it needs to be.
    //
    // "EVERY SWEEP" WAS NOT TRUE WHEN THIS WAS WRITTEN. Six of them ran the
    // full table anyway -- carry_anchored, sweep_moving_sectors,
    // detect_vertical_crush, resolve_attack, detect_prop_events and
    // sweep_entities -- so an empty level paid for 384 slots on every tick of
    // every one of them. Measured before raising the cap, precisely because
    // raising it would have multiplied a cost nobody had noticed.
    //
    // Three sweeps still run the full table, and must:
    //
    //   reset()   clears every slot, including ones above the mark
    //   spawn()   scans every slot to find a free one, and is what MOVES the
    //             mark -- bounding it by the mark could never grow it
    //   hash()    folds `active` for every slot, so the state hash is a
    //             function of the agent space rather than of how much of it
    //             has been used. Bounding that would make two worlds with the
    //             same live agents hash differently for having once had more.
    u32 agent_high_water_ = 0;
    // The same trick for props, and it is worth more here: MAX_PROPS is 768
    // and a Doom level uses a few dozen, so five per-agent loops were walking
    // seven hundred dead slots each. Slots at or above this mark have NEVER
    // been active, so bounding a loop by it cannot change an answer -- every
    // one of those loops already skips `!active`.
    //
    // It never shrinks. A high-water mark that fell on despawn would have to be
    // recomputed, and the recompute is the scan it is trying to avoid.
    u32 prop_high_water_ = 0;
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
    WeaponState          weapons_[MAX_AGENTS]{};
    u8                   teleports_[MAX_AGENTS]{};
    const fx3*           query_pos_ = nullptr;
    const u8*            query_live_ = nullptr;
    u32                  query_count_ = 0;
    // Where a shot or an auto-aim finds agent j: rewound when a shooter's
    // poses are set, else where it is. False when it was not there then.
    bool query_body(u32 j, fx3& pos) const noexcept {
        if (query_pos_ != nullptr && j < query_count_) {
            if (!query_live_[j]) return false;
            pos = query_pos_[j];
            return true;
        }
        pos = agents_[j].pos;
        return true;
    }
    // The running swing's own shape, when a weapon's `swing` step gave one:
    // what swing_shape lays over the parameters. A copy rather than a pointer
    // into the table, because the table is a Lua VM's and a swing outlives
    // the tick that started it.
    WSwing               swing_over_[MAX_AGENTS]{};
    // A swing asked for, which the next step_agent starts or refuses. Cleared
    // at the end of every step, so a weapon step's `over` -- into the table
    // step_weapon was given this tick, null for the parameters' shape -- is
    // only read while that table is certainly there.
    struct SwingRequest {
        bool          want = false;
        bool          from_step = false;    // a weapon's step, which it names
        u8            weapon = NO_WEAPON, phase = NO_PHASE, step = 0;
        const WSwing* over = nullptr;
    };
    SwingRequest         swing_req_[MAX_AGENTS]{};
    void start_swing(u32 i) noexcept;
    const WeaponTable*   weapon_table_ = nullptr;
    u64                  weapon_seed_ = 0;
    u32                  weapon_agent_ = MAX_AGENTS;
    u32                  weapon_stream_ = MAX_AGENTS;
    WeaponEventLog       weapon_events_{};
    PendingVfx           weapon_vfx_[MAX_PELLETS + 8]{};
    u32                  weapon_vfx_count_ = 0;
    WeaponLaunch         weapon_launches_[MAX_PELLETS]{};
    u32                  weapon_launch_count_ = 0;
    void present_weapon(u32 i, const WeaponTable& table) noexcept;
    u32                  tick_ = 0;
};

}  // namespace dnp::sim
