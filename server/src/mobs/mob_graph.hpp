#pragma once
//
// A mob's behaviour graph, compiled out of the Lua tables it was validated in.
//
// WHY THIS EXISTS. maps/scripts/mobs.lua is a bounded interpreter over those
// tables, and the interpretation is the cost: a think walks a state's
// transitions, and each transition's conditions, through Lua table lookups and
// `fx` userdata whose every comparison allocates. Profiled at a thousand
// monsters, 88% of all Lua time was in `lj_BC_FUNCC` -- the dispatch into C --
// with a further 15% in allocation and the collector chasing it. Not in the
// interpreter deciding things: in the cost of asking.
//
// SO C++ ASKS INSTEAD. The graph crosses once, at load, into the flat arrays
// below, and the per-think decision -- where am I, who am I hunting, which
// transition fires, which way do I walk -- runs here with no Lua and no
// allocation at all.
//
// WHAT DOES NOT MOVE, and this is the important half:
//
//   THE VOCABULARY. maps/scripts/mob_vocab.lua stays the only definition of
//   what a condition or an action IS, and mobs.lua stays the only thing that
//   validates against it. This reads a graph that has ALREADY been checked; it
//   has no opinion about whether it is well formed, and it cannot accept
//   anything the loader would have rejected. A second validator would be a
//   second answer to "is this legal", which is exactly the trap docs/ENGINE.md
//   names as two programs sharing one fact.
//
//   THE ACTIONS. All nineteen of them stay in Lua, unchanged, and are called
//   back in the order and at the tick they would have been. That is not a
//   compromise, it is where the frequency actually is: a think evaluates
//   conditions every time and fires an action only when something happens. A
//   zombieman's `chase` is four conditions eight times a second and a hitscan
//   once a second, so moving the conditions is nearly all of the win and moving
//   `spawn` would be a second implementation of pain-elemental budgeting for
//   no measurable gain.
//
//   ANY GRAPH THIS CANNOT RUN. A condition it does not implement puts that mob
//   on the Lua path for good, counted and reportable -- see MobRuntime. Silence
//   there would make the whole exercise unmeasurable.
//
#include "dnp/math/fixed.hpp"
#include "dnp/proto/messages.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace dnp::server::mobs {

using namespace dnp;

// The conditions, by the names mob_vocab.lua gives them. The order here is not
// meaningful -- transitions carry their own order, and the graph records it.
enum class Cond : u8 {
    HasTarget,
    TargetWithin,
    TargetVisible,
    TargetInFront,
    HpBelow,
    StateElapsed,
    Chance,
    VarAtLeast,
    VarIs,
    CooldownReady,
    Grounded,
    WasHurt,
    HurtByMob,
    MobWithin,
    PropWithin,
    CountWithin,
    Stuck,
    CanReach,
    AllyInFront,
    TargetAbove,
    TargetBelow,
    // Everything below is NOT evaluated here. A graph that uses one runs in
    // Lua, and MobRuntime says so out loud.
    Unsupported,
};

// `move` and `face` on a state. Kept as enums rather than strings because
// drive() reads them every think.
enum class Move : u8 { Stop, Chase, Flee, Strafe, Wander, Free };
enum class Face : u8 { Target, Hold };

struct Condition {
    Cond id = Cond::Unsupported;
    bool negate = false;
    // `distance` for target_within, and nothing else needs an fx.
    fx   f{};
    // `percent`, `ticks`, `value`, `arc`, `within` -- one integer is enough for
    // every condition in the vocabulary that takes one.
    i32  i = 0;
    // Which of the mob's var slots, for the three that name one. -1 otherwise.
    i16  var = -1;
    // A mob kind or a catalogue record, for the proximity conditions. An index
    // into MobGraph::strings, or NO_NAME.
    u32  name = 0xFFFFFFFFu;
    // `alive` on mob_within, which defaults TRUE: a corpse is still an entity,
    // because that is what lets `revive` find it, so counting them by default
    // would have a monster reacting to a room of bodies.
    bool flag = true;
};

inline constexpr u32 NO_NAME = 0xFFFFFFFFu;

struct Transition {
    u16 to = 0;            // state index within the kind
    u16 first_cond = 0;
    u16 cond_count = 0;
};

// The six actions the loop runs itself.
//
// NOT A SECOND IMPLEMENTATION of the vocabulary: these are exactly the ones
// that write state this runtime owns -- a mob's vars, its animation, its aim,
// which state it is in -- and there is nowhere else for them to write it. Every
// other action stays in maps/scripts/mobs.lua and is called back at the tick it
// would have fired, because what `spawn` or `damage_area` does is about the
// world rather than about the mob, and one implementation of each is the point.
enum class Act : u8 {
    SetVar,
    AddVar,
    SetCooldown,
    SetAnim,
    Goto,
    Face,
    SetAngle,
    VarRandom,
    // Everything else: handed to Lua by its index in the state's `sequence`.
    Lua,
};

// Where an angle comes from, for `face` and `set_angle`. One enum for both
// because they ask the same question, and having asked it twice is how two
// spellings of "towards" drift apart.
enum class Towards : u8 { Target, Facing, Away, Random, Angle, Var, Movement };

// A sequence entry. `at` is a DEADLINE relative to entering the state, not a
// countdown -- a mob nobody is near costs nothing between its own decisions --
// and `lua_index` is the 1-based index of the entry in that state's `sequence`
// array, so an action that stays in Lua can be named without anything about
// what it DOES having to cross.
struct SeqEntry {
    i32 at = 0;
    u16 lua_index = 0;
    Act id = Act::Lua;
    i16 var = -1;          // set_var / add_var / set_cooldown / set_angle
    i32 i = 0;             // value / by / ticks / anim / angle, in degrees
    i16 state = -1;        // goto
    Towards towards = Towards::Target;
    i32 offset = 0;        // face: degrees added after
    // Degrees of random slop, or -1 for ABSENT -- which is not the same as
    // zero. The two runtimes share one RNG stream, so a draw one makes and the
    // other skips desynchronises every later one; both draw exactly when the
    // GRAPH has a jitter argument, which is the same on both sides.
    i32 jitter = -1;
};

struct State {
    u32  name = 0;          // index into MobGraph::strings
    i32  anim = -1;         // index into MobGraph::strings, or -1
    Move move = Move::Stop;
    Face face = Face::Target;
    i8   solid = -1;        // -1 unset, 0 false, 1 true
    i32  ticks = -1;        // -1 for "no timeout"
    // Extra ticks, rolled on entry. -1 for absent, which is NOT zero: both
    // runtimes draw exactly when the graph carries the field.
    i32  ticks_jitter = -1;
    i16  next = -1;
    u16  first_trans = 0, trans_count = 0;
    u16  first_seq = 0, seq_count = 0;
};

struct Kind {
    u32  name = 0;
    i32  hp = 0;
    fx   sight{};
    bool has_sight = false;
    bool flying = false;
    fx   hover_z{};
    fx   jump_height{};
    i16  initial = -1;
    u16  first_state = 0, state_count = 0;
    // Var slots, as names interned per kind. Every `var` argument anywhere in
    // the graph gets one, whether or not `vars` declared it -- mob_vocab's
    // `var` type is a name, not a declaration, and set_var may name a fresh one.
    u32  first_var = 0, var_count = 0;
    // The reason this kind cannot run here, or empty. Held rather than
    // discarded so the report names the condition and not just the mob.
    std::string unsupported;
    bool supported() const { return unsupported.empty(); }
};

// The compiled graph for one level's mobs.
//
// Flat arrays with ranges into them, because a think walks them and a tree of
// small allocations is what this exists to stop being.
struct MobGraph {
    std::vector<Kind>       kinds;
    std::vector<State>      states;
    std::vector<Transition> transitions;
    std::vector<Condition>  conditions;
    std::vector<SeqEntry>   sequence;
    // Var names, in per-kind ranges.
    std::vector<u32>        var_names;
    // Interned strings: mob names, state names, animation names.
    std::vector<std::string> strings;

    void clear() {
        kinds.clear(); states.clear(); transitions.clear();
        conditions.clear(); sequence.clear(); var_names.clear(); strings.clear();
    }

    u32 intern(const char* s, size_t n) {
        for (u32 i = 0; i < strings.size(); ++i) {
            if (strings[i].size() == n && std::memcmp(strings[i].data(), s, n) == 0) return i;
        }
        strings.emplace_back(s, n);
        return static_cast<u32>(strings.size() - 1);
    }
    const char* str(u32 i) const { return i < strings.size() ? strings[i].c_str() : ""; }

    i32 kind_by_name(const char* s, size_t n) const {
        for (u32 i = 0; i < kinds.size(); ++i) {
            const std::string& k = strings[kinds[i].name];
            if (k.size() == n && std::memcmp(k.data(), s, n) == 0) return static_cast<i32>(i);
        }
        return -1;
    }

    // A state's index within a kind, or -1. Used only while compiling and by
    // the bindings that let Lua name a state -- never per think.
    i32 state_by_name(const Kind& k, const char* s, size_t n) const {
        for (u16 i = 0; i < k.state_count; ++i) {
            const std::string& v = strings[states[k.first_state + i].name];
            if (v.size() == n && std::memcmp(v.data(), s, n) == 0) return i;
        }
        return -1;
    }

    i32 var_slot(const Kind& k, const char* s, size_t n) const {
        for (u32 i = 0; i < k.var_count; ++i) {
            const std::string& v = strings[var_names[k.first_var + i]];
            if (v.size() == n && std::memcmp(v.data(), s, n) == 0) return static_cast<i32>(i);
        }
        return -1;
    }
};

}  // namespace dnp::server::mobs
