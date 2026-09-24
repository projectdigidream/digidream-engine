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
#include "lua/world_binding.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>

namespace dnp::server::mobs {

namespace {

std::map<lua_State*, MobRuntime>& runtimes() {
    static std::map<lua_State*, MobRuntime> r;
    return r;
}

// The world this VM is attached to, read the way world_binding.cpp reads it.
//
// Through the registry rather than through a parameter, because that is where
// the host already put it and a second copy would be a second answer to which
// world a script is talking about. Null when nothing is attached, which is the
// Lua-only test harness -- and every caller here checks, because a mob
// framework that needed an engine to LOAD would be untestable without one.
lua::WorldContext* ctx_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "dnp_world_ctx");
    auto* c = static_cast<lua::WorldContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return (c && c->world) ? c : nullptr;
}

// Point the runtime at whatever the context currently holds.
//
// EVERY TICK, not once: a level's nav graph is rebuilt when a shape commits,
// and a runtime holding the old pointer would path over a map that no longer
// exists. Cheap enough that caching it would be an optimisation of four stores.
MobRuntime& runtime_of(lua_State* L) {
    MobRuntime& rt = runtimes()[L];
    if (lua::WorldContext* c = ctx_of(L)) {
        rt.world = c->world;
        rt.nav = c->nav;
        rt.commands = c->commands;
        rt.rng = &c->rng;
    }
    return rt;
}

// Actions the loop does not run, handed to maps/scripts/mobs.lua.
//
// It calls ONE function -- mobs.run_action(entity_index, state, i) -- which
// looks the mob up in its own `live` table and calls the same `acts` entry it
// always did. So there is still exactly one implementation of `spawn`, and it
// is the one that has always been there.
struct LuaActions : ActionSink {
    lua_State* L = nullptr;
    u32 tick = 0;
    u64 errors = 0;

    void run(u32 entity, const char* state_name, u16 seq_index) override {
        lua_getglobal(L, "mobs");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
        lua_getfield(L, -1, "run_action");
        if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
        lua_pushnumber(L, entity);
        lua_pushstring(L, state_name);
        lua_pushnumber(L, seq_index);
        // PROTECTED, and the error is counted rather than propagated. One
        // script's broken action must not take the rest of the bucket with it
        // -- which is the failure mobs.lua already names, where a typo in one
        // monster's hitscan killed on_tick and froze every projectile on the
        // level.
        if (lua_pcall(L, 3, 0, 0) != 0) {
            ++errors;
            std::fprintf(stderr, "[mobs] action %s[%u]: %s\n",
                         state_name, seq_index, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);   // the mobs table
    }
};

// dnp.mobs_compile(defs) -> compiled, skipped, problems
//
// `defs` is mobs.defs, AFTER mobs.load has validated it. Returns how many
// kinds compiled, how many are on the Lua path, and a table of one string per
// skipped kind saying which condition kept it there.
int l_mobs_compile(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Reset FIRST and take the reference after: a new graph means every live
    // mob's kind index is meaningless, and reset() is what drops them. The
    // same rule mobs.lua's "called twice" refusal enforces one level up.
    runtimes()[L].reset();
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

// --- driving the loop from mobs.lua -----------------------------------------

// dnp.mob_track(entity, kind, dormant) -> true if the C++ loop took it
//
// FALSE IS NOT AN ERROR. A kind whose graph uses something the loop does not
// implement stays entirely in Lua, and the caller keeps its own record either
// way -- so the two runtimes never both own one mob.
int l_mob_track(lua_State* L) {
    // DNP_MOBS_LUA=1 refuses everything, which puts every mob back on the Lua
    // path without a map or a script changing.
    //
    // FOR THE COMPARISON, and for nothing else. The two runtimes are meant to
    // decide the same things, and a claim like that is worth exactly what the
    // ability to run the other side of it is worth. Not an operator setting:
    // there is one right answer here and it is measured, not configured. Read
    // once, because getenv in a spawn path is a syscall per monster.
    static const bool force_lua = [] {
        const char* v = std::getenv("DNP_MOBS_LUA");
        return v != nullptr && v[0] == '1';
    }();
    if (force_lua) { lua_pushboolean(L, 0); return 1; }

    const u32 e = static_cast<u32>(luaL_checknumber(L, 1));
    size_t n = 0;
    const char* kind = luaL_checklstring(L, 2, &n);
    const bool dormant = lua_toboolean(L, 3) != 0;
    MobRuntime& rt = runtime_of(L);
    if (rt.world == nullptr) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, track(rt, e, kind, n, dormant, rt.world->tick()) ? 1 : 0);
    return 1;
}

int l_mob_untrack(lua_State* L) {
    untrack(runtime_of(L), static_cast<u32>(luaL_checknumber(L, 1)));
    return 0;
}

// dnp.mobs_tick() -> thought, lua_actions
//
// One bucket's worth of decisions. Called from mobs.lua's on_tick, which keeps
// doing its own missiles and corpses either way: those are not per-mob
// decisions and there was never anything to move.
int l_mobs_tick(lua_State* L) {
    MobRuntime& rt = runtime_of(L);
    if (rt.world == nullptr) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 2; }
    LuaActions sink;
    sink.L = L;
    sink.tick = rt.world->tick();
    tick_mobs(rt, rt.world->tick(), &sink);
    lua_pushnumber(L, rt.thought);
    lua_pushnumber(L, rt.lua_actions);
    return 2;
}

// dnp.mob_enter(entity, state) -> true if it moved
//
// For the things outside the loop that move a mob: `hurt` sending it to pain
// or death, and `revive` putting a corpse back on its feet.
int l_mob_enter(lua_State* L) {
    const u32 e = static_cast<u32>(luaL_checknumber(L, 1));
    size_t n = 0;
    const char* st = luaL_checklstring(L, 2, &n);
    MobRuntime& rt = runtime_of(L);
    if (rt.world == nullptr) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, mob_enter(rt, e, st, n, rt.world->tick()) ? 1 : 0);
    return 1;
}

// dnp.mob_state(entity) -> state name, hp, target entity index or nil, aim
//
// What a Lua action needs to read about a mob the loop owns. Four values
// rather than a table, because this is called per action and a table is an
// allocation the collector then has to chase -- the cost this whole subsystem
// exists to remove.
int l_mob_state(lua_State* L) {
    MobRuntime& rt = runtime_of(L);
    Mob* m = mob_of(rt, static_cast<u32>(luaL_checknumber(L, 1)));
    if (!m || m->kind < 0) return 0;
    const Kind& k = rt.graph.kinds[static_cast<u32>(m->kind)];
    if (m->state >= 0) {
        lua_pushstring(L, rt.graph.str(rt.graph.states[k.first_state + m->state].name));
    } else {
        lua_pushnil(L);
    }
    lua_pushnumber(L, m->hp);
    if (m->target >= 0) lua_pushnumber(L, m->target); else lua_pushnil(L);
    lua_pushnumber(L, m->aim);
    return 4;
}

// dnp.mob_set(entity, field, value)
//
// The few fields a Lua action still writes: hp and dead, which `hurt` owns,
// and the aim, which `face` in an on_hit can move. Named rather than
// positional so adding one does not renumber the others.
int l_mob_set(lua_State* L) {
    MobRuntime& rt = runtime_of(L);
    Mob* m = mob_of(rt, static_cast<u32>(luaL_checknumber(L, 1)));
    if (!m || m->kind < 0) return 0;
    const char* field = luaL_checkstring(L, 2);
    if (std::strcmp(field, "hp") == 0)          m->hp = static_cast<i32>(luaL_checknumber(L, 3));
    else if (std::strcmp(field, "dead") == 0)   m->dead = lua_toboolean(L, 3) != 0;
    else if (std::strcmp(field, "aim") == 0)    m->aim = static_cast<u32>(luaL_checknumber(L, 3));
    else if (std::strcmp(field, "dormant") == 0) m->dormant = lua_toboolean(L, 3) != 0;
    else if (std::strcmp(field, "target") == 0) {
        if (lua_isnil(L, 3)) { m->target = -1; }
        else {
            m->target = static_cast<i32>(luaL_checknumber(L, 3));
            if (rt.world && m->target >= 0) {
                m->target_gen = rt.world->generation(static_cast<u32>(m->target));
            }
        }
    } else if (std::strcmp(field, "hurt") == 0) {
        // One call for the whole pain edge, because the three move together:
        // when it happened, and whether a mob did it.
        m->hurt_ever = true;
        if (rt.world) m->hurt_at = rt.world->tick();
        m->hurt_by_mob = lua_toboolean(L, 3) != 0;
    } else {
        return luaL_error(L, "dnp.mob_set: no field '%s'", field);
    }
    return 0;
}

// dnp.mob_var(entity, name [, value]) -> the value
//
// A read with two arguments and a write with three, which is how `entity:hp`
// and the rest of this binding layer already read and write.
int l_mob_var(lua_State* L) {
    MobRuntime& rt = runtime_of(L);
    Mob* m = mob_of(rt, static_cast<u32>(luaL_checknumber(L, 1)));
    if (!m || m->kind < 0) { lua_pushnil(L); return 1; }
    size_t n = 0;
    const char* name = luaL_checklstring(L, 2, &n);
    i32* v = mob_var(rt, *m, name, n);
    if (!v) { lua_pushnil(L); return 1; }
    if (!lua_isnoneornil(L, 3)) *v = static_cast<i32>(luaL_checknumber(L, 3));
    lua_pushnumber(L, *v);
    return 1;
}

// dnp.mobs_reset() -- a level torn down and rebuilt.
//
// `live` is keyed by entity index and a new level's indices start again from
// the bottom, so a stale record is not merely dead weight: it is one that will
// be found for somebody ELSE'S entity. mobs.lua's M.reset says the same thing
// about its own tables.
int l_mobs_reset(lua_State* L) {
    runtime_of(L).reset();
    return 0;
}

// dnp.mobs_stats() -> tracked, thought, lua_actions, routes_truncated
int l_mobs_stats(lua_State* L) {
    MobRuntime& rt = runtime_of(L);
    u32 live = 0;
    for (const Mob& m : rt.mobs) if (m.kind >= 0) ++live;
    lua_pushnumber(L, live);
    lua_pushnumber(L, rt.thought);
    lua_pushnumber(L, rt.lua_actions);
    lua_pushnumber(L, rt.routes_truncated);
    return 4;
}

const luaL_Reg MOB_FUNCS[] = {
    {"mobs_compile", l_mobs_compile},
    {"mobs_graph", l_mobs_graph},
    {"mobs_tick", l_mobs_tick},
    {"mobs_reset", l_mobs_reset},
    {"mobs_stats", l_mobs_stats},
    {"mob_track", l_mob_track},
    {"mob_untrack", l_mob_untrack},
    {"mob_enter", l_mob_enter},
    {"mob_state", l_mob_state},
    {"mob_set", l_mob_set},
    {"mob_var", l_mob_var},
    {nullptr, nullptr},
};

}  // namespace

// ONE GRAPH, and it is the runtime's own.
//
// This was two for about an hour -- a `graphs()` map beside the runtimes, so
// the compiler filled one and the loop walked the other, which is empty. It
// would have presented as every mob standing still with nothing reporting
// anything, because an empty graph is not an error, it is a level with no
// monsters defined. The same "two programs, one fact" shape this whole
// subsystem is careful about, one layer down.
MobGraph& graph_of(lua_State* L) { return runtimes()[L].graph; }

void open_mobs(lua_State* L) {
    for (const luaL_Reg* r = MOB_FUNCS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

}  // namespace dnp::server::mobs
