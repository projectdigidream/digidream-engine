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

// entity:set_input(move_x, move_y, aim [, buttons])
//
// Drives a scripted agent through the SAME controller a player runs. An enemy
// that moved by having its position written would be a second mover, and the
// two would disagree about steps, slopes, doors and being crushed -- which is
// exactly the thing the physics contract exists to prevent. So an enemy presses
// buttons.
//
// Refused for an agent a session is sitting at: a script that could write a
// player's input could walk them into a pit, and no amount of client prediction
// would survive it.
int l_entity_set_input(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    if (!ctx.world->agent(static_cast<u32>(a)).scripted) {
        return luaL_error(L, "entity %d is a player; its input comes from its session", a);
    }

    // Axes are Q1.15 on the wire, and a script writes them the same way, so a
    // scripted agent's input is byte-identical to one that arrived over the
    // network. Clamped rather than wrapped: a script asking to move at 3 gets
    // full speed, not a reversal.
    const auto axis = [&](int idx) {
        const fx v = check_fx(L, idx);
        const fx one = fx::one();
        const fx c = v > one ? one : (v < -one ? -one : v);
        return static_cast<i16>((c.raw * 32767) >> 32);
    };

    proto::InputFrame f;
    f.move_x = axis(2);
    f.move_y = axis(3);
    f.aim = brad(static_cast<u32>(luaL_checknumber(L, 4)));
    f.buttons = static_cast<u16>(lua_isnoneornil(L, 5) ? 0 : luaL_checknumber(L, 5));
    ctx.world->set_input(static_cast<u32>(a), f);
    return 0;
}

// entity:set_animation(n) -- the STATE, never the frame or the timing.
// entity:set_solid(bool)
//
// Whether other bodies collide with this one. A script turns it off for a
// corpse: Doom's dead do not block, and a body left solid in a doorway is a
// wall that appeared where you were about to run.
//
// The engine has no notion of dead, so it cannot decide this on its own -- the
// same division everything else here draws.
int l_entity_set_solid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    ctx.world->agent_mut(static_cast<u32>(a)).solid = lua_toboolean(L, 2) != 0;
    return 0;
}

// entity:hp() -> integer, and entity:set_hp(n)
//
// Health is the server's, not the simulation's: the engine has no notion of it,
// and a client only ever learns its OWN, through the private block.
int l_entity_hp(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.hp == nullptr) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, static_cast<double>(ctx.hp[a]));
    return 1;
}

int l_entity_set_hp(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.hp == nullptr) return 0;
    ctx.hp[a] = static_cast<i32>(luaL_checknumber(L, 2));
    return 0;
}

int l_entity_set_animation(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    const int n = static_cast<int>(luaL_checknumber(L, 2));
    if (n < 0 || n > 65535) return luaL_error(L, "animation state %d is out of range", n);
    ctx.world->agent_mut(static_cast<u32>(a)).animation_state = static_cast<u16>(n);
    return 0;
}

int l_entity_kind(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->agent(static_cast<u32>(a)).kind);
    return 1;
}

int l_entity_name(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.props == nullptr) { lua_pushnil(L); return 1; }
    const char* n = ctx.props->name_of(ctx.world->agent(static_cast<u32>(a)).kind);
    if (n == nullptr) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}

int l_entity_facing(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->agent(static_cast<u32>(a)).facing.raw);
    return 1;
}

/**
 * What this entity's controller is holding down, as a bitmask.
 *
 * `dnp.button` names the bits, and they are the same ones a script sets on a
 * monster with set_input -- so "is the player firing" is asked exactly the way
 * "make the monster fire" is said.
 *
 * WHY A SCRIPT NEEDS THIS. on_attack fires when a swing CONNECTS, which is all
 * a melee weapon needs and is useless to a ranged one: a shotgun that misses
 * fires no event, so a weapon implemented on on_attack alone can never miss,
 * never spend ammunition on a miss, and never draw a tracer into a wall. The
 * trigger being pulled is a different fact from something being hit, and only
 * one of them was observable.
 *
 * The CURRENT frame, which is the one the tick about to run will use. A script
 * reading it from on_tick sees what the player is pressing now.
 */
int l_entity_buttons(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->input(static_cast<u32>(a)).buttons);
    return 1;
}

int l_entity_despawn(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushboolean(L, 0); return 1; }
    if (!ctx.world->agent(static_cast<u32>(a)).scripted) {
        return luaL_error(L, "entity %d is a player; a script cannot remove it", a);
    }
    ctx.effects->clear_agent(static_cast<u32>(a));
    ctx.world->despawn(static_cast<u32>(a));
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
    {"set_input", l_entity_set_input},
    {"set_solid", l_entity_set_solid},
    {"hp", l_entity_hp},
    {"set_hp", l_entity_set_hp},
    {"set_animation", l_entity_set_animation},
    {"kind", l_entity_kind},
    {"name", l_entity_name},
    {"facing", l_entity_facing},
    {"buttons", l_entity_buttons},
    {"despawn", l_entity_despawn},
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

// prop:name() -> the catalogue's name for its kind
//
// So behaviour can be written per name. A script dispatching on a number would
// be dispatching on something an operator can change in a text file the script
// never mentions.
int l_prop_name(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0 || ctx.props == nullptr) { lua_pushnil(L); return 1; }
    const char* n = ctx.props->name_of(ctx.world->prop(static_cast<u32>(e)).kind);
    if (n == nullptr) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}

int l_prop_solid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) { lua_pushnil(L); return 1; }
    lua_pushboolean(L,
        (ctx.world->prop(static_cast<u32>(e)).flags & sim::prop_flag::SOLID) != 0);
    return 1;
}

// prop:set_solid(bool)
//
// Immediate, like despawn and for the same reason: nothing sweeps against a
// prop, so changing whether it blocks cannot invalidate a resolution already in
// progress. A door that becomes passable when unlocked is this call.
int l_prop_set_solid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) return 0;
    sim::PropState& p = ctx.world->prop_mut(static_cast<u32>(e));
    if (lua_toboolean(L, 2)) p.flags |= sim::prop_flag::SOLID;
    else                     p.flags &= static_cast<u8>(~sim::prop_flag::SOLID);
    return 0;
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
// prop:set_pos(v)
//
// Immediate, like the rest of the prop mutators and for the same reason:
// nothing sweeps against a prop, so moving one cannot invalidate a resolution
// in progress. This is how a projectile travels -- a fireball is a gameplay
// object, and what it does when it arrives is a decision the engine has no
// opinion about.
int l_prop_set_pos(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) return 0;
    const fx3 p = check_vec3(L, 2);
    // Quantised, for the reason spawn_prop is: a solid prop is simulated on
    // both sides and entity positions are quantised on the wire, so a value
    // the wire cannot carry exactly makes the two copies disagree.
    ctx.world->prop_mut(static_cast<u32>(e)).pos =
        fx3{proto::fxq_to_fx(proto::fx_to_fxq(p.x)),
            proto::fxq_to_fx(proto::fx_to_fxq(p.y)),
            proto::fxq_to_fx(proto::fx_to_fxq(p.z))};
    return 0;
}

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
    {"name", l_prop_name},
    {"solid", l_prop_solid},
    {"set_solid", l_prop_set_solid},
    {"pos", l_prop_pos},
    {"set_animation", l_prop_set_animation},
    {"set_pos", l_prop_set_pos},
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
    for (u32 i = 0; i < ctx.world->agent_high_water(); ++i) {
        if (!ctx.world->agent(i).active) continue;
        push_entity(L, ctx, i);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// dnp.players() -> { entity, ... }
//
// Only the PLAYERS, which is the question every monster script actually asks.
//
// dnp.entities() was standing in for this, and it builds a handle userdata for
// every active agent -- thirty-eight monsters on MAP02 -- to find the one or
// two that are people. Once a tick, per level, whether or not anybody is
// connected: a server hosting three maps allocated a hundred and twenty
// userdata a second with nobody playing, and the collector chasing that is what
// produced 39 ms in a tick on levels that were empty.
//
// A player is an agent with no catalogue kind: nobody placed it, so it walked
// in. That is the same test the script was making, moved to where it costs an
// integer compare instead of an allocation.
int l_dnp_players(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    int n = 0;
    // To the high-water mark, not to MAX_AGENTS. This runs once a tick per
    // level from mobs.lua's on_tick, and an AgentState is several hundred
    // bytes: at 1024 slots the full sweep reads most of a megabyte to find the
    // one agent that is a person. It showed up as 11% of all Lua time with a
    // thousand monsters on the level, for a call made once a tick.
    for (u32 i = 0; i < ctx.world->agent_high_water(); ++i) {
        const sim::AgentState& a = ctx.world->agent(i);
        if (!a.active || a.kind != 0) continue;
        push_entity(L, ctx, i);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// dnp.player_count() -> integer
//
// The same question with no allocation at all, for the overwhelmingly common
// answer: nobody is here, so there is nothing for a monster to do. An empty
// level costs one C call a tick now rather than a table and forty handles.
int l_dnp_player_count(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    int n = 0;
    for (u32 i = 0; i < ctx.world->agent_high_water(); ++i) {
        const sim::AgentState& a = ctx.world->agent(i);
        if (a.active && a.kind == 0) ++n;
    }
    lua_pushinteger(L, n);
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
            "cannot spawn '%s': the prop catalogue is empty or missing. "
            "The engine's own assets/props.txt carries no props by design -- "
            "every prop with a picture belongs to a module (docs/MODULES.md), "
            "so this usually means a missing --module doom2. See also --props.",
            name);
    }
    const u16 kind = ctx.props->kind_by_name(name, len);
    if (kind == 0) {
        return luaL_error(L, "no prop named '%s' in the prop catalogue", name);
    }
    // Stored at the position the WIRE can carry, exactly.
    //
    // A solid prop is simulated on both sides, and entity positions are
    // quantised on the wire (proto::fx_to_fxq). A prop kept at full precision
    // on the server and reconstructed from the quantised value on the client
    // sits a fraction apart on the two, so a body pressed against it is pushed
    // out to two different places and mispredicts every tick it is touching.
    //
    // Props do not move, so this costs one rounding at spawn and makes the two
    // copies identical for the rest of the map's life. A prop that ever does
    // move will need the same treatment wherever it is moved from.
    const fx3 at{proto::fxq_to_fx(proto::fx_to_fxq(pos.x)),
                 proto::fxq_to_fx(proto::fx_to_fxq(pos.y)),
                 proto::fxq_to_fx(proto::fx_to_fxq(pos.z))};
    const u32 e = ctx.world->spawn_prop(kind, at);
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
    ctx.world->prop_mut(e).flags  = ctx.prop_flags(kind);
    push_prop(L, ctx, e);
    return 1;
}

// dnp.prop(i) -> prop, or nil. `i` is an ENTITY index, which is what
// prop:index() returns and what an event carries.
// dnp.spawn_enemy(name, position) -> entity, or nil
//
// An agent nobody is sitting at, drawn from the catalogue like a prop and moved
// through the controller like a player. Both halves matter: it is a thing you
// can see, and a thing the same physics applies to.
int l_dnp_spawn_enemy(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const fx3 pos = check_vec3(L, 2);

    if (ctx.props == nullptr || ctx.props->empty()) {
        return luaL_error(L,
            "cannot spawn '%s': the prop catalogue is empty or missing. "
            "The engine's own assets/props.txt carries no props by design -- "
            "every prop with a picture belongs to a module (docs/MODULES.md), "
            "so this usually means a missing --module doom2. See also --props.",
            name);
    }
    const u16 kind = ctx.props->kind_by_name(name, len);
    if (kind == 0) return luaL_error(L, "no entry named '%s' in the prop catalogue", name);

    const u32 a = ctx.world->spawn_scripted(kind, pos);
    if (a >= sim::MAX_AGENTS) { lua_pushnil(L); return 1; }
    // Its body comes from the catalogue, the same numbers a prop's collider
    // does -- so an imp is as wide as its sprite says and the map's own body
    // proportions do not silently apply to it.
    ctx.world->params_mut(a).radius = ctx.prop_radius(kind);
    ctx.world->params_mut(a).height = ctx.prop_height(kind);
    // How fast it walks, from the catalogue. A parameter rather than something
    // the AI multiplies its input by: move_speed is already replicated and
    // already predicted, so a slow monster is slow on every client without a
    // word of it crossing the wire. Zero leaves the engine's default, so a prop
    // that never walks does not have to name a speed.
    if (ctx.props != nullptr) {
        const fx sp = ctx.props->speed_of(kind);
        if (sp > fx::zero()) ctx.world->params_mut(a).move_speed = sp;
    }
    ctx.effects->set_base(a, ctx.world->params(a));
    push_entity(L, ctx, a);
    return 1;
}

int l_dnp_prop(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const int i = static_cast<int>(luaL_checknumber(L, 1));
    if (i < 0 || !sim::World::is_prop(static_cast<u32>(i))) { lua_pushnil(L); return 1; }
    push_prop(L, ctx, static_cast<u32>(i));
    return 1;
}

// dnp.props() -> { prop, ... }
//
// Ascending entity index, always. Any order would be deterministic on its own,
// but a script iterating props has to see the same order the server's own loops
// use or a replay of the command log stops matching -- the same rule
// dnp.entities() follows.
int l_dnp_props(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    int n = 0;
    for (u32 e = sim::MAX_AGENTS; e < ctx.world->prop_high_water(); ++e) {
        if (!ctx.world->prop(e).active) continue;
        push_prop(L, ctx, e);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// dnp.hitscan(from_entity, range) -> hit_entity | nil, x, y, z
//
// A shot that arrives instantly: the ray stops at the first agent or prop in
// its path, or at the first wall. Doom's own zombies and shotgunners work this
// way, and so does every tracer weapon since.
//
// The ENGINE answers where the ray ended and what was there. Whether that hurts
// -- and by how much, and whether the shooter had ammunition -- is script, the
// same division a swing draws. Returning the impact point as well as the target
// is what lets a script draw a tracer that ends on the wall rather than at
// infinity.
int l_dnp_hitscan(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    const fx range = check_fx(L, 2);
    const geom::MapData* m = ctx.world->map();
    if (m == nullptr) { lua_pushnil(L); return 1; }

    const sim::AgentState& sh = ctx.world->agent(static_cast<u32>(a));
    // From the middle of the body, not its feet: a shot from ankle height is
    // stopped by every step in the map.
    const fx eye = sh.pos.z + ctx.world->params(static_cast<u32>(a)).height / 2;
    const fx2 from{sh.pos.x, sh.pos.y};

    // An OPTIONAL angular offset from the body's facing, in brad, so one pull
    // of a trigger can put several pellets in different places.
    //
    // The offset, not the randomness. Where a pellet goes is a gameplay
    // decision and belongs in the script -- which also means a shotgun's
    // pattern can be tuned without recompiling anything. Without it every
    // pellet of a seven-pellet shot travels the identical ray and a shotgun is
    // a pistol that does seven times the damage at any range.
    // brad wraps by design -- an angle has no range to saturate against -- so a
    // negative offset is just an addition that goes round the other way.
    const i64 spread = static_cast<i64>(luaL_optnumber(L, 3, 0));
    const brad aim = sh.facing + brad(static_cast<u32>(spread));
    const fx2 dir = fx_dir(aim);
    const fx2 to = from + dir * range;

    // Nearest ENTITY along the ray, by the same "in front and within the
    // corridor" test a swing uses -- a cylinder test rather than an exact
    // capsule intersection, which for bodies this size differs only where a
    // shot grazes an edge.
    i32  best = -1;
    fx   best_t = fx::one() + fx::one();
    const auto consider = [&](fx2 p, fx r, fx z0, fx z1, i32 entity) {
        if (eye < z0 || eye > z1) return;               // over or under it
        const fx2 toward = p - from;
        const fx along = dot(dir, toward);
        if (along <= fx::zero() || along > range) return;
        // Perpendicular distance from the ray to the centre.
        const fx2 closest = from + dir * along;
        const u128 off_sq = len_sq_raw(p - closest);
        if (off_sq > static_cast<u128>(static_cast<i128>(r.raw) * r.raw)) return;
        const fx t = along / range;
        if (t >= best_t) return;
        best_t = t;
        best = entity;
    };

    for (u32 i = 0; i < ctx.world->agent_high_water(); ++i) {
        if (i == static_cast<u32>(a) || !ctx.world->agent(i).active) continue;
        const sim::AgentState& e = ctx.world->agent(i);
        consider(fx2{e.pos.x, e.pos.y}, ctx.world->params(i).radius,
                 e.pos.z, e.pos.z + ctx.world->params(i).height, static_cast<i32>(i));
    }
    for (u32 e = sim::MAX_AGENTS; e < ctx.world->prop_high_water(); ++e) {
        if (!ctx.world->prop(e).active) continue;
        const sim::PropState& p = ctx.world->prop(e);
        consider(fx2{p.pos.x, p.pos.y}, p.radius, p.pos.z, p.pos.z + p.height,
                 static_cast<i32>(e));
    }

    // And the geometry, which OCCLUDES: a wall between the shooter and a target
    // stops the shot, so the wall is consulted even when a body was found.
    const physics::RayHit wall =
        physics::raycast(*m, sh.mv.sector, from, dir, range, 0, eye);
    fx2 end = to;
    if (wall.blocked) {
        const fx wall_t = wall.distance / range;
        if (best < 0 || wall_t < best_t) { best = -1; best_t = wall_t; }
        end = wall.point;
    }
    if (best >= 0) end = from + dir * (range * best_t);

    if (best < 0) lua_pushnil(L);
    else if (sim::World::is_prop(static_cast<u32>(best))) push_prop(L, ctx, static_cast<u32>(best));
    else push_entity(L, ctx, static_cast<u32>(best));
    push_vec3(L, fx3{end.x, end.y, eye});
    return 2;
}

// --- UI (docs/protocol.md section 11) ----------------------------------------

// Copy a Lua string into a fixed UiText, refusing rather than truncating.
//
// Truncating a label is the kind of silent damage that gets noticed months
// later by someone wondering why one shop item has a short name.
static bool ui_text(lua_State* L, int idx, proto::UiText& out, const char* what) {
    size_t n = 0;
    const char* p = lua_tolstring(L, idx, &n);
    if (p == nullptr) { out.len = 0; out.text[0] = '\0'; return true; }
    if (n > proto::UI_MAX_TEXT) {
        luaL_error(L, "ui: %s is %d bytes, the limit is %d", what,
                   static_cast<int>(n), static_cast<int>(proto::UI_MAX_TEXT));
        return false;
    }
    out.len = static_cast<u16>(n);
    for (size_t i = 0; i < n; ++i) out.text[i] = p[i];
    if (n < proto::UI_MAX_TEXT) out.text[n] = '\0';
    return true;
}

static proto::UiWidgetKind ui_kind_of(lua_State* L, const char* name) {
    if (name == nullptr) return proto::UiWidgetKind::Label;
    if (std::strcmp(name, "label") == 0)  return proto::UiWidgetKind::Label;
    if (std::strcmp(name, "button") == 0) return proto::UiWidgetKind::Button;
    if (std::strcmp(name, "slider") == 0) return proto::UiWidgetKind::Slider;
    if (std::strcmp(name, "select") == 0) return proto::UiWidgetKind::Select;
    if (std::strcmp(name, "toggle") == 0) return proto::UiWidgetKind::Toggle;
    luaL_error(L, "ui: unknown widget kind '%s'", name);
    return proto::UiWidgetKind::Label;
}

// dnp.levels() -> { "map01", ... }
//
// What the operator actually loaded, in progression order. The index into this
// list is what entity:change_map takes.
int l_dnp_levels(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    if (ctx.levels == nullptr) return 1;
    const u32 n = ctx.levels->level_count();
    for (u32 i = 0; i < n; ++i) {
        lua_pushstring(L, ctx.levels->level_name(i));
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// dnp.ui_open(entity, { title = "...", widgets = { {...}, ... } }) -> ui_id
//
// A widget is a table: kind, id, label, and whichever of value/lo/hi/options
// its kind uses. `id` is the script's own number and comes back untouched in
// on_ui_event, so a script indexes its own handlers rather than matching labels.
int l_dnp_ui_open(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) { lua_pushnumber(L, 0); return 1; }
    luaL_checktype(L, 2, LUA_TTABLE);

    proto::UiPanel panel;
    lua_getfield(L, 2, "title");
    ui_text(L, -1, panel.title, "title");
    lua_pop(L, 1);

    lua_getfield(L, 2, "widgets");
    if (lua_istable(L, -1)) {
        const int n = static_cast<int>(lua_objlen(L, -1));
        if (n > static_cast<int>(proto::UI_MAX_WIDGETS)) {
            return luaL_error(L, "ui: %d widgets, the limit is %d", n,
                              static_cast<int>(proto::UI_MAX_WIDGETS));
        }
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
            proto::UiWidget& g = panel.widgets[panel.count];

            lua_getfield(L, -1, "kind");
            g.kind = ui_kind_of(L, lua_tostring(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "id");
            g.id = static_cast<u16>(lua_tonumber(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "label");
            ui_text(L, -1, g.label, "label");
            lua_pop(L, 1);

            lua_getfield(L, -1, "value");
            g.value = static_cast<i32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "lo");
            g.lo = static_cast<i32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "hi");
            g.hi = static_cast<i32>(lua_tonumber(L, -1));
            lua_pop(L, 1);

            lua_getfield(L, -1, "options");
            if (lua_istable(L, -1)) {
                const int m = static_cast<int>(lua_objlen(L, -1));
                if (m > static_cast<int>(proto::UI_MAX_OPTIONS)) {
                    return luaL_error(L, "ui: %d options, the limit is %d", m,
                                      static_cast<int>(proto::UI_MAX_OPTIONS));
                }
                for (int k = 1; k <= m; ++k) {
                    lua_rawgeti(L, -1, k);
                    ui_text(L, -1, g.options[g.option_count], "option");
                    ++g.option_count;
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);   // options

            ++panel.count;
            lua_pop(L, 1);   // the widget table
        }
    }
    lua_pop(L, 1);   // widgets

    lua_pushnumber(L, static_cast<double>(
        ctx.ui->open_ui(ctx.level, static_cast<u32>(a), panel)));
    return 1;
}

// dnp.ui_set(entity, ui_id, widget_id, value)
//
// Change one widget in a panel that is already open. Without it a script that
// merely SHOWS something changing has to close and reopen the panel, which
// allocates a new id and, from the player's side, is a window that flickers
// every time their health does.
int l_dnp_ui_set(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) return 0;
    ctx.ui->set_ui(ctx.level, static_cast<u32>(a),
                   static_cast<u32>(luaL_checknumber(L, 2)),
                   static_cast<u16>(luaL_checknumber(L, 3)),
                   static_cast<i32>(luaL_checknumber(L, 4)));
    return 0;
}

// dnp.camera(entity, { distance = 40, pitch = 0.14, lead = 3, ... })
//
// How the camera behaves for whoever is watching this agent. Design §4.1 puts
// the camera on the CLIENT and lets Lua configure and trigger it, so nothing
// here says where the camera is -- only how it should follow.
//
// Named fields rather than positional, and only the ones given are sent: a
// script that wants a longer lead should not have to restate the pitch.
static const char* const CAMERA_FIELD_NAMES[] = {
    "distance", "pitch", "smoothing", "max_accel",
    "lead", "lead_smoothing", "deadzone_x", "deadzone_y"
};

int l_dnp_camera(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) return 0;
    luaL_checktype(L, 2, LUA_TTABLE);

    proto::CameraSet c;
    for (u8 id = 0; id < static_cast<u8>(sizeof(CAMERA_FIELD_NAMES) / sizeof(char*)); ++id) {
        lua_getfield(L, 2, CAMERA_FIELD_NAMES[id]);
        if (!lua_isnil(L, -1)) {
            // THOUSANDTHS on the wire. A camera is not simulation state, so it
            // does not need fx -- but it does need a fixed scale, or a script
            // writing 0.14 and a client reading 0 would look like the setting
            // doing nothing.
            const double v = lua_tonumber(L, -1);
            c.fields[c.count].id = id;
            c.fields[c.count].milli = static_cast<i32>(v * 1000.0 +
                                                       (v >= 0 ? 0.5 : -0.5));
            ++c.count;
        }
        lua_pop(L, 1);
    }
    if (c.count > 0) ctx.ui->set_camera(ctx.level, static_cast<u32>(a), c);
    return 0;
}

// dnp.side_texture(side, "middle") -> name, and
// dnp.set_side_texture(side, "middle", "SW2COMP")
//
// The picture on one field of one side. A switch that has been thrown.
//
// The ENGINE has no idea what a switch is: it reports that an entity reached
// for a side and stops there, exactly as it does for a door. Which texture
// pairs with which -- Doom's SW1/SW2 convention -- belongs in the Doom script,
// so the read exists to let a script work the pairing out for itself rather
// than have the engine know it.
static const char* const SIDE_FIELD_NAMES[] = { "upper", "middle", "lower" };

static bool side_field_of(lua_State* L, int idx, proto::SideField& out) {
    const char* n = luaL_checkstring(L, idx);
    for (u8 i = 0; i < 3; ++i) {
        if (std::strcmp(n, SIDE_FIELD_NAMES[i]) == 0) {
            out = static_cast<proto::SideField>(i);
            return true;
        }
    }
    luaL_error(L, "side field must be 'upper', 'middle' or 'lower', got '%s'", n);
    return false;
}

int l_dnp_side_texture(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    const u32 side = static_cast<u32>(luaL_checknumber(L, 1));
    proto::SideField f;
    if (m == nullptr || ctx.map_store == nullptr || side >= m->side_count ||
        !side_field_of(L, 2, f)) {
        lua_pushnil(L);
        return 1;
    }
    const geom::RenderData rd = ctx.map_store->render_view();
    if (side >= rd.side_count) { lua_pushnil(L); return 1; }
    const geom::SideRender& r = rd.side(side);
    const u16 tex = f == proto::SideField::Upper ? r.upper
                  : f == proto::SideField::Lower ? r.lower
                                                 : r.middle;
    if (tex == geom::NO_TEXTURE || tex >= rd.texture_count) { lua_pushnil(L); return 1; }
    const geom::TextureName& tn = rd.texture(tex);
    lua_pushlstring(L, tn.name, tn.len);
    return 1;
}

int l_dnp_set_side_texture(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    const u32 side = static_cast<u32>(luaL_checknumber(L, 1));
    proto::SideField f;
    if (m == nullptr || side >= m->side_count || !side_field_of(L, 2, f)) return 0;
    if (ctx.ui == nullptr) return 0;

    proto::SideTexture t;
    t.side = static_cast<u16>(side);
    t.field = f;
    if (!ui_text(L, 3, t.name, "texture name")) return 0;
    ctx.ui->set_side_texture(ctx.level, t);
    return 0;
}

// dnp.ui_close(entity, ui_id)
int l_dnp_ui_close(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) return 0;
    ctx.ui->close_ui(ctx.level, static_cast<u32>(a),
                     static_cast<u32>(luaL_checknumber(L, 2)));
    return 0;
}

// dnp.vfx(kind, position [, direction] [, param])
//
// A transient visual: a tracer, a flash, an impact. Fire and forget, off the
// entity replication path (§9.2) -- it has no identity, nothing refers to it,
// and a client that missed one has missed nothing it needs to stay consistent.
//
// The engine attaches no meaning to `kind` or `param`. What a 1 looks like is
// between the catalogue and the renderer, exactly as it is for a prop.
int l_dnp_vfx(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    sim::PendingVfx v;
    v.kind = static_cast<u16>(luaL_checknumber(L, 1));
    v.pos = check_vec3(L, 2);
    v.dir = brad(lua_isnoneornil(L, 3) ? 0u : static_cast<u32>(luaL_checknumber(L, 3)));
    v.param = lua_isnoneornil(L, 4) ? 0u : static_cast<u32>(luaL_checknumber(L, 4));
    lua_pushboolean(L, ctx.world->push_vfx(v) ? 1 : 0);
    return 1;
}

// dnp.can_see(a, b) -> bool
//
// Is there an unobstructed line between two entities' eye height?
//
// The engine's own raycast, which is the same swept test the mover and the
// grapple use -- so "the monster can see you" and "the monster could walk to
// you" cannot disagree about what a wall is. Without it a monster shoots
// through the floor at somebody two rooms away, which is what happens when
// "is it close" is mistaken for "is it visible".
int l_dnp_can_see(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    if (m == nullptr) { lua_pushboolean(L, 0); return 1; }

    const auto eye_of = [&](int idx, fx3& out, i32& sector) {
        if (proto::Handle* h = to_handle(L, idx)) {
            const i32 a = resolve(L, ctx, *h);
            if (a < 0) return false;
            const sim::AgentState& e = ctx.world->agent(static_cast<u32>(a));
            out = fx3{e.pos.x, e.pos.y,
                      e.pos.z + ctx.world->params(static_cast<u32>(a)).height / 2};
            sector = e.mv.sector;
            return true;
        }
        if (to_prop(L, idx)) {
            const i32 e = resolve_prop(L, ctx, check_prop_handle(L, idx));
            if (e < 0) return false;
            const sim::PropState& p = ctx.world->prop(static_cast<u32>(e));
            out = fx3{p.pos.x, p.pos.y, p.pos.z + p.height / 2};
            sector = geom::find_sector(*m, fx2{p.pos.x, p.pos.y});
            return true;
        }
        return false;
    };

    fx3 from{}, to{};
    i32 from_sector = -1, to_sector = -1;
    if (!eye_of(1, from, from_sector) || !eye_of(2, to, to_sector)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    const fx2 d{to.x - from.x, to.y - from.y};
    const fx dist = length(d);
    if (dist <= fx::zero()) { lua_pushboolean(L, 1); return 1; }

    // At EYE HEIGHT. Sight is not a floorplan question: a two-sided line with a
    // balcony behind it, or a shut door, is a wall to look through even though
    // it names a neighbouring sector.
    // SLOPED, from one eye to the other. A flat cast at the looker's own height
    // is a different line from the one to the target the moment the two are at
    // different heights, and on a Doom map they nearly always are -- a step, a
    // stair, a ledge. It answered "no" for a player standing in the open a
    // little below, so the monster lost its target, reacquired it, and stood
    // there doing that: an enemy stuck on a step whenever the player was lower.
    const physics::RayHit hit =
        physics::raycast(*m, from_sector, fx2{from.x, from.y}, normalize(d), dist,
                         0, from.z, to.z);
    // BLOCKED before reaching the target is a wall in the way. Blocked at or
    // past it is the wall behind it.
    lua_pushboolean(L, (!hit.blocked || hit.distance >= dist) ? 1 : 0);
    return 1;
}

// dnp.length(v) -> fx
int l_dnp_length(lua_State* L) {
    const fx3 v = check_vec3(L, 1);
    push_fx(L, length(fx2{v.x, v.y}));
    return 1;
}

// dnp.sweep(from, dir, distance) -> blocked, impact
//
// Does a straight line of `distance` from `from` along `dir` reach, or does the
// map stop it?
//
// The engine's own raycast, which is the same swept test the mover uses -- so a
// projectile is stopped by exactly what a body would be stopped by. A fireball
// that ignored geometry sailed through every wall in the map.
int l_dnp_sweep(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    const fx3 from = check_vec3(L, 1);
    const fx3 dir3 = check_vec3(L, 2);
    const fx dist = check_fx(L, 3);
    if (m == nullptr || dist <= fx::zero()) {
        lua_pushboolean(L, 0);
        push_vec3(L, from);
        return 2;
    }
    const fx2 dir = normalize(fx2{dir3.x, dir3.y});
    const i32 sector = geom::find_sector(*m, fx2{from.x, from.y});
    // At the projectile's own height, so a fireball is stopped by the step it
    // would actually hit rather than by the map's floorplan.
    const physics::RayHit hit =
        physics::raycast(*m, sector, fx2{from.x, from.y}, dir, dist, 0, from.z);
    if (!hit.blocked) {
        lua_pushboolean(L, 0);
        push_vec3(L, from);
        return 2;
    }
    lua_pushboolean(L, 1);
    // Stopped a hair short of the surface, so the thing that explodes is
    // visible against the wall rather than inside it.
    const fx back = fx::from_frac(1, 10);
    const fx d = hit.distance > back ? hit.distance - back : fx::zero();
    push_vec3(L, fx3{from.x + dir.x * d, from.y + dir.y * d, from.z});
    return 2;
}

// dnp.sweep_hit(from, dir, distance, radius, [ignore_a], [ignore_b])
//   -> nil                     nothing in reach
//   -> false, impact           the map stopped it
//   -> entity, impact          an entity stopped it
//
// One question, one answer. `dnp.sweep` reports geometry ONLY, so a caller
// that also wanted to know whom it reached had to walk every entity itself --
// which the mob runtime did, in Lua, for every missile every tick, and which
// could not be right anyway: asking "is there a wall" and "is there a monster"
// separately gives a fireball that bursts on somebody standing behind a door.
//
// The three returns are distinguishable without a flag: nil is nothing,
// `false` is a wall, an entity is an entity.
int l_dnp_sweep_hit(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const fx3 from = check_vec3(L, 1);
    const fx3 dir3 = check_vec3(L, 2);
    const fx dist = check_fx(L, 3);
    const fx radius = check_fx(L, 4);

    // TWO, because a missile has two things to skip: itself, which sits
    // exactly at `from` on the tick it is fired, and the mob that fired it,
    // which is standing in the same place. Skipping either alone leaves the
    // other, and the symptom is a fireball that bursts the instant it exists.
    //
    // EITHER KIND. A missile IS a prop, and a prop handle is a different
    // userdata type from an entity handle -- so insisting on an entity here
    // meant every missile raised "expected an entity, got userdata" on its
    // first step, every tick, and hung motionless where it was launched.
    // Reported as "projectiles get stuck in the air".
    const auto any_handle = [&](int idx) -> i32 {
        if (lua_isnoneornil(L, idx)) return -1;
        if (proto::Handle* h = to_handle(L, idx)) return resolve(L, ctx, *h);
        if (proto::Handle* h = to_prop(L, idx)) return resolve_prop(L, ctx, *h);
        luaL_error(L, "expected an entity or a prop, got %s", luaL_typename(L, idx));
        return -1;
    };
    const i32 ignore_a = any_handle(5);
    const i32 ignore_b = any_handle(6);

    const fx2 dir = normalize(fx2{dir3.x, dir3.y});
    const sim::World::SweepHit hit =
        ctx.world->sweep_entities(from, dir, dist, radius, ignore_a, ignore_b, -1);

    if (!hit.hit) { lua_pushnil(L); return 1; }

    if (hit.entity_hit) {
        push_entity(L, ctx, hit.entity);
    } else {
        lua_pushboolean(L, 0);
    }
    // Stopped a hair short, so a burst is visible against what it hit rather
    // than inside it -- the same tenth of a unit dnp.sweep backs off by, and
    // for the same reason.
    const fx back = fx::from_frac(1, 10);
    const fx d = hit.distance > back ? hit.distance - back : fx::zero();
    push_vec3(L, fx3{from.x + dir.x * d, from.y + dir.y * d, from.z});
    return 2;
}

// dnp.find_path(from_sector, to_sector, [radius], [height], [jump_height])
//   -> { {sector=, x=, y=, jump=}, ... }  the route, from inclusive to inclusive
//   -> nil, reason                  no route, or the query was refused
//
// A* over the sector graph (design §3.2.4). Sectors are nodes; adjacency comes
// from the map, INCLUDING across `open` sides where nothing names a neighbour
// -- see the header of core/include/dnp/nav/navgraph.hpp.
//
// Waypoints, not bare indices: every caller that walks a route immediately
// wants somewhere to steer at, and a sector index alone would make that a
// second binding call per node. The centroid is what the graph already
// measured its costs between, so it is the point the cost refers to.
//
// WHETHER AN EDGE IS WALKABLE IS ASKED AT QUERY TIME, of the same
// physics::sector_admits the mover asks. So a route cannot lead through a door
// the body would be stopped by, cannot refuse a step it would in fact walk up,
// and changes when a lift moves -- without anything being rebuilt.
//
// `jump_height` is how much MORE than a step this body can gain by hopping.
// Zero -- the default -- is design §3.2.2's rule and routes exactly as before.
// Where a step needs a jump the waypoint says `jump = true`, and A CALLER THAT
// IGNORES IT HAS BROKEN THE ROUTE: the path leads somewhere the body will not
// reach by walking, so it presses into the ledge forever. Worse than never
// having routed it there.
int l_dnp_find_path(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    if (m == nullptr || ctx.nav == nullptr) {
        lua_pushnil(L);
        lua_pushliteral(L, "no map");
        return 2;
    }

    // A PER-TICK BUDGET, because the instruction hook cannot see this.
    //
    // A count hook measures bytecode, and an A* over five hundred sectors is
    // one call. Design risk 6 names the failure exactly: an unbudgeted
    // find_path storm pushes the loop past the 250 ms drop threshold, every
    // client gets a burst of corrections, and a Lua performance problem
    // presents as a netcode bug -- the hardest thing here to diagnose. So the
    // refusal is explicit and says why, rather than being a mystery stall.
    // PER LEVEL, on the context -- see WorldContext::path_budget_tick for what
    // a process-wide static did instead.
    constexpr u32 PATHS_PER_TICK = 64;
    const u32 now = ctx.world->tick();
    if (now != ctx.path_budget_tick) { ctx.path_budget_tick = now; ctx.path_budget_used = 0; }
    if (ctx.path_budget_used >= PATHS_PER_TICK) {
        lua_pushnil(L);
        lua_pushliteral(L, "path budget");
        return 2;
    }
    ++ctx.path_budget_used;

    const i32 from = static_cast<i32>(luaL_checknumber(L, 1));
    const i32 to   = static_cast<i32>(luaL_checknumber(L, 2));

    nav::NavBody body;
    if (!lua_isnoneornil(L, 3)) body.radius = check_fx(L, 3);
    if (!lua_isnoneornil(L, 4)) body.height = check_fx(L, 4);
    if (!lua_isnoneornil(L, 5)) body.jump_height = check_fx(L, 5);

    // Static, not a local: NavQuery is tens of kilobytes and Emscripten's
    // default stack is 64 KB. The server is native, but the two builds share
    // the habit for a reason and a stack overflow here would present as
    // corruption somewhere else entirely.
    static nav::NavQuery query;
    static u32 path[nav::MAX_NAV_NODES];
    static bool jump[nav::MAX_NAV_NODES];

    const nav::PathResult r =
        nav::find_path(*m, *ctx.nav, query, static_cast<u32>(from),
                       static_cast<u32>(to), body, path, nav::MAX_NAV_NODES,
                       jump);

    if (r.status != nav::PathStatus::Ok &&
        r.status != nav::PathStatus::SameSector) {
        lua_pushnil(L);
        lua_pushstring(L, nav::path_status_text(r.status));
        return 2;
    }

    lua_createtable(L, static_cast<int>(r.length), 0);
    for (u32 i = 0; i < r.length; ++i) {
        lua_createtable(L, 0, 3);
        lua_pushnumber(L, path[i]);
        lua_setfield(L, -2, "sector");
        const fx2 c = ctx.nav->node(path[i]).centroid;
        push_fx(L, c.x); lua_setfield(L, -2, "x");
        push_fx(L, c.y); lua_setfield(L, -2, "y");
        lua_pushboolean(L, jump[i] ? 1 : 0);
        lua_setfield(L, -2, "jump");
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// dnp.atan2(y, x) -> a brad
//
// The engine's own CORDIC, not libm: a script computing an aim with a
// double-precision atan2 would produce an angle the simulation could not have
// produced, and the difference would show up as a monster facing a fraction of
// a degree off on one machine.
int l_dnp_atan2(lua_State* L) {
    lua_pushnumber(L, fx_atan2(check_fx(L, 1), check_fx(L, 2)).raw);
    return 1;
}

// dnp.dir(brad) -> a unit vec3 in the XY plane
int l_dnp_dir(lua_State* L) {
    const fx2 d = fx_dir(brad(static_cast<u32>(luaL_checknumber(L, 1))));
    push_vec3(L, fx3{d.x, d.y, fx::zero()});
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
    {"players", l_dnp_players},
    {"player_count", l_dnp_player_count},
    {"sectors", l_dnp_sectors},
    {"sector", l_dnp_sector},
    {"spawn_prop", l_dnp_spawn_prop},
    {"spawn_enemy", l_dnp_spawn_enemy},
    {"hitscan", l_dnp_hitscan},
    {"can_see", l_dnp_can_see},
    {"length", l_dnp_length},
    {"sweep", l_dnp_sweep},
    {"sweep_hit", l_dnp_sweep_hit},
    {"find_path", l_dnp_find_path},
    {"atan2", l_dnp_atan2},
    {"dir", l_dnp_dir},
    {"vfx", l_dnp_vfx},
    {"levels", l_dnp_levels},
    {"ui_open", l_dnp_ui_open},
    {"camera", l_dnp_camera},
    {"side_texture", l_dnp_side_texture},
    {"set_side_texture", l_dnp_set_side_texture},
    {"ui_set", l_dnp_ui_set},
    {"ui_close", l_dnp_ui_close},
    {"prop", l_dnp_prop},
    {"props", l_dnp_props},
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

    // dnp.button.* -- the protocol's own values, not copies of them.
    //
    // A script pressing JUMP needs the same 0x0001 the wire uses, and a
    // hardcoded one in Lua is the "two programs, one number" failure this repo
    // has paid for twice. These are read straight from proto::button.
    //
    // BUILT HERE, ONCE. This lived inside push_sector(), which runs every time
    // a sector userdata is handed to Lua -- so the table was rebuilt on every
    // such call, and, worse, did not exist at all until something had pushed a
    // sector. A script reading dnp.button at load time got nil, which is how
    // this was found: a probe at the top of a file crashed on
    // "attempt to index field 'button' (a nil value)" while the same
    // expression worked fine from on_tick.
    {
        lua_createtable(L, 0, 11);
        struct { const char* name; u16 bit; } const BUTTONS[] = {
            {"JUMP", proto::button::JUMP},
            {"DASH", proto::button::DASH},
            {"CLIMB", proto::button::CLIMB},
            {"HOVER", proto::button::HOVER},
            {"FLIGHT", proto::button::FLIGHT},
            {"GRAPPLE", proto::button::GRAPPLE},
            {"INTERACT", proto::button::INTERACT},
            {"ATTACK_PRIMARY", proto::button::ATTACK_PRIMARY},
            {"ATTACK_SECONDARY", proto::button::ATTACK_SECONDARY},
            {"USE", proto::button::USE},
            {"CROUCH", proto::button::CROUCH},
        };
        for (const auto& b : BUTTONS) {
            lua_pushnumber(L, b.bit);
            lua_setfield(L, -2, b.name);
        }
        lua_setfield(L, -2, "button");
    }

    lua_setglobal(L, "dnp");
}

}  // namespace dnp::server::lua
