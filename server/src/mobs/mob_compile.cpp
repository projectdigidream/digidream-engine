//
// Reading a validated mob graph out of Lua, once, at load.
//
// THIS DOES NOT VALIDATE. maps/scripts/mobs.lua has already checked every
// field, every argument, every enum and every cross-reference against
// maps/scripts/mob_vocab.lua, and it is the only thing that may. What arrives
// here is known-good, and the one judgement this makes is a different one:
// whether the graph uses anything the C++ decision loop does not implement.
//
// That judgement is RECORDED, not enforced. A kind using a condition from the
// Lua column keeps running exactly as it does today, on the Lua path, and says
// so by name -- see Kind::unsupported. Refusing it would be worse than useless:
// the point of this file is to make mobs cheaper, and a mob that cannot be made
// cheaper must still work.
//
// WHY A SEPARATE PASS AT ALL, rather than compiling as mobs.load validates: two
// programs, one fact. The loader would then be deciding both "is this legal"
// and "can C++ run it", and the second answer would drift the moment somebody
// added a condition here without touching the loader. This walks the finished
// tables, so the only thing it can disagree with is itself.
//
#include <cstring>

#include "mobs/mob_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include "lua/fx_binding.hpp"

namespace dnp::server::mobs {

namespace {

// A field off the table at `t`, left on the stack. Returns false and pops
// nothing useful when it is nil.
bool field(lua_State* L, int t, const char* k) {
    lua_getfield(L, t, k);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return false; }
    return true;
}

bool opt_bool(lua_State* L, int t, const char* k, bool dflt) {
    if (!field(L, t, k)) return dflt;
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

// -1 when absent, so a caller can tell "not set" from "set to zero" -- which
// `ticks` and `anim` both need.
i32 opt_int(lua_State* L, int t, const char* k, i32 dflt) {
    if (!field(L, t, k)) return dflt;
    const i32 v = static_cast<i32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}

bool opt_fx(lua_State* L, int t, const char* k, fx& out) {
    if (!field(L, t, k)) return false;
    const bool ok = lua::is_fx(L, -1);
    if (ok) out = lua::check_fx(L, -1);
    lua_pop(L, 1);
    return ok;
}

// A string field, as a pointer into Lua's own storage. Valid only until the
// value is popped, which every caller does immediately.
const char* opt_str(lua_State* L, int t, const char* k, size_t& n) {
    if (!field(L, t, k)) { n = 0; return nullptr; }
    if (!lua_isstring(L, -1)) { lua_pop(L, 1); n = 0; return nullptr; }
    const char* s = lua_tolstring(L, -1, &n);
    return s;   // caller pops
}

bool streq(const char* s, size_t n, const char* lit) {
    const size_t m = std::strlen(lit);
    return n == m && std::memcmp(s, lit, m) == 0;
}

}  // namespace

// --- which runtime runs a name --------------------------------------------
//
// Two chains and one table, and the split between them is the point.
//
// The CHAINS say what this loop implements. The TABLE says what it
// deliberately does not, and why. They are disjoint by construction -- a name
// in the chain never reaches the table -- so their union either covers
// mob_vocab.lua or it does not, and server/tests/test_mob_graph.cpp is what
// asks. Before the table existed the fallback set was whatever the chains
// happened to miss, which is not a statement anything can check: adding a
// condition and forgetting this file silently dropped every kind using it onto
// the Lua path.
//
// `target_is` asks what KIND of thing is being hunted, and the answer for a
// prop is one Mob::target cannot represent -- see target_nearest in
// compile_kind. Left unsupported so a graph that asks runs in Lua, rather than
// answering "not a prop" for a mob that is in fact chasing one.
Cond cond_by_name(const char* s, size_t n) {
    if (streq(s, n, "has_target"))     return Cond::HasTarget;
    if (streq(s, n, "target_within"))  return Cond::TargetWithin;
    if (streq(s, n, "target_visible")) return Cond::TargetVisible;
    if (streq(s, n, "target_in_front"))return Cond::TargetInFront;
    if (streq(s, n, "hp_below"))       return Cond::HpBelow;
    if (streq(s, n, "state_elapsed"))  return Cond::StateElapsed;
    if (streq(s, n, "chance"))         return Cond::Chance;
    if (streq(s, n, "var_at_least"))   return Cond::VarAtLeast;
    if (streq(s, n, "var_is"))         return Cond::VarIs;
    if (streq(s, n, "cooldown_ready")) return Cond::CooldownReady;
    if (streq(s, n, "grounded"))       return Cond::Grounded;
    if (streq(s, n, "was_hurt"))       return Cond::WasHurt;
    if (streq(s, n, "hurt_by_mob"))    return Cond::HurtByMob;
    if (streq(s, n, "mob_within"))     return Cond::MobWithin;
    if (streq(s, n, "prop_within"))    return Cond::PropWithin;
    if (streq(s, n, "count_within"))   return Cond::CountWithin;
    if (streq(s, n, "stuck"))          return Cond::Stuck;
    if (streq(s, n, "can_reach"))      return Cond::CanReach;
    if (streq(s, n, "ally_in_front"))  return Cond::AllyInFront;
    if (streq(s, n, "target_above"))   return Cond::TargetAbove;
    if (streq(s, n, "target_below"))   return Cond::TargetBelow;
    return Cond::Unsupported;
}

// The eight this loop runs itself, because they write state it owns: a mob's
// vars, its animation, its aim, which state it is in. Everything else is
// Act::Lua and is called back through ActionSink at the tick it comes due.
Act act_by_name(const char* s, size_t n) {
    if (streq(s, n, "goto"))         return Act::Goto;
    if (streq(s, n, "set_var"))      return Act::SetVar;
    if (streq(s, n, "add_var"))      return Act::AddVar;
    if (streq(s, n, "set_cooldown")) return Act::SetCooldown;
    if (streq(s, n, "set_anim"))     return Act::SetAnim;
    if (streq(s, n, "face"))         return Act::Face;
    if (streq(s, n, "set_angle"))    return Act::SetAngle;
    if (streq(s, n, "var_random"))   return Act::VarRandom;
    return Act::Lua;
}

// WHAT THE CHAINS ABOVE DELIBERATELY OMIT, and why.
//
// Written down rather than left as "whatever did not match", because a set
// defined by omission is not one anything can check -- and the reasons are
// worth having next to the classifier, where the person adding a condition is
// already typing.
namespace {

struct LuaOnly { const char* name; const char* why; };

constexpr LuaOnly LUA_CONDS[] = {
    {"lua",       "an arbitrary chunk may write anything this loop owns"},
    {"target_is", "a prop target cannot live in Mob::target, which is an agent index"},
};

constexpr LuaOnly LUA_ACTIONS[] = {
    {"lua",            "an arbitrary chunk may write anything this loop owns"},
    {"hitscan",        "touches the world; rare, so one implementation is Lua's"},
    {"missile",        "touches the world"},
    {"damage",         "touches the world"},
    {"melee",          "touches the world"},
    {"swing",          "presses a button on the world's input; one tick, rare"},
    {"telegraph",      "emits a vfx event; rare, and touches the world"},
    {"impulse",        "touches the world"},
    {"spawn",          "touches the world, and owns the brood budget"},
    {"teleport",       "touches the world, and asks it where a body fits; rare"},
    {"revive",         "reaches into another mob's state"},
    {"damage_area",    "touches the world"},
    {"set_param",      "goes through the effect stack"},
    {"effect",         "goes through the effect stack"},
    {"vfx",            "touches the world"},
    {"sound",          "emits a sound event; rare"},
    {"despawn",        "touches the world"},
    {"target_nearest", "may point at a prop, which Mob::target cannot hold"},
    {"target_attacker","reads `hurt`'s record, which lives in Lua"},
    {"target_none",    "writes through mob_set; rare, so it stays beside its pair"},
    {"wake",           "writes `dormant` through mob_set; rare"},
    {"sleep",          "writes `dormant` through mob_set; rare"},
    {"alert_nearby",   "reaches into every mob near it"},
};

template <size_t N>
const char* declared_lua(const LuaOnly (&table)[N], const char* s, size_t n) {
    for (const LuaOnly& e : table) if (streq(s, n, e.name)) return e.why;
    return nullptr;
}

}  // namespace

RunsIn runs_in_cond(const char* s, size_t n, const char** why) {
    if (why) *why = nullptr;
    if (cond_by_name(s, n) != Cond::Unsupported) return RunsIn::Cpp;
    if (const char* w = declared_lua(LUA_CONDS, s, n)) {
        if (why) *why = w;
        return RunsIn::Lua;
    }
    return RunsIn::Undeclared;
}

RunsIn runs_in_action(const char* s, size_t n, const char** why) {
    if (why) *why = nullptr;
    if (act_by_name(s, n) != Act::Lua) return RunsIn::Cpp;
    if (const char* w = declared_lua(LUA_ACTIONS, s, n)) {
        if (why) *why = w;
        return RunsIn::Lua;
    }
    return RunsIn::Undeclared;
}

namespace {

Move move_by_name(const char* s, size_t n) {
    if (s == nullptr) return Move::Stop;
    if (streq(s, n, "chase"))  return Move::Chase;
    if (streq(s, n, "flee"))   return Move::Flee;
    if (streq(s, n, "strafe")) return Move::Strafe;
    if (streq(s, n, "wander")) return Move::Wander;
    if (streq(s, n, "free"))   return Move::Free;
    // "charge" and "stop" both drive straight at the aim with no steering,
    // which is what mobs.lua's drive() does for anything it does not name.
    return Move::Stop;
}

Face face_by_name(const char* s, size_t n) {
    // mobs.lua defaults `face` to "target" and special-cases only that value;
    // "movement" and "hold" both leave the aim where it was.
    if (s == nullptr) return Face::Target;
    return streq(s, n, "target") ? Face::Target : Face::Hold;
}

// Every var name the kind mentions, wherever it mentions it. `vars` declares
// the initial values, but mob_vocab's `var` type is a NAME rather than a
// declaration, so set_var may introduce one the definition never listed -- and
// a slot table built only from `vars` would then have nowhere to put it.
void collect_var(MobGraph& g, Kind& k, const char* s, size_t n) {
    if (s == nullptr || n == 0) return;
    if (g.var_slot(k, s, n) >= 0) return;
    g.var_names.push_back(g.intern(s, n));
    ++k.var_count;
}

// Walk a condition list (a transition's `when`) collecting var names, before
// the slots are needed. Two passes over the same tables, because a slot index
// cannot be resolved until every name in the kind is known.
void scan_vars_in_conditions(lua_State* L, int list, MobGraph& g, Kind& k) {
    if (!lua_istable(L, list)) return;
    const int n = static_cast<int>(lua_objlen(L, list));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, list, i);
        if (lua_istable(L, -1)) {
            size_t len = 0;
            const char* v = opt_str(L, lua_gettop(L), "var", len);
            if (v != nullptr) { collect_var(g, k, v, len); lua_pop(L, 1); }
        }
        lua_pop(L, 1);
    }
}

void scan_vars_in_sequence(lua_State* L, int list, MobGraph& g, Kind& k) {
    if (!lua_istable(L, list)) return;
    const int n = static_cast<int>(lua_objlen(L, list));
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, list, i);
        if (lua_istable(L, -1)) {
            size_t len = 0;
            const char* v = opt_str(L, lua_gettop(L), "var", len);
            if (v != nullptr) { collect_var(g, k, v, len); lua_pop(L, 1); }
        }
        lua_pop(L, 1);
    }
}

}  // namespace

// The state names of one kind, in the order `states` happens to yield them.
//
// ORDER IS RECORDED, NOT RELIED ON. Lua does not keep the order of a hash, and
// mobs.lua says so where it explains why `initial` cannot default to "the
// first state". So the index a state gets here is an artefact of this walk --
// which is fine, because nothing outside this graph ever sees it: states are
// named everywhere a person or another table refers to them, and resolved to
// indices only after every name is in hand.
bool compile_kind(lua_State* L, int def_idx, MobGraph& g, Kind& k,
                  std::string& err) {
    // Absolute, by hand: lua_absindex is 5.2 and this builds against LuaJIT's
    // 5.1 API. Every push below would shift a relative index underneath us.
    if (def_idx < 0) def_idx = lua_gettop(L) + 1 + def_idx;

    // --- pass 1: state names, so `next`, `to` and `goto` can be resolved -----
    k.first_state = static_cast<u16>(g.states.size());
    k.state_count = 0;
    k.first_var = static_cast<u32>(g.var_names.size());
    k.var_count = 0;

    if (!field(L, def_idx, "states")) { err = "no states"; return false; }
    const int states = lua_gettop(L);

    lua_pushnil(L);
    while (lua_next(L, states) != 0) {
        // key at -2, value at -1
        if (lua_isstring(L, -2) && lua_istable(L, -1)) {
            size_t n = 0;
            const char* s = lua_tolstring(L, -2, &n);
            State st;
            st.name = g.intern(s, n);
            g.states.push_back(st);
            ++k.state_count;
        }
        lua_pop(L, 1);
    }

    // --- pass 2: var names, from `vars` and from every argument that names one
    if (field(L, def_idx, "vars")) {
        const int vars = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, vars) != 0) {
            if (lua_isstring(L, -2)) {
                size_t n = 0;
                const char* s = lua_tolstring(L, -2, &n);
                collect_var(g, k, s, n);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    for (u16 si = 0; si < k.state_count; ++si) {
        const char* sname = g.str(g.states[k.first_state + si].name);
        lua_getfield(L, states, sname);
        if (lua_istable(L, -1)) {
            const int st_idx = lua_gettop(L);
            if (field(L, st_idx, "sequence")) {
                scan_vars_in_sequence(L, lua_gettop(L), g, k);
                lua_pop(L, 1);
            }
            if (field(L, st_idx, "transitions")) {
                const int tr = lua_gettop(L);
                const int tn = static_cast<int>(lua_objlen(L, tr));
                for (int i = 1; i <= tn; ++i) {
                    lua_rawgeti(L, tr, i);
                    if (lua_istable(L, -1) && field(L, lua_gettop(L), "when")) {
                        scan_vars_in_conditions(L, lua_gettop(L), g, k);
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    // --- pass 3: the states themselves --------------------------------------
    for (u16 si = 0; si < k.state_count; ++si) {
        State& st = g.states[k.first_state + si];
        lua_getfield(L, states, g.str(st.name));
        const int st_idx = lua_gettop(L);

        st.anim = opt_int(L, st_idx, "anim", -1);
        st.ticks = opt_int(L, st_idx, "ticks", -1);
        st.ticks_jitter = opt_int(L, st_idx, "ticks_jitter", -1);
        {
            size_t n = 0;
            const char* s = opt_str(L, st_idx, "move", n);
            st.move = move_by_name(s, n);
            if (s != nullptr) lua_pop(L, 1);
        }
        {
            size_t n = 0;
            const char* s = opt_str(L, st_idx, "face", n);
            st.face = face_by_name(s, n);
            if (s != nullptr) lua_pop(L, 1);
        }
        if (field(L, st_idx, "solid")) {
            st.solid = lua_toboolean(L, -1) ? 1 : 0;
            lua_pop(L, 1);
        }
        {
            size_t n = 0;
            const char* s = opt_str(L, st_idx, "next", n);
            st.next = !s ? static_cast<i16>(-1)
                    : (n == 9 && std::memcmp(s, "@previous", 9) == 0) ? NEXT_PREVIOUS
                    : static_cast<i16>(g.state_by_name(k, s, n));
            if (s != nullptr) lua_pop(L, 1);
        }

        // sequence: `at`, the Lua index, and whether it is a `goto`
        st.first_seq = static_cast<u16>(g.sequence.size());
        st.seq_count = 0;
        if (field(L, st_idx, "sequence")) {
            const int seq = lua_gettop(L);
            const int sn = static_cast<int>(lua_objlen(L, seq));
            for (int i = 1; i <= sn; ++i) {
                lua_rawgeti(L, seq, i);
                if (lua_istable(L, -1)) {
                    const int e = lua_gettop(L);
                    SeqEntry se;
                    se.at = opt_int(L, e, "at", 0);
                    se.lua_index = static_cast<u16>(i);
                    size_t n = 0;
                    const char* act = opt_str(L, e, "action", n);
                    if (act != nullptr) {
                        // WHICH RUNTIME RUNS IT is act_by_name's answer, and
                        // it is asked here rather than decided here so that
                        // the test can ask the same question of every name in
                        // the vocabulary.
                        se.id = act_by_name(act, n);

                        // The two rules below are about the KIND rather than
                        // the name, which is why they stay here: whether a
                        // graph can run in this loop at all, not which
                        // runtime performs one action.
                        //
                        // THE ESCAPE HATCH GOES TO THE SLOW PATH, whole.
                        // `lua` may write anything -- a mob's vars, its aim,
                        // its state -- and this runtime owns all three.
                        // Running the kind here and letting an arbitrary chunk
                        // write behind it would be two owners of one number.
                        if (streq(act, n, "lua") && k.supported()) {
                            k.unsupported.assign("lua (action)");
                        }
                        // A PROP TARGET CANNOT LIVE HERE, and saying so is
                        // better than letting it.
                        //
                        // Mob::target is an AGENT index; a prop is the other
                        // half of the entity index space. Lua's
                        // target_nearest would hand this runtime a prop index
                        // through mob_set, where it would be read as whichever
                        // agent happens to share the number -- a monster
                        // hunting a stranger, for reasons nothing reports. So
                        // a graph that can target a prop runs entirely in Lua,
                        // counted with the rest.
                        //
                        // PER ARGUMENT, not per name, which is the reason this
                        // cannot be a field on the vocabulary entry: the same
                        // action is fine when it hunts a player and is not
                        // when it hunts a barrel.
                        else if (streq(act, n, "target_nearest") && k.supported()) {
                            lua_pop(L, 1);
                            size_t on = 0;
                            const char* of = opt_str(L, e, "of", on);
                            const bool props = of != nullptr &&
                                               (streq(of, on, "prop") || streq(of, on, "any"));
                            if (of != nullptr) lua_pop(L, 1);
                            if (props) k.unsupported.assign("target_nearest of a prop");
                            // Pushed again so the common pop below balances.
                            lua_pushnil(L);
                        }
                        lua_pop(L, 1);

                        switch (se.id) {
                        case Act::Goto: {
                            size_t sn2 = 0;
                            const char* to = opt_str(L, e, "state", sn2);
                            if (to != nullptr) {
                                se.state = static_cast<i16>(g.state_by_name(k, to, sn2));
                                lua_pop(L, 1);
                            }
                            break;
                        }
                        case Act::VarRandom: {
                            size_t vn = 0;
                            const char* v = opt_str(L, e, "var", vn);
                            if (v != nullptr) {
                                se.var = static_cast<i16>(g.var_slot(k, v, vn));
                                lua_pop(L, 1);
                            }
                            se.i = opt_int(L, e, "min", 0);
                            se.offset = opt_int(L, e, "max", 0);   // the other end
                            break;
                        }
                        case Act::SetVar:
                        case Act::AddVar:
                        case Act::SetCooldown: {
                            size_t vn = 0;
                            const char* v = opt_str(L, e, "var", vn);
                            if (v != nullptr) {
                                se.var = static_cast<i16>(g.var_slot(k, v, vn));
                                lua_pop(L, 1);
                            }
                            // One integer serves all three; their argument
                            // names are distinct so reading each in turn cannot
                            // collide.
                            se.i = opt_int(L, e, "value",
                                   opt_int(L, e, "by",
                                   opt_int(L, e, "ticks", 0)));
                            se.jitter = opt_int(L, e, "jitter", -1);
                            break;
                        }
                        case Act::SetAnim:
                            se.i = opt_int(L, e, "anim", -1);
                            break;
                        case Act::Face:
                        case Act::SetAngle: {
                            size_t tn2 = 0;
                            const char* to = opt_str(L, e, "towards", tn2);
                            se.towards = Towards::Target;
                            if (to != nullptr) {
                                if (streq(to, tn2, "facing"))        se.towards = Towards::Facing;
                                else if (streq(to, tn2, "away"))     se.towards = Towards::Away;
                                else if (streq(to, tn2, "random"))   se.towards = Towards::Random;
                                else if (streq(to, tn2, "angle"))    se.towards = Towards::Angle;
                                else if (streq(to, tn2, "var"))      se.towards = Towards::Var;
                                else if (streq(to, tn2, "movement")) se.towards = Towards::Movement;
                                lua_pop(L, 1);
                            }
                            se.i = opt_int(L, e, "angle", 0);
                            se.offset = opt_int(L, e, "offset", 0);
                            se.jitter = opt_int(L, e, "jitter", -1);
                            size_t vn2 = 0;
                            const char* v2 = opt_str(L, e, "var", vn2);
                            if (v2 != nullptr) {
                                se.var = static_cast<i16>(g.var_slot(k, v2, vn2));
                                lua_pop(L, 1);
                            }
                            break;
                        }
                        case Act::Lua:
                            break;
                        }
                    }
                    g.sequence.push_back(se);
                    ++st.seq_count;
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }

        // transitions, in declaration order -- which IS the priority
        st.first_trans = static_cast<u16>(g.transitions.size());
        st.trans_count = 0;
        if (field(L, st_idx, "transitions")) {
            const int tr = lua_gettop(L);
            const int tn = static_cast<int>(lua_objlen(L, tr));
            for (int i = 1; i <= tn; ++i) {
                lua_rawgeti(L, tr, i);
                if (lua_istable(L, -1)) {
                    const int t = lua_gettop(L);
                    Transition tt;
                    {
                        size_t n = 0;
                        const char* to = opt_str(L, t, "to", n);
                        tt.to = to ? static_cast<u16>(g.state_by_name(k, to, n)) : 0;
                        if (to != nullptr) lua_pop(L, 1);
                    }
                    tt.first_cond = static_cast<u16>(g.conditions.size());
                    tt.cond_count = 0;
                    if (field(L, t, "when")) {
                        const int wl = lua_gettop(L);
                        const int wn = static_cast<int>(lua_objlen(L, wl));
                        for (int j = 1; j <= wn; ++j) {
                            lua_rawgeti(L, wl, j);
                            if (lua_istable(L, -1)) {
                                const int c = lua_gettop(L);
                                Condition cc;
                                size_t n = 0;
                                const char* name = opt_str(L, c, "cond", n);
                                if (name != nullptr) {
                                    cc.id = cond_by_name(name, n);
                                    if (cc.id == Cond::Unsupported && k.supported()) {
                                        k.unsupported.assign(name, n);
                                    }
                                    lua_pop(L, 1);
                                }
                                cc.negate = opt_bool(L, c, "negate", false);
                                // `by` on the height pair uses the same fx
                                // slot as `distance`; no condition carries
                                // both, so reading each in turn cannot clash.
                                if (!opt_fx(L, c, "distance", cc.f)) opt_fx(L, c, "by", cc.f);
                                // One integer covers every condition that takes
                                // one; they are mutually exclusive by argument
                                // name, so reading each in turn cannot collide.
                                cc.i = opt_int(L, c, "percent",
                                       opt_int(L, c, "ticks",
                                       opt_int(L, c, "value",
                                       opt_int(L, c, "arc",
                                       opt_int(L, c, "within", 0)))));
                                {
                                    size_t vn = 0;
                                    const char* v = opt_str(L, c, "var", vn);
                                    if (v != nullptr) {
                                        cc.var = static_cast<i16>(g.var_slot(k, v, vn));
                                        lua_pop(L, 1);
                                    }
                                }
                                // A mob kind or a catalogue record. Interned
                                // rather than resolved: a mob name means a
                                // kind in this same graph, and a prop name
                                // means a record in a file this subsystem
                                // never opens -- so both stay names and the
                                // comparison is by name at evaluation.
                                {
                                    size_t nn = 0;
                                    const char* nm = opt_str(L, c, "mob", nn);
                                    if (nm == nullptr) nm = opt_str(L, c, "prop", nn);
                                    if (nm != nullptr) {
                                        cc.name = g.intern(nm, nn);
                                        lua_pop(L, 1);
                                    }
                                }
                                if (field(L, c, "alive")) {
                                    cc.flag = lua_toboolean(L, -1) != 0;
                                    lua_pop(L, 1);
                                }
                                // `at_least` reuses the integer, and is read
                                // after the others so a condition carrying
                                // both cannot collide -- none does.
                                cc.i = opt_int(L, c, "at_least", cc.i);
                                g.conditions.push_back(cc);
                                ++tt.cond_count;
                            }
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 1);
                    }
                    g.transitions.push_back(tt);
                    ++st.trans_count;
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }

        lua_pop(L, 1);   // the state table
    }

    // `initial`, resolved last for the same reason `next` was: every name has
    // to exist before any of them can become an index.
    {
        size_t n = 0;
        const char* s = opt_str(L, def_idx, "initial", n);
        k.initial = s ? static_cast<i16>(g.state_by_name(k, s, n)) : -1;
        if (s != nullptr) lua_pop(L, 1);
    }

    k.hp = opt_int(L, def_idx, "hp", 0);
    k.has_sight = opt_fx(L, def_idx, "sight", k.sight);
    k.flying = opt_bool(L, def_idx, "flying", false);
    opt_fx(L, def_idx, "hover_z", k.hover_z);
    opt_fx(L, def_idx, "jump_height", k.jump_height);

    lua_pop(L, 1);   // states
    return true;
}

}  // namespace dnp::server::mobs
