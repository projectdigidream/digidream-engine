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

    // The last input actually written. set_move is sticky -- the engine keeps
    // applying the last frame -- so an unchanged decision must NOT be resent:
    // each one is a command that would replicate and appear in a snapshot.
    bool input_sent = false;
    fx   mx{}, my{};
    u32  sent_aim = 0;
    u16  buttons = 0;

    u32  first_var = 0;       // into MobRuntime::vars

    // Valid only within this mob's own think, exactly as mobs.lua's m.p/m.tp
    // are: fetched once at the top of its turn rather than once per condition.
    fx3  p{};
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

// Install the `dnp.mobs_*` bindings on the table at the top of the stack.
void open_mobs(lua_State* L);

// The compiled graph for the level whose Lua state this is. One per VM, since
// one VM hosts one level's scripts.
MobGraph& graph_of(lua_State* L);

}  // namespace dnp::server::mobs
