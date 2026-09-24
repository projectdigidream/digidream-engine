//
// The `dnp.mobs_*` bindings: how a level's graph gets from Lua into C++, and
// how anything can find out what happened to it.
//
// ONE GRAPH PER lua_State, because one VM hosts one level's scripts and a
// level has one mobs.load. Keyed off the state pointer rather than held in
// WorldContext so that nothing in the existing binding layer has to know this
// file exists yet; when the decision loop lands it moves next to the context.
//
#include "mobs/mob_runtime.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>
#include <map>

namespace dnp::server::mobs {

namespace {

std::map<lua_State*, MobGraph>& graphs() {
    static std::map<lua_State*, MobGraph> g;
    return g;
}

// dnp.mobs_compile(defs) -> compiled, skipped, problems
//
// `defs` is mobs.defs, AFTER mobs.load has validated it. Returns how many
// kinds compiled, how many are on the Lua path, and a table of one string per
// skipped kind saying which condition kept it there.
int l_mobs_compile(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    MobGraph& g = graph_of(L);
    g.clear();

    int skipped = 0;
    lua_newtable(L);                 // problems
    int nprob = 0;

    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        // key at -2 (the mob's name), value at -1 (its definition)
        if (lua_isstring(L, -2) && lua_istable(L, -1)) {
            size_t n = 0;
            const char* name = lua_tolstring(L, -2, &n);

            Kind k;
            k.name = g.intern(name, n);
            std::string err;
            if (compile_kind(L, -1, g, k, err)) {
                if (!k.supported()) {
                    ++skipped;
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                                  "%s: stays on the Lua path -- uses `%s`",
                                  g.str(k.name), k.unsupported.c_str());
                    lua_pushstring(L, buf);
                    lua_rawseti(L, -4, ++nprob);
                }
                g.kinds.push_back(std::move(k));
            } else {
                ++skipped;
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s: %s", g.str(k.name), err.c_str());
                lua_pushstring(L, buf);
                lua_rawseti(L, -4, ++nprob);
            }
        }
        lua_pop(L, 1);
    }

    // The problems table is below the two numbers, so push them after it and
    // reorder -- the alternative is building it last, which would mean walking
    // `defs` twice.
    lua_pushnumber(L, static_cast<lua_Number>(g.kinds.size()) - skipped);
    lua_pushnumber(L, skipped);
    lua_insert(L, -3);
    lua_insert(L, -3);
    return 3;
}

// dnp.mobs_graph(name) -> a table describing what C++ compiled for that kind
//
// FOR THE TEST, and for nothing else. The whole claim of this file is that the
// graph in C++ is the graph in Lua, and a claim with no way to look at both
// sides is not one anybody can check. So this reports the compiled form in the
// terms the Lua tables use -- state names, transition targets, condition names
// and their arguments -- and the test walks the two together.
//
// Deliberately NOT the internal indices: a comparison in terms of indices this
// file chose would agree with itself by construction.
int l_mobs_graph(lua_State* L) {
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    MobGraph& g = graph_of(L);
    const i32 ki = g.kind_by_name(name, n);
    if (ki < 0) { lua_pushnil(L); return 1; }
    const Kind& k = g.kinds[static_cast<u32>(ki)];

    static const char* const COND_NAME[] = {
        "has_target", "target_within", "target_visible", "target_in_front",
        "hp_below", "state_elapsed", "chance", "var_at_least", "var_is",
        "cooldown_ready", "grounded", "was_hurt", "hurt_by_mob", "unsupported",
    };
    static const char* const MOVE_NAME[] = {
        "stop", "chase", "flee", "strafe", "wander", "free",
    };

    lua_createtable(L, 0, 6);
    lua_pushnumber(L, k.hp);                    lua_setfield(L, -2, "hp");
    lua_pushboolean(L, k.flying);               lua_setfield(L, -2, "flying");
    lua_pushboolean(L, k.supported());          lua_setfield(L, -2, "supported");
    lua_pushstring(L, k.unsupported.c_str());   lua_setfield(L, -2, "unsupported");
    if (k.initial >= 0) {
        lua_pushstring(L, g.str(g.states[k.first_state + k.initial].name));
        lua_setfield(L, -2, "initial");
    }
    lua_pushnumber(L, k.var_count);             lua_setfield(L, -2, "var_count");

    lua_createtable(L, 0, static_cast<int>(k.state_count));
    for (u16 si = 0; si < k.state_count; ++si) {
        const State& st = g.states[k.first_state + si];
        lua_createtable(L, 0, 6);
        lua_pushnumber(L, st.anim);  lua_setfield(L, -2, "anim");
        lua_pushnumber(L, st.ticks); lua_setfield(L, -2, "ticks");
        lua_pushstring(L, MOVE_NAME[static_cast<u8>(st.move)]);
        lua_setfield(L, -2, "move");
        lua_pushstring(L, st.face == Face::Target ? "target" : "hold");
        lua_setfield(L, -2, "face");
        lua_pushnumber(L, st.solid); lua_setfield(L, -2, "solid");
        if (st.next >= 0) {
            lua_pushstring(L, g.str(g.states[k.first_state + st.next].name));
            lua_setfield(L, -2, "next");
        }

        lua_createtable(L, static_cast<int>(st.trans_count), 0);
        for (u16 ti = 0; ti < st.trans_count; ++ti) {
            const Transition& t = g.transitions[st.first_trans + ti];
            lua_createtable(L, 0, 2);
            lua_pushstring(L, g.str(g.states[k.first_state + t.to].name));
            lua_setfield(L, -2, "to");
            lua_createtable(L, static_cast<int>(t.cond_count), 0);
            for (u16 ci = 0; ci < t.cond_count; ++ci) {
                const Condition& c = g.conditions[t.first_cond + ci];
                lua_createtable(L, 0, 4);
                lua_pushstring(L, COND_NAME[static_cast<u8>(c.id)]);
                lua_setfield(L, -2, "cond");
                lua_pushboolean(L, c.negate); lua_setfield(L, -2, "negate");
                lua_pushnumber(L, c.i);       lua_setfield(L, -2, "i");
                // As a RAW, because the point of comparing is exactness and a
                // double would round the very thing being compared.
                lua_pushstring(L, std::to_string(c.f.raw).c_str());
                lua_setfield(L, -2, "f_raw");
                lua_pushnumber(L, c.var);     lua_setfield(L, -2, "var");
                lua_rawseti(L, -2, ci + 1);
            }
            lua_setfield(L, -2, "when");
            lua_rawseti(L, -2, ti + 1);
        }
        lua_setfield(L, -2, "transitions");

        lua_createtable(L, static_cast<int>(st.seq_count), 0);
        for (u16 qi = 0; qi < st.seq_count; ++qi) {
            const SeqEntry& e = g.sequence[st.first_seq + qi];
            lua_createtable(L, 0, 3);
            lua_pushnumber(L, e.at);        lua_setfield(L, -2, "at");
            lua_pushnumber(L, e.lua_index); lua_setfield(L, -2, "lua_index");
            static const char* const ACT_NAME[] = {
                "set_var", "add_var", "set_cooldown", "set_anim", "goto", "face", "lua",
            };
            lua_pushstring(L, ACT_NAME[static_cast<u8>(e.id)]);
            lua_setfield(L, -2, "action");
            lua_pushnumber(L, e.i);   lua_setfield(L, -2, "i");
            lua_pushnumber(L, e.var); lua_setfield(L, -2, "var");
            if (e.state >= 0) {
                lua_pushstring(L, g.str(g.states[k.first_state + e.state].name));
                lua_setfield(L, -2, "goto_state");
            }
            lua_rawseti(L, -2, qi + 1);
        }
        lua_setfield(L, -2, "sequence");

        lua_setfield(L, -2, g.str(st.name));
    }
    lua_setfield(L, -2, "states");
    return 1;
}

const luaL_Reg MOB_FUNCS[] = {
    {"mobs_compile", l_mobs_compile},
    {"mobs_graph", l_mobs_graph},
    {nullptr, nullptr},
};

}  // namespace

MobGraph& graph_of(lua_State* L) { return graphs()[L]; }

void open_mobs(lua_State* L) {
    for (const luaL_Reg* r = MOB_FUNCS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

}  // namespace dnp::server::mobs
