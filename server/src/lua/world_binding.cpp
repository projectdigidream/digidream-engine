#include "lua/world_binding.hpp"

#include <cstring>

#include "dnp/proto/messages.hpp"
#include "dnp/physics/params_table.hpp"
#include "lua/fx_binding.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace dnp::server::lua {

namespace {

const char* const ENTITY_MT = "dnp.entity";
const char* const CTX_KEY   = "dnp_world_ctx";

WorldContext& ctx_of(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, CTX_KEY);
    auto* c = static_cast<WorldContext*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!c || !c->world) luaL_error(L, "dnp: no world attached");
    return *c;
}

// The handle, stored exactly as it goes on the wire (protocol §9): index+1 in
// the low 24 bits, generation in the high 8. Same value in Lua, in a snapshot
// and in the server, so there is never a second encoding to keep in step.
proto::Handle* to_handle(lua_State* L, int idx) {
    return static_cast<proto::Handle*>(luaL_testudata(L, idx, ENTITY_MT));
}

proto::Handle check_handle(lua_State* L, int idx) {
    if (proto::Handle* h = to_handle(L, idx)) return *h;
    luaL_error(L, "expected an entity, got %s", luaL_typename(L, idx));
    return proto::Handle::none();
}

// Resolve a handle to a live agent index, or -1.
//
// This is the single place §8.7's rule is implemented, so every binding
// inherits it rather than each remembering to check.
i32 resolve(lua_State* L, WorldContext& ctx, proto::Handle h) {
    if (!h.valid()) return -1;
    const u32 idx = h.index();
    if (idx == 0 || idx > sim::MAX_AGENTS) return -1;
    const u32 agent = idx - 1;
    if (!ctx.world->agent(agent).active) { ++ctx.stats.stale_handle_ops; return -1; }
    if (ctx.world->generation(agent) != h.generation()) {
        // The slot was reused. Not an error: holding a handle to something that
        // has since died is ordinary, and raising here would make every script
        // wrap every call in pcall.
        ++ctx.stats.stale_handle_ops;
        return -1;
    }
    (void)L;
    return static_cast<i32>(agent);
}

// --------------------------------------------------------------- entity verbs

int l_entity_valid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_pushboolean(L, resolve(L, ctx, check_handle(L, 1)) >= 0);
    return 1;
}

int l_entity_pos(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    // Reads are immediate: the live world, not a queued view of it.
    push_vec3(L, ctx.world->agent(static_cast<u32>(a)).pos);
    return 1;
}

int l_entity_vel(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    push_vec3(L, ctx.world->agent(static_cast<u32>(a)).vel);
    return 1;
}

int l_entity_grounded(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    lua_pushboolean(L, a >= 0 && ctx.world->agent(static_cast<u32>(a)).mv.grounded);
    return 1;
}

int l_entity_sector(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->agent(static_cast<u32>(a)).mv.sector);
    return 1;
}

int l_entity_index_of(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, a);
    return 1;
}

// Writes are deferred. Named as verbs, never as assignments, because a script
// that reads the value back inside the same callback will see the old one.
int l_entity_teleport(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;                          // defined no-op
    const fx3 p = check_vec3(L, 2);
    if (ctx.commands->push(Command{CmdKind::Teleport, static_cast<u32>(a), p})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

int l_entity_add_impulse(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    const fx3 v = check_vec3(L, 2);
    // Relative, and that is part of the contract: two effects both pushing this
    // tick both take effect.
    if (ctx.commands->push(Command{CmdKind::AddImpulse, static_cast<u32>(a), v})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

int l_entity_set_velocity(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    const fx3 v = check_vec3(L, 2);
    if (ctx.commands->push(Command{CmdKind::SetVelocity, static_cast<u32>(a), v})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// --------------------------------------------------------------- parameters

int l_entity_param(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    const char* name = luaL_checkstring(L, 2);
    const i32 id = physics::param_id_by_name(name);
    if (id < 0) return luaL_error(L, "no movement parameter named '%s'", name);
    // The EFFECTIVE value -- base plus whatever is currently modifying it,
    // which is what the physics will actually use this tick.
    push_fx(L, fx::from_raw(physics::param_get_raw(ctx.world->params(static_cast<u32>(a)),
                                                   static_cast<u16>(id))));
    return 1;
}

int l_entity_base_param(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || !ctx.effects) { lua_pushnil(L); return 1; }
    const char* name = luaL_checkstring(L, 2);
    const i32 id = physics::param_id_by_name(name);
    if (id < 0) return luaL_error(L, "no movement parameter named '%s'", name);
    push_fx(L, fx::from_raw(physics::param_get_raw(ctx.effects->base(static_cast<u32>(a)),
                                                   static_cast<u16>(id))));
    return 1;
}

// entity:add_effect(field, op, value [, ticks]) -> id
//
// `op` is "mul", "add" or "set". There is no priority argument, deliberately:
// design §6 leaves stacking to the game, and modifiers compose in the order
// they were added. Returns 0 if the table is full, which a script may check.
int l_entity_add_effect(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || !ctx.effects) { lua_pushnumber(L, 0); return 1; }

    const char* name = luaL_checkstring(L, 2);
    const i32 id = physics::param_id_by_name(name);
    if (id < 0) return luaL_error(L, "no movement parameter named '%s'", name);

    const char* op_s = luaL_checkstring(L, 3);
    EffectOp op;
    if (std::strcmp(op_s, "mul") == 0)      op = EffectOp::Mul;
    else if (std::strcmp(op_s, "add") == 0) op = EffectOp::Add;
    else if (std::strcmp(op_s, "set") == 0) op = EffectOp::Set;
    else return luaL_error(L, "effect op must be 'mul', 'add' or 'set', got '%s'", op_s);

    const fx value = check_fx(L, 4);
    const u32 ticks = lua_isnoneornil(L, 5) ? 0u
                                            : static_cast<u32>(luaL_checknumber(L, 5));
    const u32 expires = ticks == 0 ? 0u : ctx.world->tick() + ticks;

    const u32 eid = ctx.effects->add(static_cast<u32>(a), static_cast<u16>(id), op,
                                     value, expires);
    if (eid != 0) ctx.effects_dirty = true;
    lua_pushnumber(L, eid);
    return 1;
}

int l_entity_remove_effect(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (!ctx.effects) { lua_pushboolean(L, 0); return 1; }
    // Not gated on the handle: an effect outliving the entity that carried it
    // still needs removing, and refusing here would leak table slots.
    const u32 eid = static_cast<u32>(luaL_checknumber(L, 2));
    const bool removed = ctx.effects->remove(eid);
    if (removed) ctx.effects_dirty = true;
    lua_pushboolean(L, removed);
    return 1;
}

int l_entity_eq(lua_State* L) {
    lua_pushboolean(L, check_handle(L, 1) == check_handle(L, 2));
    return 1;
}

int l_entity_tostring(lua_State* L) {
    const proto::Handle h = check_handle(L, 1);
    // Never the address: a heap pointer in script output is an ASLR-derived
    // value that differs between runs for no reason anyone can act on.
    lua_pushfstring(L, "entity(%d, gen %d)", static_cast<int>(h.index()),
                    static_cast<int>(h.generation()));
    return 1;
}

const luaL_Reg ENTITY_METHODS[] = {
    {"valid", l_entity_valid},
    {"pos", l_entity_pos},
    {"vel", l_entity_vel},
    {"grounded", l_entity_grounded},
    {"sector", l_entity_sector},
    {"index", l_entity_index_of},
    {"teleport", l_entity_teleport},
    {"add_impulse", l_entity_add_impulse},
    {"set_velocity", l_entity_set_velocity},
    {"param", l_entity_param},
    {"base_param", l_entity_base_param},
    {"add_effect", l_entity_add_effect},
    {"remove_effect", l_entity_remove_effect},
    {nullptr, nullptr},
};

// ------------------------------------------------------------------ dnp table

int l_dnp_tick(lua_State* L) {
    lua_pushnumber(L, ctx_of(L).world->tick());
    return 1;
}

int l_dnp_entity(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (i < 0 || static_cast<u32>(i) >= sim::MAX_AGENTS) { lua_pushnil(L); return 1; }
    push_entity(L, ctx, static_cast<u32>(i));
    return 1;
}

int l_dnp_entities(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    int n = 0;
    // Ascending index order, always. Any order would be deterministic on its
    // own, but a script iterating entities must see the same order the server's
    // own loops use, or a replay of the command log stops matching.
    for (u32 i = 0; i < sim::MAX_AGENTS; ++i) {
        if (!ctx.world->agent(i).active) continue;
        push_entity(L, ctx, i);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int l_dnp_stale_ops(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(ctx_of(L).stats.stale_handle_ops));
    return 1;
}

const luaL_Reg DNP_FUNCS[] = {
    {"tick", l_dnp_tick},
    {"entity", l_dnp_entity},
    {"entities", l_dnp_entities},
    {"stale_handle_ops", l_dnp_stale_ops},
    {nullptr, nullptr},
};

}  // namespace

void push_entity(lua_State* L, WorldContext& ctx, u32 index) {
    if (index >= sim::MAX_AGENTS || !ctx.world->agent(index).active) {
        lua_pushnil(L);
        return;
    }
    void* p = lua_newuserdata(L, sizeof(proto::Handle));
    // index+1, matching main.cpp and the wire: raw 0 means "no entity", and a
    // slot-0 agent at generation 0 would otherwise encode to exactly that.
    *static_cast<proto::Handle*>(p) =
        proto::Handle::make(index + 1, ctx.world->generation(index));
    luaL_getmetatable(L, ENTITY_MT);
    lua_setmetatable(L, -2);
}

void open_world(lua_State* L, WorldContext* ctx) {
    lua_pushlightuserdata(L, ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, CTX_KEY);

    luaL_newmetatable(L, ENTITY_MT);
    lua_newtable(L);
    for (const luaL_Reg* r = ENTITY_METHODS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_entity_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_entity_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "locked");            lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    lua_newtable(L);
    for (const luaL_Reg* r = DNP_FUNCS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setglobal(L, "dnp");
}

}  // namespace dnp::server::lua
