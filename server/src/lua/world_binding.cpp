#include "lua/world_binding.hpp"

#include <cstring>

#include "dnp/proto/messages.hpp"
#include "dnp/sim/sectormotion.hpp"
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

// entity:change_map([index]) -> true if the request was accepted
//
// With no argument, the next level in the server's progression -- which is the
// order the maps were given on the command line. With an index, that level.
//
// DEFERRED like every other mutation here, and more so: this one despawns the
// entity from the world the calling script belongs to. Applied after every
// level has stepped (see Server::tick), so a script never pulls a body out from
// under the tick that is iterating it.
//
// A stale handle is the usual defined no-op: a player who disconnected between
// touching the exit and the flush point is not an error.
int l_entity_change_map(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushboolean(L, 0); return 1; }
    if (ctx.map_changes == nullptr) {
        // No progression configured -- one map, or a test harness. Saying so
        // beats a silent no-op, because "the exit does nothing" is exactly what
        // a broken map script looks like.
        lua_pushboolean(L, 0);
        return 1;
    }
    const i32 to = lua_isnoneornil(L, 2) ? -1 : static_cast<i32>(luaL_checknumber(L, 2));
    ctx.map_changes->request_map_change(ctx.level, static_cast<u32>(a), to);
    lua_pushboolean(L, 1);
    return 1;
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
    {"change_map", l_entity_change_map},
    {"add_impulse", l_entity_add_impulse},
    {"set_velocity", l_entity_set_velocity},
    {"param", l_entity_param},
    {"base_param", l_entity_base_param},
    {"add_effect", l_entity_add_effect},
    {"remove_effect", l_entity_remove_effect},
    {nullptr, nullptr},
};

// ----------------------------------------------------------------------- props
//
// A separate handle type from an entity, deliberately. A prop has no velocity,
// no controller and no parameters, and folding it into the agent methods would
// mean every one of them growing a "unless it is actually a key" branch -- which
// is the shape a type is for.
//
// Generational all the same: a script that picked up a key and kept the handle
// is holding something that no longer exists, and that has to be a defined
// no-op rather than a slot that quietly refers to the next key spawned there.

const char* const PROP_MT = "dnp.prop";

proto::Handle* to_prop(lua_State* L, int idx) {
    return static_cast<proto::Handle*>(luaL_testudata(L, idx, PROP_MT));
}

proto::Handle check_prop_handle(lua_State* L, int idx) {
    if (proto::Handle* h = to_prop(L, idx)) return *h;
    luaL_error(L, "expected a prop, got %s", luaL_typename(L, idx));
    return proto::Handle{};
}

// Entity index for a live prop, or -1.
i32 resolve_prop(lua_State* L, WorldContext& ctx, proto::Handle h) {
    (void)L;
    if (!h.valid()) return -1;
    const u32 idx = h.index();
    if (idx == 0) return -1;
    const u32 entity = idx - 1;
    if (!sim::World::is_prop(entity)) return -1;
    if (!ctx.world->prop(entity).active) { ++ctx.stats.stale_handle_ops; return -1; }
    if (ctx.world->generation(entity) != h.generation()) {
        ++ctx.stats.stale_handle_ops;
        return -1;
    }
    return static_cast<i32>(entity);
}

int l_prop_valid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_pushboolean(L, resolve_prop(L, ctx, check_prop_handle(L, 1)) >= 0);
    return 1;
}

int l_prop_index(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(check_prop_handle(L, 1).index()) - 1);
    return 1;
}

int l_prop_kind(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->prop(static_cast<u32>(e)).kind);
    return 1;
}

int l_prop_pos(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) { lua_pushnil(L); return 1; }
    push_vec3(L, ctx.world->prop(static_cast<u32>(e)).pos);
    return 1;
}

// prop:set_animation(n)
//
// The STATE, never the timing. Which frame is on screen and how a mesh blends
// between clips is the client's decision (design §4) -- a server that shipped
// frame numbers would be deciding the frame rate of a machine it has never
// seen, and would have to be right about it sixty times a second.
int l_prop_set_animation(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) return 0;
    const int n = static_cast<int>(luaL_checknumber(L, 2));
    if (n < 0 || n > 65535) return luaL_error(L, "animation state %d is out of range", n);
    ctx.world->prop_mut(static_cast<u32>(e)).animation_state = static_cast<u16>(n);
    return 0;
}

// prop:despawn()
//
// Immediate, unlike every entity mutation beside it. A prop is not part of the
// physics resolution -- nothing collides with one -- so removing it mid-callback
// cannot pull geometry out from under a tick. What it CAN do is make a second
// script touching the same key in the same tick find nothing, which is exactly
// the behaviour a contested pickup wants.
int l_prop_despawn(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) { lua_pushboolean(L, 0); return 1; }
    ctx.world->despawn_prop(static_cast<u32>(e));
    lua_pushboolean(L, 1);
    return 1;
}

int l_prop_eq(lua_State* L) {
    lua_pushboolean(L, check_prop_handle(L, 1).raw == check_prop_handle(L, 2).raw);
    return 1;
}

int l_prop_tostring(lua_State* L) {
    lua_pushfstring(L, "prop(%d)", static_cast<int>(check_prop_handle(L, 1).index()) - 1);
    return 1;
}

const luaL_Reg PROP_METHODS[] = {
    {"valid", l_prop_valid},
    {"index", l_prop_index},
    {"kind", l_prop_kind},
    {"pos", l_prop_pos},
    {"set_animation", l_prop_set_animation},
    {"despawn", l_prop_despawn},
    {nullptr, nullptr},
};

// --------------------------------------------------------------------- sectors
//
// A sector handle carries only an index, with no generation -- unlike an
// entity. Sectors do not die: the map's topology is fixed for the lifetime of
// the map, so there is no reuse for a generation to protect against. Giving
// them one anyway would imply a lifecycle they do not have.

const char* const SECTOR_MT = "dnp.sector";

u32* to_sector(lua_State* L, int idx) {
    return static_cast<u32*>(luaL_testudata(L, idx, SECTOR_MT));
}

u32 check_sector(lua_State* L, int idx) {
    if (u32* p = to_sector(L, idx)) return *p;
    luaL_error(L, "expected a sector, got %s", luaL_typename(L, idx));
    return 0;
}

bool sector_in_range(WorldContext& ctx, u32 s) {
    return ctx.world->map() && s < ctx.world->map()->sector_count;
}

int l_sector_index(lua_State* L) {
    lua_pushnumber(L, check_sector(L, 1));
    return 1;
}

int l_sector_tag(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->map()->sector(s).tag);
    return 1;
}

int l_sector_floor(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    push_fx(L, ctx.world->map()->sector(s).floor_z);
    return 1;
}

int l_sector_ceil(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    push_fx(L, ctx.world->map()->sector(s).ceil_z);
    return 1;
}

// sector:move_parametric(from, to, duration_ticks [, loop] [, target])
//
// PARAMETRIC: a pure function of the tick, so it is replicated once and every
// client computes it for ticks the server has not reached (design §3.1.2). Free
// to replicate and free to predict.
int l_sector_move_parametric(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s) || !ctx.map_store) { lua_pushboolean(L, 0); return 1; }

    const fx3 from = check_vec3(L, 2);
    const fx3 to   = check_vec3(L, 3);
    const u32 dur  = static_cast<u32>(luaL_checknumber(L, 4));

    const char* loop_s = lua_isnoneornil(L, 5) ? "once" : luaL_checkstring(L, 5);
    sim::LoopMode loop;
    if (std::strcmp(loop_s, "once") == 0)          loop = sim::LoopMode::Once;
    else if (std::strcmp(loop_s, "loop") == 0)     loop = sim::LoopMode::Loop;
    else if (std::strcmp(loop_s, "pingpong") == 0) loop = sim::LoopMode::PingPong;
    else return luaL_error(L, "loop must be 'once', 'loop' or 'pingpong', got '%s'", loop_s);

    const char* tgt_s = lua_isnoneornil(L, 6) ? "both" : luaL_checkstring(L, 6);
    sim::MotionTarget target;
    if (std::strcmp(tgt_s, "both") == 0)         target = sim::MotionTarget::Both;
    else if (std::strcmp(tgt_s, "floor") == 0)   target = sim::MotionTarget::Floor;
    else if (std::strcmp(tgt_s, "ceiling") == 0) target = sim::MotionTarget::Ceiling;
    else return luaL_error(L, "target must be 'both', 'floor' or 'ceiling', got '%s'", tgt_s);

    if (ctx.world->motions().add(*ctx.map_store, s) >= sim::MAX_MOVING_SECTORS) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // start_tick is NOW, not zero: a motion declared at load time and one
    // declared by a trigger mid-match must both mean "begin from here".
    lua_pushboolean(L, ctx.world->motions().start_parametric(
        s, from, to, ctx.world->tick(), dur, loop, target) ? 1 : 0);
    return 1;
}

// sector:set_offset(v)
//
// SCRIPTED-DISCRETE: whatever this callback decided, with no closed form. The
// server streams it in every snapshot and a rider falls back to anchor-relative
// prediction (§3.2.5). Costs bandwidth every tick it moves, which is the price
// of being unpredictable.
int l_sector_set_offset(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s) || !ctx.map_store) { lua_pushboolean(L, 0); return 1; }
    const fx3 off = check_vec3(L, 2);
    if (ctx.world->motions().add(*ctx.map_store, s) >= sim::MAX_MOVING_SECTORS) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Queued, not applied. Registering the sector as movable above is
    // configuration and happens now; the OFFSET is a per-tick mutation and goes
    // through the same choke point everything else does, so the command log
    // stays a complete replay artifact.
    lua_pushboolean(L, ctx.commands->push(Command{CmdKind::SetSectorOffset, s, off}) ? 1 : 0);
    return 1;
}

int l_sector_offset(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    const i32 mi = ctx.world->motions().find(s);
    if (mi < 0) { lua_pushnil(L); return 1; }
    push_vec3(L, ctx.world->motions().at(static_cast<u32>(mi)).offset);
    return 1;
}

// sector:neighbors() -> { sector, ... }
//
// Computed live rather than baked into the map, because Doom's floor actions
// are defined against the CURRENT heights of the neighbours: "raise to the next
// higher floor" means the next higher floor at the moment the switch is thrown,
// and a staircase built by an earlier trigger changes the answer.
//
// Ascending sector index, deduplicated: two sectors that share several edges
// are one neighbour, and a script summing over the list would otherwise weight
// them twice.
int l_sector_neighbors(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    const geom::MapData* m = ctx.world->map();
    const geom::Sector& sec = m->sector(s);

    lua_newtable(L);
    int n = 0;
    for (u16 e = 0; e < sec.side_count; ++e) {
        const i32 nb = m->side(sec.first_side + e).neighbor;
        if (nb < 0) continue;
        bool seen = false;
        for (u16 k = 0; k < e && !seen; ++k) {
            seen = m->side(sec.first_side + k).neighbor == nb;
        }
        if (seen) continue;
        push_sector(L, static_cast<u32>(nb));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int l_sector_eq(lua_State* L) {
    lua_pushboolean(L, check_sector(L, 1) == check_sector(L, 2));
    return 1;
}

int l_sector_tostring(lua_State* L) {
    lua_pushfstring(L, "sector(%d)", static_cast<int>(check_sector(L, 1)));
    return 1;
}

const luaL_Reg SECTOR_METHODS[] = {
    {"index", l_sector_index},
    {"tag", l_sector_tag},
    {"floor", l_sector_floor},
    {"ceil", l_sector_ceil},
    {"offset", l_sector_offset},
    {"neighbors", l_sector_neighbors},
    {"move_parametric", l_sector_move_parametric},
    {"set_offset", l_sector_set_offset},
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

int l_dnp_sectors(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    const u32 n = ctx.world->map() ? ctx.world->map()->sector_count : 0;
    for (u32 i = 0; i < n; ++i) {
        push_sector(L, i);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// dnp.random(lo, hi) -> integer in [lo, hi]
//
// Deterministic and seeded from the map, never from a clock. "Random" in a
// simulation means reproducibly arbitrary: a scripted-discrete mover has to be
// something no CLIENT can derive, which is not the same as something the server
// cannot replay.
int l_dnp_random(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 lo = static_cast<i32>(luaL_checknumber(L, 1));
    const i32 hi = static_cast<i32>(luaL_checknumber(L, 2));
    if (hi < lo) return luaL_error(L, "dnp.random(%d, %d): empty range", lo, hi);
    lua_pushnumber(L, ctx.rng.next_range(lo, hi + 1));
    return 1;
}

// dnp.sector(i) -> sector, or nil.
//
// Alongside dnp.sectors() rather than replacing it: a script that wants one
// sector by index should not build a table of eight hundred to get it, which
// on a converted Doom map is what dnp.sectors() costs every call.
int l_dnp_sector(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const u32 n = ctx.world->map() ? ctx.world->map()->sector_count : 0;
    if (i < 0 || static_cast<u32>(i) >= n) { lua_pushnil(L); return 1; }
    push_sector(L, static_cast<u32>(i));
    return 1;
}

// dnp.side(i) -> { neighbor = sector | nil, climbable, grapple, blocking, ledge }
//
// A plain table, not a userdata: a side has no identity worth carrying and no
// mutator, so there is nothing for a handle type to protect. `neighbor` is a
// SECTOR handle rather than an index, so `dnp.side(n).neighbor:move_parametric`
// reads as one thought -- which is exactly what a door script does with it.
int l_dnp_side(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    const geom::MapData* m = ctx.world->map();
    if (!m || i < 0 || static_cast<u32>(i) >= m->side_count) { lua_pushnil(L); return 1; }
    const geom::Side& sd = m->side(static_cast<u32>(i));

    lua_newtable(L);
    if (sd.neighbor >= 0) push_sector(L, static_cast<u32>(sd.neighbor));
    else                  lua_pushnil(L);
    lua_setfield(L, -2, "neighbor");
    lua_pushboolean(L, (sd.flags & geom::side_flag::CLIMBABLE) != 0);
    lua_setfield(L, -2, "climbable");
    lua_pushboolean(L, (sd.flags & geom::side_flag::GRAPPLE) != 0);
    lua_setfield(L, -2, "grapple");
    lua_pushboolean(L, (sd.flags & geom::side_flag::BLOCKING) != 0);
    lua_setfield(L, -2, "blocking");
    lua_pushboolean(L, (sd.flags & geom::side_flag::LEDGE) != 0);
    lua_setfield(L, -2, "ledge");
    return 1;
}

// dnp.spawn_prop(name, position) -> prop, or nil
//
// By NAME, resolved through the catalogue the operator ships. A script naming a
// prop that is not installed gets nil and a line in the log, rather than a
// silent slot-zero entity -- "the key did not appear" should say which key.
int l_dnp_spawn_prop(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const fx3 pos = check_vec3(L, 2);

    if (ctx.props == nullptr || ctx.props->empty()) {
        // Named in terms of the FILE, because that is what is wrong. A map
        // asking for a prop the operator has not installed used to surface as
        // "the map's script failed", which points at the script.
        return luaL_error(L,
            "cannot spawn '%s': the prop catalogue is empty or missing "
            "(see --props, default assets/props.txt)", name);
    }
    const u16 kind = ctx.props->kind_by_name(name, len);
    if (kind == 0) {
        return luaL_error(L, "no prop named '%s' in the prop catalogue", name);
    }
    const u32 e = ctx.world->spawn_prop(kind, pos);
    if (e >= sim::MAX_ENTITIES) {
        // Full is not an error a map can do anything about, so it is a nil and
        // a counter rather than a raise -- but it is never silent.
        lua_pushnil(L);
        return 1;
    }
    // The catalogue's size, applied at spawn. It is the simulation's half of
    // the file and the only half the server reads.
    ctx.world->prop_mut(e).radius = ctx.prop_radius(kind);
    ctx.world->prop_mut(e).height = ctx.prop_height(kind);
    push_prop(L, ctx, e);
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
    {"sectors", l_dnp_sectors},
    {"sector", l_dnp_sector},
    {"spawn_prop", l_dnp_spawn_prop},
    {"side", l_dnp_side},
    {"random", l_dnp_random},
    {"stale_handle_ops", l_dnp_stale_ops},
    {nullptr, nullptr},
};

}  // namespace

void push_prop(lua_State* L, WorldContext& ctx, u32 entity) {
    if (!sim::World::is_prop(entity) || !ctx.world->prop(entity).active) {
        lua_pushnil(L);
        return;
    }
    void* p = lua_newuserdata(L, sizeof(proto::Handle));
    // index+1, the same convention agents use and the same one on the wire:
    // raw 0 means "no entity", and there is one index space, so a prop handle
    // and an agent handle are comparable numbers by construction.
    *static_cast<proto::Handle*>(p) =
        proto::Handle::make(entity + 1, ctx.world->generation(entity));
    luaL_getmetatable(L, PROP_MT);
    lua_setmetatable(L, -2);
}

void push_sector(lua_State* L, u32 index) {
    *static_cast<u32*>(lua_newuserdata(L, sizeof(u32))) = index;
    luaL_getmetatable(L, SECTOR_MT);
    lua_setmetatable(L, -2);
}

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

    luaL_newmetatable(L, PROP_MT);
    lua_newtable(L);
    for (const luaL_Reg* r = PROP_METHODS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_prop_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_prop_tostring); lua_setfield(L, -2, "__tostring");
    lua_pushliteral(L, "locked");          lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    luaL_newmetatable(L, SECTOR_MT);
    lua_newtable(L);
    for (const luaL_Reg* r = SECTOR_METHODS; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_sector_eq);       lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, l_sector_tostring); lua_setfield(L, -2, "__tostring");
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
