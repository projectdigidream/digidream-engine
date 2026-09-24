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
    return Cond::Unsupported;
}

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
            st.next = s ? static_cast<i16>(g.state_by_name(k, s, n)) : -1;
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
                        if (streq(act, n, "goto")) {
                            lua_pop(L, 1);
                            size_t sn2 = 0;
                            const char* to = opt_str(L, e, "state", sn2);
                            if (to != nullptr) {
                                se.goto_state = static_cast<i16>(g.state_by_name(k, to, sn2));
                                lua_pop(L, 1);
                            }
                        } else {
                            lua_pop(L, 1);
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
                                opt_fx(L, c, "distance", cc.f);
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
