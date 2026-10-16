#pragma once
//
// The C++ side of the mob runtime.
//
// Built in increments, and this header is the seam between them. Today it
// carries the compiled graph and the report about it; the decision loop that
// reads that graph lands on top of it.
//
// WHERE THE LINE IS, and why it is drawn by frequency rather than by
// difficulty. A think evaluates conditions EVERY time and fires an action only
// when something happens -- a zombieman's `chase` is four conditions eight
// times a second and a hitscan about once. So the conditions, the transition
// scan and the steering are what move; all nineteen actions stay in
// maps/scripts/mobs.lua, unchanged, and are called back at the tick they would
// have fired. That is not a staging compromise. Reimplementing `spawn` here
// would be a second answer to pain-elemental budgeting for no measurable gain,
// and docs/ENGINE.md has a name for two programs sharing one fact.
//
#include "mobs/mob_graph.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/sim/world.hpp"
#include "dnp/nav/navgraph.hpp"
#include "lua/commands.hpp"
#include "lua/world_binding.hpp"

struct lua_State;

namespace dnp::server::mobs {

// One live mob. What maps/scripts/mobs.lua keeps in its `m` record, minus
// everything only an action touches -- the entity handle, the definition table
// and the brood stay in Lua, because the actions that read them stay there too.
struct Mob {
    u32  entity = 0;          // agent index
    u8   generation = 0;      // so a recycled slot is not mistaken for this one
    i32  kind = -1;
    i16  state = -1;          // index within the kind
    u32  entered = 0;         // the tick it entered that state
    u16  seq_i = 0;           // how far through the state's sequence

    i32  hp = 0, max_hp = 0;
    i32  target = -1;         // agent index, or -1
    u8   target_gen = 0;
    u32  aim = 0;             // brad

    bool dormant = false;
    bool dead = false;
    bool want_jump = false;

    // The pain edge. `hurt_ever` distinguishes "never hurt" from "hurt on tick
    // zero", which mobs.lua gets for free from nil and this does not.
    bool hurt_ever = false;
    bool hurt_by_mob = false;
    u32  hurt_at = 0;

    i32  anim = -1;
    // WHERE IT WAS before the state it is in, for a reaction (`next =
    // "@previous"`) to resume: mobs.lua's m.prev, field for field.
    bool has_prev = false;
    i16  prev_state = -1;
    u32  prev_elapsed = 0;
    u16  prev_seq_i = 0;
    i32  prev_state_ticks = -1;
    i32  prev_anim = -1;
    // How long THIS entry of the current state lasts: `ticks` plus whatever
    // `ticks_jitter` rolled on the way in. -1 for a state with no timeout.
    i32  state_ticks = -1;

    // The last input actually written. set_move is sticky -- the engine keeps
    // applying the last frame -- so an unchanged decision must NOT be resent:
    // each one is a command that would replicate and appear in a snapshot.
    bool input_sent = false;
    fx   mx{}, my{};
    u32  sent_aim = 0;
    u16  buttons = 0;

    u32  first_var = 0;       // into MobRuntime::vars

    // The route it is following, as a range into MobRuntime::route. Sectors,
    // not points: a mob is at a waypoint when it is IN that sector, and a
    // radius test would have it circle a centroid it cannot stand on.
    u32  route_at = 0;
    u32  route_len = 0;
    u32  route_i = 0;
    i32  route_to = -1;
    u32  repath_at = 0;
    u32  wander = 0;

    // The anchor for `stuck`: where the body last actually got to, and when.
    // It moves when the body gets a quarter unit away from it, so "has not got
    // anywhere for N ticks" is a subtraction. Rewriting it every think instead
    // measures a window exactly one think long and can never be true, which is
    // what the first version of this did.
    fx3  anchor{};
    u32  still_since = 0;
    bool has_anchor = false;

    // Valid only within this mob's own think, exactly as mobs.lua's m.p/m.tp
    // are: fetched once at the top of its turn rather than once per condition.
    fx3  p{};
    u32  pos_tick = 0;
    fx3  tp{};
    bool has_tp = false;
};

// What a condition is allowed to look at outside the mob itself.
//
// Narrow on purpose. Every condition in the vocabulary is a pure read except
// `chance`, which the vocabulary says so about, and keeping the surface this
// small is what makes "a condition never mutates" checkable rather than
// merely stated.
struct EvalWorld {
    const sim::World* world = nullptr;
    u32  tick = 0;
    Rng* rng = nullptr;       // only `chance` touches it
    // The proximity conditions need to know what else is here: the graph, to
    // name a mob's kind, and the runtime's live set to walk. The catalogue is
    // for `prop_within`, which compares a record's NAME -- a number in a
    // script is a number somebody has to look up in a file the script does not
    // mention.
    const MobGraph* graph = nullptr;
    const std::vector<Mob>* mobs = nullptr;
    const lua::PropCatalogueView* props = nullptr;
    // For can_reach, the one condition that asks the engine to SEARCH rather
    // than to look -- and the context with it, because the search is spent
    // against the same per-tick budget dnp.find_path spends.
    const nav::NavGraph* nav = nullptr;
    lua::WorldContext* ctx = nullptr;
};

// Does this transition's `when` hold? AND over the list, in declaration order,
// stopping at the first false -- which is what makes the order in a transition
// worth thinking about, since `target_visible` costs a raycast and a
// `target_within` before it is what makes that cheap.
bool holds(const MobGraph& g, const Kind& k, const Mob& m,
           const Transition& t, const EvalWorld& w, const i32* vars);

// One condition. Exposed so the differential test can ask about a single one
// rather than inferring which of several disagreed.
bool evaluate(const MobGraph& g, const Kind& k, const Mob& m,
              const Condition& c, const EvalWorld& w, const i32* vars);

// Compile one mob definition -- the table `defs[name]` in mobs.lua -- into `k`,
// appending its states, transitions, conditions and sequence entries to `g`.
//
// `def_idx` is a stack index of that table. The table is left where it was; the
// stack is balanced on return.
//
// Returns false only for a definition with no `states`, which the loader would
// already have complained about. Everything else it can read, it reads; a
// condition it does not implement is recorded in `k.unsupported` and is not an
// error, because that mob still has to run.
bool compile_kind(lua_State* L, int def_idx, MobGraph& g, Kind& k,
                  std::string& err);

// Must match maps/scripts/mobs.lua's THINK. Two numbers that have to be equal
// and live in two files are the failure this repository keeps paying for, so
// the binding asserts them against each other at load rather than trusting
// this comment.
inline constexpr u32 THINK = 8;

struct MobRuntime {
    MobGraph graph;

    // Borrowed, and set once per level. Not owned: a second copy of the world
    // or the nav graph here would be a second answer to where everything is.
    sim::World*          world = nullptr;
    const nav::NavGraph* nav = nullptr;
    lua::CommandQueue*   commands = nullptr;
    Rng*                 rng = nullptr;
    const lua::PropCatalogueView* props = nullptr;
    // For the per-tick path budget, which this loop spends alongside
    // dnp.find_path -- see WorldContext::take_path_budget.
    lua::WorldContext*   ctx = nullptr;

    std::vector<Mob> mobs;
    std::vector<i32> vars;                 // flat, per-mob ranges
    std::vector<u32> bucket[THINK];        // mob indices, in spawn order
    std::vector<i32> by_entity;            // agent index -> mob index, or -1
    std::vector<u16> route;                // flat, per-mob ranges of sectors
    std::vector<u8>  route_jump;

    u32 spawned = 0;
    bool asleep = false;

    // What the loop did, for the report. Not state: a description of how the
    // answer was reached.
    u32 thought = 0;          // mobs that took a decision this tick
    u32 lua_actions = 0;      // actions handed back to Lua this tick
    u32 routes_truncated = 0;
    u32 routes_failed = 0;     // searches that found no route (or gave up), each backing off
    // This tick's route searching, for the slow-tick report (mob_binding.cpp).
    u32 paths_this_tick = 0;
    u32 expanded_this_tick = 0;
    u64 path_us_this_tick = 0;

    void reset() {
        mobs.clear();
        vars.clear();
        for (u32 i = 0; i < THINK; ++i) bucket[i].clear();
        by_entity.assign(sim::MAX_AGENTS, -1);
        route.clear();
        route_jump.clear();
        spawned = 0;
        asleep = false;
    }
};


// The heading an action asks for, or false when there is none. Shared by
// `face` and `set_angle`: they ask the same question, and having asked it
// twice is how two spellings of "towards" drift apart.
bool heading_for(MobRuntime& rt, const sim::World& w, const Mob& m,
                 const SeqEntry& e, const i32* vars, i64& out);

// Where an action this loop does not run goes.
//
// An INTERFACE rather than a direct call into Lua, for the reason every
// boundary in this subsystem is one: the decision loop is plain integer code
// over flat arrays and has no business knowing what a lua_State is. The
// implementation lives beside the bindings, and a test can hand it a recorder
// instead and assert which actions fired at which tick without a VM at all.
struct ActionSink {
    virtual ~ActionSink() = default;
    // Run entry `seq_index` (1-based, into that state's Lua `sequence`) for the
    // mob on `entity`.
    //
    // VOID, and the caller works out for itself whether the mob left the state
    // -- by looking at whether it is in a different one, or entered the same
    // one again, once this returns. An action that goes somewhere does it
    // through mob_enter like everything else, so there is nothing for the sink
    // to report that the runtime cannot already see. A bool here would be a
    // second channel for one fact, and the sink would have to be told what its
    // own callee did.
    virtual void run(u32 entity, const char* state_name, u16 seq_index) = 0;
};

// --- driving it -------------------------------------------------------------

// Start tracking a mob that has already been spawned into the world.
//
// The ENTITY comes first and this follows, which is the order mobs.lua uses:
// dnp.spawn_enemy makes the body from the catalogue, and only then is there
// something for a brain to belong to. Returns false for a kind this runtime
// does not run -- which is not a failure, it is the Lua path, and the caller
// keeps its own record either way.
bool track(MobRuntime& rt, u32 entity, const char* kind, size_t kind_len,
           bool dormant, u32 tick);

// Stop tracking one, and forget everything.
void untrack(MobRuntime& rt, u32 entity);

// One tick: the bucket whose turn it is, each mob refreshed, aimed, decided
// and driven. Actions this loop does not run go to `lua`.
void tick_mobs(MobRuntime& rt, u32 tick, ActionSink* lua);

// Enter a named state, for the things outside the loop that move a mob: Lua's
// `hurt` sending it to `pain` or `death`, and `revive` putting a corpse back.
// A name rather than an index, because that is what the caller has and what
// the graph is written in.
bool mob_enter(MobRuntime& rt, u32 entity, const char* state, size_t len, u32 tick);

// The mob on this entity, or null. For the bindings that let Lua read and
// write the few fields its own actions still touch.
Mob* mob_of(MobRuntime& rt, u32 entity);

// A mob's var by name, for the same reason. Returns null for a name the kind
// never mentions -- which cannot happen for a graph that compiled, because
// every `var` argument anywhere in it got a slot.
i32* mob_var(MobRuntime& rt, Mob& m, const char* name, size_t len);

// Install the `dnp.mobs_*` bindings on the table at the top of the stack.
void open_mobs(lua_State* L);

// The compiled graph for the level whose Lua state this is. One per VM, since
// one VM hosts one level's scripts.
MobGraph& graph_of(lua_State* L);

}  // namespace dnp::server::mobs
