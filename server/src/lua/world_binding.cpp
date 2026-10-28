#include "lua/world_binding.hpp"
#include "mobs/mob_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dnp/physics/movedefs.hpp"
#include "dnp/sim/blast.hpp"
#include "dnp/proto/messages.hpp"
#include "dnp/sim/sectormotion.hpp"
#include "dnp/physics/params_table.hpp"
#include "lua/fx_binding.hpp"
#include "sky.hpp"
#include "platform.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#include "lua/lua51_compat.hpp"

namespace dnp::server::lua {

namespace {

const char* const ENTITY_MT = "dnp.entity";
const char* const CTX_KEY   = "dnp_world_ctx";

// An angle argument in brad, of any size and either sign, wrapped to a turn
// (2^32) as every angle in the engine wraps. A negative or huge double cast
// straight to u32 is undefined -- and in wasm (the web build) a trap.
brad check_brad(lua_State* L, int idx) {
    const double d = std::fmod(luaL_checknumber(L, idx), 4294967296.0);
    return brad(static_cast<u32>(static_cast<i64>(d < 0 ? d + 4294967296.0 : d)));
}

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
    push_vec3(L, ctx.to_local(ctx.world->agent(static_cast<u32>(a)).pos));
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

// e:jumps() -> how many jumps the body has used since it last stood on
// something. Rises by one on every jump, in the air as well as off the ground,
// which is what makes it the way to hear a double jump.
int l_entity_jumps(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->agent(static_cast<u32>(a)).mv.jumps_used);
    return 1;
}

// e:pitch() -> where the body looks vertically, as an fx fraction of a turn
// (+ up), from its input this tick. Zero for a scheme with no vertical aim.
int l_entity_pitch(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    push_fx(L, fx::from_frac(ctx.world->agent(static_cast<u32>(a)).pitch, 65536));
    return 1;
}

int l_entity_sector(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    // The level's own number for it: -1 when the body stands in another level.
    lua_pushnumber(L, ctx.sector_to_local(ctx.world->agent(static_cast<u32>(a)).mv.sector));
    return 1;
}

// e:effect_sectors() -> the effect sectors (`sector type effect`) the body is
// in -- the footprint over where it stands, the height overlapping its own --
// as the level's numbers, ascending; {} when none. They overlap anything, so a
// body is in any number of them at once; graphs' sector entering and leaving
// reads them, as e:sector() for a room.
int l_entity_effect_sectors(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    lua_newtable(L);
    const geom::MapData* m = ctx.world->map();
    if (a < 0 || m == nullptr) return 1;
    const auto& ag = ctx.world->agent(static_cast<u32>(a));
    const fx feet = ag.pos.z;
    const fx head = feet + ctx.world->params(static_cast<u32>(a)).height;
    const u32 first = ctx.chunk ? ctx.chunk->sectors.first : 0u;
    const u32 count = ctx.chunk ? ctx.chunk->sectors.count : m->sector_count;
    int n = 0;
    // ponytail: every sector of the level, per call; a list of the effect ones if levels grow thousands.
    for (u32 s = first; s < first + count && s < m->sector_count; ++s) {
        const geom::Sector& sec = m->sector(s);
        if (!sec.effect || head <= sec.floor_z || feet >= sec.ceil_z) continue;
        if (!geom::point_in_sector(*m, s, fx2{ag.pos.x, ag.pos.y})) continue;
        lua_pushnumber(L, ctx.sector_to_local(static_cast<i32>(s)));
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int l_entity_index_of(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, a);
    return 1;
}

// e:player() -> which player this body is, as a number that stays the same
// from level to level, or nil for a body nobody plays. What the game VM keys a
// player's state by: an agent index is a slot in ONE world, handed to whoever
// arrives next, and a player walking into a level off the grid gets another.
int l_entity_player(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.player == nullptr || ctx.player[a] == 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.player[a]);
    return 1;
}

// e:input_tick() -> which of this player's inputs the step about to run
// consumes, or nil for a body nobody plays. The PLAYER's clock, not the
// world's: on a tick the server runs without fresh input (a repeat) it stays
// where it was, so something timed from a player's input -- their own
// rocket's shove, which their client predicts on its input ticks -- can wait
// for the input it was meant for.
int l_entity_input_tick(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.input_tick == nullptr || ctx.player == nullptr || ctx.player[a] == 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.input_tick[a]);
    return 1;
}

// Writes are deferred. Named as verbs, never as assignments, because a script
// that reads the value back inside the same callback will see the old one.
int l_entity_teleport(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;                          // defined no-op
    const fx3 p = ctx.to_world(check_vec3(L, 2));
    if (ctx.commands->push(Command{CmdKind::Teleport, static_cast<u32>(a), p})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// entity:change_map([index [, spawn_tag]]) -> true if the request was accepted
//
// `spawn_tag` names the spawn point in the destination to arrive at -- the
// door you came through, not the start of the map. A tag the level does not
// have arrives at its start, said on the server's output.
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
    const char* tag = lua_isnoneornil(L, 3) ? nullptr : luaL_checkstring(L, 3);
    ctx.map_changes->request_map_change(ctx.level, static_cast<u32>(a), to, tag);
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
// dnp entity:press(buttons)
//
// OR a button mask into the input this agent ALREADY has, leaving the movement
// axes and the aim exactly as they were.
//
// This exists for one reason: a monster's axes are owned by the native mob loop
// (server/src/mobs/mob_runtime.cpp), which recomputes them on think ticks, and
// `set_input` writes a whole frame. A script wanting a monster to SWING would
// have to pass movement it does not know, and would stop the monster dead for a
// tick to do it.
//
// One tick is enough and that is the point: the controller's attack is
// edge-triggered, so a single tick with the button down starts a swing that then
// runs for `attack_ticks` on its own. Nothing here has to hold it.
//
// Scripted agents only, the same rule set_input follows: a script that could
// press a PLAYER's buttons could swing their weapon for them, and no amount of
// reconciliation survives an input the client never sent.
int l_entity_press(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    if (!ctx.world->agent(static_cast<u32>(a)).scripted) {
        return luaL_error(L, "entity %d is a player; its input comes from its session", a);
    }
    proto::InputFrame f = ctx.world->input(static_cast<u32>(a));
    f.buttons = static_cast<u16>(f.buttons | static_cast<u16>(luaL_checknumber(L, 2)));
    ctx.world->set_input(static_cast<u32>(a), f);
    return 0;
}

// entity:swing()
//
// START A SWING NOW, in the shape its attack parameters hold: what a monster's
// `swing` step does (mobs.lua writes that step's own shape into its body
// first), and a weapon run in Lua. Not the button: a press is ignored while a
// swing sweeps, and a step is somebody saying "now" -- this REPLACES a swing
// still sweeping, and does nothing inside the last one's cooldown
// (World::request_swing, docs/SWINGS.md).
//
// A player too, unlike `press`: this is the server deciding something about
// the body, not an input forged in the player's name, so the snapshot says
// so (LUA_APPLIED) and reconciliation takes it like a shove.
int l_entity_swing(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    if (ctx.commands->push(Command{CmdKind::Swing, static_cast<u32>(a), fx3{}})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// entity:release(buttons)
//
// The other half of `press`: clear a mask from the input this agent already
// has, leaving the axes and the aim alone.
//
// It exists because `press` is NOT self-limiting. The native mob loop's
// set_move early-returns when nothing it knows about has changed
// (server/src/mobs/mob_runtime.cpp), comparing against the Mob's OWN cache --
// which cannot know a script reached past it and OR'd a bit into the world's
// input frame. A monster in a `move = "stop"` attack state has constant axes,
// so nothing would ever write the button back to zero, `attack_held` would stay
// true, and the SECOND swing would never start.
//
// So a script that presses releases on a later tick, and the press is one tick
// by construction rather than by luck. (A monster's `swing` no longer presses
// at all: it calls entity:swing, which starts the swing outright.)
int l_entity_release(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) return 0;
    if (!ctx.world->agent(static_cast<u32>(a)).scripted) {
        return luaL_error(L, "entity %d is a player; its input comes from its session", a);
    }
    proto::InputFrame f = ctx.world->input(static_cast<u32>(a));
    f.buttons = static_cast<u16>(f.buttons & ~static_cast<u16>(luaL_checknumber(L, 2)));
    ctx.world->set_input(static_cast<u32>(a), f);
    return 0;
}

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
    f.aim = check_brad(L, 4);
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

// entity:solid() -> whether other bodies collide with it. What auto-aim asks
// (a corpse is not solid, and a predicting client mirrors only solid bodies).
int l_entity_solid(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    lua_pushboolean(L, a >= 0 && ctx.world->agent(static_cast<u32>(a)).solid);
    return 1;
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

// entity:avatar() -> n, and entity:set_avatar(n)
//
// WHICH ART DRAWS THIS PLAYER, so two players in one level can look nothing
// alike. A number, never a record and never a sheet name: the catalogue's own
// `avatar <id> as <n>` lines say what each number draws, so a script picks a
// costume without knowing anything about the art that fulfils it -- the same
// division set_animation makes for a state.
//
// Only players have one. An enemy is drawn from its kind, and asking a monster
// for its avatar is a question with no answer rather than a zero.
int l_entity_avatar(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.avatar == nullptr) { lua_pushnil(L); return 1; }
    if (ctx.world != nullptr && ctx.world->agent(static_cast<u32>(a)).kind != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<double>(ctx.avatar[a]));
    return 1;
}

int l_entity_set_avatar(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.avatar == nullptr) return 0;
    const double n = luaL_checknumber(L, 2);
    if (n < 0 || n > 65535) {
        return luaL_error(L, "set_avatar: %f is not an avatar number (0..65535)", n);
    }
    ctx.avatar[a] = static_cast<u16>(n);
    return 0;
}

// entity:set_layer(slot, kind), entity:layer(slot), entity:set_anim(from, to)
//
// WHAT ELSE A PLAYER WEARS, drawn on its avatar (proto::Look). A LAYER is a
// catalogue record -- armour, a weapon in hand -- drawn on the body in its own
// state, clock and facing; eight slots, 1 to 8, 1 nearest the body. A kind
// number or a record's name; nil or 0 takes it off. An ANIMATION REMAP plays
// state `to` wherever the body would play `from`, the layers too: an attack
// that looks different with each weapon. nil takes it back. Eight of them.
//
// Players only, as the avatar is, and server state as it is.
static i32 look_slot(lua_State* L, WorldContext& ctx) {
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.look == nullptr) return -1;
    if (ctx.world != nullptr && ctx.world->agent(static_cast<u32>(a)).kind != 0) return -1;
    return a;
}
static void look_changed(WorldContext& ctx, i32 a) {
    u16& r = ctx.look_rev[a];
    r = static_cast<u16>(r == 0xFFFF ? 1 : r + 1);   // never back to 0, "never set"
}
int l_entity_layer(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = look_slot(L, ctx);
    const lua_Integer slot = luaL_checkinteger(L, 2);
    if (a < 0 || slot < 1 || slot > static_cast<lua_Integer>(proto::LOOK_LAYERS)) { lua_pushnil(L); return 1; }
    const u16 k = ctx.look[a].layer[slot - 1];
    if (k == 0) lua_pushnil(L); else lua_pushnumber(L, k);
    return 1;
}
int l_entity_set_layer(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const lua_Integer slot = luaL_checkinteger(L, 2);
    if (slot < 1 || slot > static_cast<lua_Integer>(proto::LOOK_LAYERS)) {
        return luaL_error(L, "set_layer: slot %d is not 1..%d", static_cast<int>(slot), static_cast<int>(proto::LOOK_LAYERS));
    }
    u16 kind = 0;
    if (lua_type(L, 3) == LUA_TSTRING) {
        size_t len = 0;
        const char* name = lua_tolstring(L, 3, &len);
        kind = (ctx.props == nullptr || ctx.props->empty()) ? 0 : ctx.props->kind_by_name(name, len);
        if (kind == 0) return luaL_error(L, "set_layer: no prop called '%s' in the catalogue", name);
    } else if (!lua_isnoneornil(L, 3)) {
        const double n = luaL_checknumber(L, 3);
        if (n < 0 || n > 65535) return luaL_error(L, "set_layer: %f is not a prop kind (0..65535)", n);
        kind = static_cast<u16>(n);
    }
    const i32 a = look_slot(L, ctx);
    if (a < 0) return 0;
    if (ctx.look[a].layer[slot - 1] == kind) return 0;
    ctx.look[a].layer[slot - 1] = kind;
    look_changed(ctx, a);
    return 0;
}
int l_entity_set_anim(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const double from = luaL_checknumber(L, 2);
    const bool clear = lua_isnoneornil(L, 3);
    const double to = clear ? 0 : luaL_checknumber(L, 3);
    if (from < 0 || from > 65535 || to < 0 || to > 65535) return luaL_error(L, "set_anim: states are 0..65535");
    const i32 a = look_slot(L, ctx);
    if (a < 0) return 0;
    proto::Look& l = ctx.look[a];
    u32 at = 0;
    while (at < l.remaps && l.from[at] != static_cast<u16>(from)) ++at;
    if (clear) {
        if (at == l.remaps) return 0;
        for (u32 j = at + 1; j < l.remaps; ++j) { l.from[j - 1] = l.from[j]; l.to[j - 1] = l.to[j]; }
        --l.remaps;
    } else {
        if (at < l.remaps && l.to[at] == static_cast<u16>(to)) return 0;
        if (at == l.remaps) {
            if (l.remaps >= proto::LOOK_REMAPS) return luaL_error(L, "set_anim: a player holds %d remaps at most", static_cast<int>(proto::LOOK_REMAPS));
            l.from[l.remaps++] = static_cast<u16>(from);
        }
        l.to[at] = static_cast<u16>(to);
    }
    look_changed(ctx, a);
    return 0;
}

// entity:set_trail(kind) and entity:trail(), and the same on a prop
//
// A PARTICLE KIND THAT FOLLOWS THIS BODY for as long as it is here: a missile's
// smoke, a fireball's flame, a monster's aura. The client keeps an emitter of
// that `vfx` kind on the body every frame it draws it, and lets it stop when
// the trail is set back to 0 or the body goes -- so it costs one field, sent
// when it changes, and no event a tick. The kind is drawn by the client's
// catalogue (assets/props.txt); a number nobody describes draws nothing.
//
// Server state, as the avatar is: not physics, not hashed, not predicted. The
// slot's generation is kept with it, so a body that takes the slot over later
// starts with none.
int trail_get(lua_State* L, WorldContext& ctx, i32 e) {
    if (e < 0 || ctx.trail == nullptr || ctx.world == nullptr) { lua_pushnil(L); return 1; }
    const u32 i = static_cast<u32>(e);
    lua_pushnumber(L, ctx.trail_gen[i] == ctx.world->generation(i) ? ctx.trail[i] : 0);
    return 1;
}
int trail_set(lua_State* L, WorldContext& ctx, i32 e) {
    if (e < 0 || ctx.trail == nullptr || ctx.world == nullptr) return 0;
    const double n = luaL_checknumber(L, 2);
    if (!(n >= 0 && n <= 65535) || n != static_cast<double>(static_cast<u32>(n))) {
        return luaL_error(L, "set_trail: %f is not a particle kind (0..65535; 0 is none)", n);
    }
    const u32 i = static_cast<u32>(e);
    ctx.trail[i] = static_cast<u16>(n);
    ctx.trail_gen[i] = ctx.world->generation(i);
    return 0;
}
int l_entity_trail(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    return trail_get(L, ctx, resolve(L, ctx, check_handle(L, 1)));
}
int l_entity_set_trail(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    return trail_set(L, ctx, resolve(L, ctx, check_handle(L, 1)));
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

// THE MOVEMENT STATES (docs/MOVEMENT.md), by the names the module gave them.
// A name it did not declare is an error: a typo should stop the script, not
// quietly do nothing.
const physics::MoveDefs& move_defs_of(WorldContext& ctx) { return ctx.world->move_defs(); }

int named(lua_State* L, int idx, i32 found, const char* what) {
    if (found < 0) return luaL_error(L, "no %s is called '%s' (module.txt)", what, luaL_checkstring(L, idx));
    return found;
}

// e:movement_state() -- its name; "default" for state 0.
int l_entity_movement_state(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    const u8 s = ctx.world->agent(static_cast<u32>(a)).mv.move_state;
    const physics::MoveDefs& d = move_defs_of(ctx);
    lua_pushstring(L, s < d.state_count ? d.states[s].name : "default");
    return 1;
}

// e:ask_movement_state(name) -- taken by a transition that says `asked`.
int l_entity_ask_movement_state(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const char* n = luaL_checkstring(L, 2);
    const i32 s = std::strcmp(n, "default") == 0 ? 0 : physics::move_state_index(move_defs_of(ctx), n);
    named(L, 2, s, "movement state");
    if (a < 0) return 0;
    if (ctx.commands->push(Command{CmdKind::AskState, static_cast<u32>(a), fx3{fx::from_int(s), fx::zero(), fx::zero()}})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// e:meter(name) -- its value now and its max, two fx.
int l_entity_meter(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const i32 m = named(L, 2, physics::meter_index(move_defs_of(ctx), luaL_checkstring(L, 2)), "meter");
    if (a < 0) { lua_pushnil(L); return 1; }
    push_fx(L, ctx.world->agent(static_cast<u32>(a)).mv.meters[m]);
    push_fx(L, move_defs_of(ctx).meters[m].max);
    return 2;
}

// e:set_meter(name, v) -- from the next tick; kept within 0..max.
int l_entity_set_meter(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const i32 m = named(L, 2, physics::meter_index(move_defs_of(ctx), luaL_checkstring(L, 2)), "meter");
    const fx v = check_fx(L, 3);
    if (a < 0) return 0;
    if (ctx.commands->push(Command{CmdKind::SetMeter, static_cast<u32>(a), fx3{fx::from_int(m), v, fx::zero()}})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// e:start_move(name) -- as a transition's `do move` would.
int l_entity_start_move(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const i32 k = named(L, 2, physics::layered_move_index(move_defs_of(ctx), luaL_checkstring(L, 2)), "move");
    if (a < 0) return 0;
    if (ctx.commands->push(Command{CmdKind::StartMove, static_cast<u32>(a), fx3{fx::from_int(k), fx::zero(), fx::zero()}})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}

// e:set_movement_state_enabled(name, on) / e:set_move_enabled(name, on) --
// switch one of the module's states or layered moves on or off for this body,
// from the next tick (docs/MOVEMENT.md "Switching them off"). `on` defaults to
// true. `default` is always on: naming it is an error, as a typo is.
int enable_movement(lua_State* L, bool state) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const char* n = luaL_checkstring(L, 2);
    if (state && std::strcmp(n, "default") == 0) return luaL_error(L, "'default' is always on");
    const i32 k = state ? named(L, 2, physics::move_state_index(move_defs_of(ctx), n), "movement state")
                        : named(L, 2, physics::layered_move_index(move_defs_of(ctx), n), "move");
    const bool on = lua_isnoneornil(L, 3) || lua_toboolean(L, 3);
    if (a < 0) return 0;
    if (ctx.commands->push(Command{state ? CmdKind::EnableState : CmdKind::EnableMove, static_cast<u32>(a),
                                   fx3{fx::from_int(k), on ? fx::one() : fx::zero(), fx::zero()}})) {
        ++ctx.stats.commands_queued;
    }
    return 0;
}
int l_entity_set_movement_state_enabled(lua_State* L) { return enable_movement(L, true); }
int l_entity_set_move_enabled(lua_State* L) { return enable_movement(L, false); }

// e:movement_state_enabled(name) / e:move_enabled(name) -- whether it is on now
// (a switch queued this tick shows from the next).
int movement_enabled(lua_State* L, bool state) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const char* n = luaL_checkstring(L, 2);
    if (state && std::strcmp(n, "default") == 0) { lua_pushboolean(L, a >= 0); return 1; }
    const i32 k = state ? named(L, 2, physics::move_state_index(move_defs_of(ctx), n), "movement state")
                        : named(L, 2, physics::layered_move_index(move_defs_of(ctx), n), "move");
    if (a < 0) { lua_pushnil(L); return 1; }
    const physics::MoveState& m = ctx.world->agent(static_cast<u32>(a)).mv;
    const u8 bits = state ? m.states_off : m.moves_off;
    lua_pushboolean(L, ((bits >> (state ? k - 1 : k)) & 1u) == 0);
    return 1;
}
int l_entity_movement_state_enabled(lua_State* L) { return movement_enabled(L, true); }
int l_entity_move_enabled(lua_State* L) { return movement_enabled(L, false); }

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
    // As the rules let it act: a frozen body (a cinematic) presses nothing.
    lua_pushnumber(L, ctx.world->live_input(static_cast<u32>(a)).buttons);
    return 1;
}

// entity:actions() -> the module's own keys held this tick, as a bitmask: bit
// i is dnp.actions()[i + 1] (docs/KEYS.md). Read by graphs' button nodes.
int l_entity_actions(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, ctx.world->input(static_cast<u32>(a)).actions);
    return 1;
}

// dnp.actions() -> the module's own key names, in bit order.
int l_dnp_actions(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    lua_newtable(L);
    if (ctx.actions == nullptr) return 1;
    for (size_t i = 0; i < ctx.actions->size(); ++i) {
        lua_pushstring(L, (*ctx.actions)[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
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

// entity:set_body_param(name, value)
//
// Write a movement parameter into the body ITSELF, permanently, rather than
// stacking a modifier on top of it.
//
// WHY THIS IS NOT add_effect. The effect stack is for TEMPORARY modifiers -- it
// recomputes from base every tick so it cannot drift, and it holds MAX_EFFECTS
// of them for the whole world, 256. A property of a body, spent once at spawn
// and never removed, is not a modifier: a level of a hundred monsters each
// declaring a swing would need eight slots apiece and exhaust the stack, and the
// monsters past the limit would silently get the engine's defaults.
//
// The base is re-seeded along with the live value, so effects still compose on
// top of the new number rather than on top of the one it replaced -- which is
// the whole reason this is a binding and not two calls a script makes in order.
//
// Scripted agents only, the same rule set_input follows. A player's body comes
// from the catalogue and their session; a script rewriting it would be changing
// physics the client predicts with no epoch to notice it by.
int l_entity_set_body_param(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || !ctx.effects) return 0;
    if (!ctx.world->agent(static_cast<u32>(a)).scripted) {
        return luaL_error(L, "entity %d is a player; its body comes from the "
                             "catalogue and its session", a);
    }
    const char* name = luaL_checkstring(L, 2);
    const i32 id = physics::param_id_by_name(name);
    if (id < 0) return luaL_error(L, "no movement parameter named '%s'", name);
    // AN fx EITHER WAY, as the effect stack takes one: a script says 13 and
    // means the number, not a bit pattern. So an integral field -- every U8 and
    // every Bool -- is TRUNCATED rather than having a Q32.32 raw poured into it.
    //
    // Skipping this wrote 13 * 2^32 into `attack_ticks`, a u8, and the swing came
    // out 254 ticks long: four seconds of collider that then counted down across
    // every following swing, so the first one worked and none of the rest ever
    // started. The identical mistake effects.hpp:145 records having made.
    const fx v = check_fx(L, 3);
    const bool integral =
        physics::PARAM_TABLE[static_cast<u16>(id)].type != physics::ParamType::Fx;
    physics::param_set_raw(ctx.world->params_mut(static_cast<u32>(a)),
                           static_cast<u16>(id), integral ? v.trunc_int() : v.raw);
    ctx.effects->set_base(static_cast<u32>(a),
                          ctx.world->params(static_cast<u32>(a)));
    if (ctx.ui) ctx.ui->body_param_changed(ctx.level, static_cast<u32>(a), static_cast<u16>(id));
    return 0;
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
    {"jumps", l_entity_jumps},
    {"pitch", l_entity_pitch},
    {"sector", l_entity_sector},
    {"effect_sectors", l_entity_effect_sectors},
    {"index", l_entity_index_of},
    {"player", l_entity_player},
    {"input_tick", l_entity_input_tick},
    {"teleport", l_entity_teleport},
    {"set_input", l_entity_set_input},
    {"press", l_entity_press},
    {"release", l_entity_release},
    {"swing", l_entity_swing},
    {"set_solid", l_entity_set_solid},
    {"solid", l_entity_solid},
    {"hp", l_entity_hp},
    {"set_hp", l_entity_set_hp},
    {"avatar", l_entity_avatar},
    {"set_avatar", l_entity_set_avatar},
    {"layer", l_entity_layer},
    {"set_layer", l_entity_set_layer},
    {"set_anim", l_entity_set_anim},
    {"trail", l_entity_trail},
    {"set_trail", l_entity_set_trail},
    {"set_animation", l_entity_set_animation},
    {"movement_state", l_entity_movement_state},
    {"ask_movement_state", l_entity_ask_movement_state},
    {"meter", l_entity_meter},
    {"set_meter", l_entity_set_meter},
    {"start_move", l_entity_start_move},
    {"set_movement_state_enabled", l_entity_set_movement_state_enabled},
    {"set_move_enabled", l_entity_set_move_enabled},
    {"movement_state_enabled", l_entity_movement_state_enabled},
    {"move_enabled", l_entity_move_enabled},
    {"kind", l_entity_kind},
    {"name", l_entity_name},
    {"facing", l_entity_facing},
    {"buttons", l_entity_buttons},
    {"actions", l_entity_actions},
    {"despawn", l_entity_despawn},
    {"change_map", l_entity_change_map},
    {"add_impulse", l_entity_add_impulse},
    {"set_velocity", l_entity_set_velocity},
    {"param", l_entity_param},
    {"base_param", l_entity_base_param},
    {"set_body_param", l_entity_set_body_param},
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
    push_vec3(L, ctx.to_local(ctx.world->prop(static_cast<u32>(e)).pos));
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
    const fx3 p = ctx.to_world(check_vec3(L, 2));
    // Quantised, for the reason spawn_prop is: a solid prop is simulated on
    // both sides and entity positions are quantised on the wire, so a value
    // the wire cannot carry exactly makes the two copies disagree.
    ctx.world->prop_mut(static_cast<u32>(e)).pos =
        fx3{proto::fxq_to_fx(proto::fx_to_fxq(p.x)),
            proto::fxq_to_fx(proto::fx_to_fxq(p.y)),
            proto::fxq_to_fx(proto::fx_to_fxq(p.z))};
    return 0;
}

// prop:set_facing(brad)
//
// Which way a prop is POINTING, which is not something it has until a script
// says so: a key and a barrel have no heading worth replicating, and every prop
// spawned so far has been one of those.
//
// A MISSILE is not. `PropState::facing` has existed and been replicated in the
// entity block since props were drawn at all, and nothing could write it -- so
// every rocket in flight reported a heading of zero, and an eight-view sprite
// with eight rotations extracted showed rotation 1 from every angle. It read as
// the sprite lacking its rotations, which it does not.
int l_prop_trail(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    return trail_get(L, ctx, resolve_prop(L, ctx, check_prop_handle(L, 1)));
}
int l_prop_set_trail(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    return trail_set(L, ctx, resolve_prop(L, ctx, check_prop_handle(L, 1)));
}

int l_prop_set_facing(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 e = resolve_prop(L, ctx, check_prop_handle(L, 1));
    if (e < 0) return 0;
    ctx.world->prop_mut(static_cast<u32>(e)).facing =
        check_brad(L, 2);
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
    // Its trail goes with it (the generation stamp already says so; this is for
    // a slot reused often enough for the stamp to come round again).
    if (ctx.trail != nullptr) ctx.trail[e] = 0;
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
    {"set_facing", l_prop_set_facing},
    {"trail", l_prop_trail},
    {"set_trail", l_prop_set_trail},
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
    // The level's own number (-1 for another level's sector, reached across a
    // join): the object holds the world's index, the script sees its own.
    lua_pushnumber(L, ctx_of(L).sector_to_local(static_cast<i32>(check_sector(L, 1))));
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

// sector:center() -> vec3: the middle of its bounding box, at its floor. Where
// a door or a lift is heard from -- a sector has no position of its own.
int l_sector_center(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    const geom::MapData& m = *ctx.world->map();
    const geom::Sector& sec = m.sector(s);
    if (sec.point_count == 0) { lua_pushnil(L); return 1; }
    fx2 lo = m.point(sec.first_point), hi = lo;
    for (u32 i = 1; i < sec.point_count; ++i) {
        const fx2 p = m.point(static_cast<u32>(sec.first_point) + i);
        if (p.x < lo.x) lo.x = p.x;
        if (p.y < lo.y) lo.y = p.y;
        if (p.x > hi.x) hi.x = p.x;
        if (p.y > hi.y) hi.y = p.y;
    }
    const fx two = fx::from_int(2);
    push_vec3(L, ctx.to_local(fx3{(lo.x + hi.x) / two, (lo.y + hi.y) / two, sec.floor_z}));
    return 1;
}

int l_sector_ceil(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 s = check_sector(L, 1);
    if (!sector_in_range(ctx, s)) { lua_pushnil(L); return 1; }
    push_fx(L, ctx.world->map()->sector(s).ceil_z);
    return 1;
}

// sector:move_parametric(from, to, duration_ticks [, loop] [, target]
//                        [, spin_degrees] [, pivot])
//
// PARAMETRIC: a pure function of the tick, so it is replicated once and every
// client computes it for ticks the server has not reached (design §3.1.2). Free
// to replicate and free to predict.
//
// `spin_degrees` turns the footprint about `pivot` over the same duration, so a
// platform that slides and turns is one motion arriving at both endpoints
// together. The pivot defaults to the mean of the sector's corners, which is
// what an author means by "spin it" and saves them looking up world
// coordinates for a sector they identified by tag.
//
// The turn is measured FROM WHERE THE SECTOR IS, not from its authored
// orientation. A second spin on a sector already turned ninety degrees would
// otherwise snap it back to square before starting.
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

    // THE TURN. Degrees in, brad out, through the fx the argument already is:
    // a parsed fx holds `deg * 2^32` exactly, so the brad is that raw over 360
    // and there is no double anywhere in it.
    i64 spin_from = 0;
    i64 spin_to = 0;
    fx2 pivot{};
    if (!lua_isnoneornil(L, 7)) {
        const fx deg = check_fx(L, 7);
        const i32 mi = ctx.world->motions().find(s);
        if (mi >= 0) spin_from = static_cast<i64>(ctx.world->motions().at(static_cast<u32>(mi)).angle);
        spin_to = spin_from + deg.raw / 360;

        if (!lua_isnoneornil(L, 8)) {
            const fx3 p = ctx.to_world(check_vec3(L, 8));
            pivot = fx2{p.x, p.y};
        } else {
            // The mean of the corners, the same one the nav graph measures its
            // costs between -- deliberately not the area centroid, for the
            // reason navgraph.cpp gives: a shape's corners are what a person
            // drew, and the mean of them is where they would point.
            const geom::Sector& sec = ctx.world->map()->sector(s);
            i64 sx = 0, sy = 0;
            for (u16 k = 0; k < sec.point_count; ++k) {
                sx += ctx.world->map()->point(sec.first_point + k).x.raw;
                sy += ctx.world->map()->point(sec.first_point + k).y.raw;
            }
            if (sec.point_count > 0) {
                pivot = fx2{fx::from_raw(sx / sec.point_count),
                            fx::from_raw(sy / sec.point_count)};
            }
        }
    }

    // start_tick is NOW, not zero: a motion declared at load time and one
    // declared by a trigger mid-match must both mean "begin from here".
    lua_pushboolean(L, ctx.world->motions().start_parametric(
        s, from, to, ctx.world->tick(), dur, loop, target,
        pivot, spin_from, spin_to) ? 1 : 0);
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
    {"center", l_sector_center},
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
        // In a shared world: this level's own, and the players. Another level's
        // monsters are that level's script's business.
        if (ctx.owner && ctx.world->agent(i).kind != 0 && ctx.owner[i] != ctx.level) continue;
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
    // The level's own sectors, keyed by its own numbers.
    const u32 n = ctx.chunk ? ctx.chunk->sectors.count
                            : (ctx.world->map() ? ctx.world->map()->sector_count : 0);
    for (u32 i = 0; i < n; ++i) {
        push_sector(L, static_cast<u32>(ctx.sector_from_local(static_cast<i32>(i))));
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
    const int i = ctx.sector_from_local(static_cast<int>(luaL_checknumber(L, 1)));
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
    const int i = ctx.side_from_local(static_cast<int>(luaL_checknumber(L, 1)));
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
// dnp.prop_kind(name) -> the catalogue kind of a record by name, or nil. For a
// loader that wants to refuse a misspelt item when it reads the table rather
// than when the first one fails to spawn.
int l_dnp_prop_kind(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const u16 kind = (ctx.props == nullptr || ctx.props->empty()) ? 0 : ctx.props->kind_by_name(name, len);
    if (kind == 0) lua_pushnil(L);
    else lua_pushnumber(L, kind);
    return 1;
}

int l_dnp_spawn_prop(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const fx3 asked = check_vec3(L, 2);
    fx3 pos = ctx.to_world(asked);
    // HELD BACK while a level is restored: whatever its scripts place as they
    // load, the saved state puts back instead (docs/SAVING.md). Caught
    // here rather than trusted to every script, because any script can place
    // things -- including ones nobody thought to teach about restoring.
    // EXCEPT a placement the save says is still there (see Placement): that one
    // comes back, where it was left.
    WorldContext::KeptPlacement* kept = nullptr;
    if (ctx.restoring) {
        for (WorldContext::KeptPlacement& k : ctx.kept_placements) {
            if (k.used || k.asked != asked || k.name.size() != len ||
                std::memcmp(k.name.data(), name, len) != 0) continue;
            k.used = true;
            if (k.there) kept = &k;
            break;
        }
        if (kept == nullptr) {
            ++ctx.held_back_spawns;
            lua_pushnil(L);
            return 1;
        }
        pos = ctx.to_world(kept->at);
    }

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
    if (kept != nullptr) ctx.world->prop_mut(e).facing = kept->facing;
    ctx.own(e);
    if (ctx.loading) {
        ctx.placements.push_back({std::string(name, len), asked, e, ctx.world->generation(e)});
    }
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
    // HELD BACK while a level is restored: whatever its scripts place as they
    // load, the saved state puts back instead (docs/SAVING.md). Caught
    // here rather than trusted to every script, because any script can place
    // things -- including ones nobody thought to teach about restoring.
    if (ctx.restoring) {
        ++ctx.held_back_spawns;
        lua_pushnil(L);
        return 1;
    }
    size_t len = 0;
    const char* name = luaL_checklstring(L, 1, &len);
    const fx3 pos = ctx.to_world(check_vec3(L, 2));

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
    ctx.own(a);
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
        if (ctx.owner && ctx.owner[e] != ctx.level) continue;   // see dnp.entities
        push_prop(L, ctx, e);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// dnp.hitscan(from_entity, range, [spread_brad], [slope]) -> hit | nil, impact
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
    // An OPTIONAL angular offset from the body's facing, in brad, so one pull
    // of a trigger can put several pellets in different places. The offset,
    // not the randomness: where a pellet goes is the script's decision (or,
    // for a player's own weapon, the core's -- docs/WEAPON_PREDICTION.md).
    // brad wraps by design, so a negative offset goes round the other way.
    const i64 spread = static_cast<i64>(luaL_optnumber(L, 3, 0));
    const brad aim = sh.facing + brad(static_cast<u32>(spread));
    // AND AN OPTIONAL SLOPE: rise per unit of horizontal run, zero for the flat
    // ray this binding cast for its whole life. Without one a body whose middle
    // is not level with yours could not be shot at all.
    const fx slope = lua_isnoneornil(L, 4) ? fx::zero() : check_fx(L, 4);

    // The shot itself is the core's (World::hitscan), so a predicting client
    // asks its own world the identical question and draws the same tracer.
    // Judged where this shooter SAW the others (lag compensation).
    const RewindForShot rewound(ctx, static_cast<u32>(a));
    const sim::World::ShotHit hit = ctx.world->hitscan(static_cast<u32>(a), aim, slope, range);
    if (hit.entity < 0) lua_pushnil(L);
    else if (sim::World::is_prop(static_cast<u32>(hit.entity))) push_prop(L, ctx, static_cast<u32>(hit.entity));
    else push_entity(L, ctx, static_cast<u32>(hit.entity));
    push_vec3(L, ctx.to_local(hit.at));
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
// dnp.level() -> which level this script is running in
//
// The index `entity:change_map` takes and the position in `dnp.levels()`, minus
// one for Lua's counting. A map's OWN script never needs it -- it knows what map
// it is by being in it -- but a SHARED script loaded into every level does, and
// without it the only way to tell them apart was to compare names against a
// list the script had to be told.
int l_dnp_level(lua_State* L) {
    lua_pushnumber(L, ctx_of(L).level);
    return 1;
}

// dnp.spawn_point(tag) -> vec3, or nil when this level has no spawn so tagged
//
// Where a map's tagged spawn point is, in this level's coordinates: what a
// script needs to send somebody to it without a map change.
int l_dnp_spawn_point(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t n = 0;
    const char* tag = luaL_checklstring(L, 1, &n);
    // The level's own spawns, in its own coordinates -- which is what a script
    // passes back (e:teleport translates to the world).
    const geom::MapStorage* m = ctx.own_map();
    if (m == nullptr || !m->has_spawn_tag(tag, static_cast<u32>(n))) {
        lua_pushnil(L);
        return 1;
    }
    push_vec3(L, m->spawn_for_tag(tag, static_cast<u32>(n)));
    return 1;
}

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

// dnp.ui_open(entity, { title = "...", template = "shop", widgets = { {...}, ... } }) -> ui_id
//
// `template` names one of the module's layout panels (docs/UI_LAYOUT.md 5):
// where the panel goes and how it looks. Presentation only -- a client without
// the template draws the default panel with the same widgets.
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

    // `pinned = true`: no ×, so it stays until the script closes it.
    lua_getfield(L, 2, "pinned");
    if (lua_toboolean(L, -1)) panel.flags |= proto::UI_FLAG_PINNED;
    lua_pop(L, 1);

    lua_getfield(L, 2, "template");
    if (!lua_isnil(L, -1)) {
        size_t tn = 0;
        const char* tp = lua_tolstring(L, -1, &tn);
        if (tp == nullptr || !proto::valid_template_name(tp, tn)) {
            return luaL_error(L, "ui: template '%s' must be 1..%d of [a-z0-9_]",
                              tp ? tp : "?", static_cast<int>(proto::UI_MAX_TEMPLATE));
        }
        panel.template_len = static_cast<u8>(tn);
        for (size_t i = 0; i < tn; ++i) panel.template_name[i] = tp[i];
    }
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

// dnp.ui_show(entity, ui_id, widget, shown)
//
// Show or hide part of a panel that is already open: a widget by its id (a
// number), or one of its template's decor elements by name (a string). Hidden
// is not closed -- the panel keeps the widget's value, and showing it again
// puts it back where it was.
int l_dnp_ui_show(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) return 0;
    const u32 ui_id = static_cast<u32>(luaL_checknumber(L, 2));
    const bool shown = lua_toboolean(L, 4) != 0;
    if (lua_type(L, 3) == LUA_TSTRING) {
        size_t n = 0;
        const char* name = lua_tolstring(L, 3, &n);
        if (n == 0 || n > 32) return luaL_error(L, "dnp.ui_show: a decor element's name is 1..32 bytes");
        ctx.ui->show_ui(ctx.level, static_cast<u32>(a), ui_id, 0, std::string(name, n), shown);
    } else {
        ctx.ui->show_ui(ctx.level, static_cast<u32>(a), ui_id, static_cast<u16>(luaL_checknumber(L, 3)),
                        std::string(), shown);
    }
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
    "lead", "lead_smoothing", "deadzone_x", "deadzone_y",
    // Where the camera stands around the subject, and whether the player may
    // turn or zoom it away from there (docs/protocol.md §11.1, fields 8..10).
    "yaw", "lock_yaw", "lock_zoom",
    // Camera paths and shake (design §4.1, fields 11..17). APPENDED, and the
    // reason is the whole contract of this array: its INDEX is the wire id, so
    // a name inserted rather than appended silently repoints every field after
    // it -- a map asking for a longer lead would get a different pitch, in
    // every level already shipped.
    //
    // cam_path carries a TAG. An untagged path defaults to tag 0, so 0 is a
    // legal path and cannot mean "none"; negative does.
    "cam_mode", "cam_path", "cam_path_speed", "cam_path_attach", "cam_path_blend",
    // Edge-triggered, unlike everything above it: a shake is an event, not a
    // setting, and the client fires one when the field ARRIVES rather than
    // holding it. See docs/protocol.md §11.1.
    "cam_shake", "cam_shake_time",
    // How much further ahead the camera aims the faster its subject runs
    // forward: forward speed times this, added to `lead` (field 18).
    "lead_velocity",
    // The automatic camera (fields 19..22): it swings round behind the player
    // unless they turned it in the last auto_delay seconds, at auto_rate; and
    // with collide, walls pull it in.
    "auto_turn", "auto_delay", "auto_rate", "collide",
    // Where the camera sits and looks, by height over the player's feet
    // (fields 23, 24): camera_height (negative: the pitch decides, as before) and
    // aim_height, the point it looks at.
    "camera_height", "aim_height",
    // The automatic camera's cone (fields 25, 26): degrees across, from the
    // camera to the point it looks at, kept clear of walls by turning round the
    // player; and how fast it turns to.
    "cone", "cone_rate"
};

// The fields a Lua table names, in thousandths. Shared by dnp.camera and
// dnp.level_camera, which differ only in whom the settings are for.
static void read_camera_table(lua_State* L, int idx, proto::CameraSet& c) {
    for (u8 id = 0; id < static_cast<u8>(sizeof(CAMERA_FIELD_NAMES) / sizeof(char*)); ++id) {
        // The array is sized for the name list, so this cannot fire today -- and
        // that is exactly the problem it exists for. The loop is bounded by the
        // list rather than by the buffer, so the day someone appends a name past
        // CAMERA_MAX_FIELDS the overrun is silent and nothing here mentions it.
        if (c.count >= proto::CAMERA_MAX_FIELDS) break;
        lua_getfield(L, idx, CAMERA_FIELD_NAMES[id]);
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
}

int l_dnp_camera(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    if (a < 0 || ctx.ui == nullptr) return 0;
    luaL_checktype(L, 2, LUA_TTABLE);
    proto::CameraSet c;
    read_camera_table(L, 2, c);
    if (c.count > 0) ctx.ui->set_camera(ctx.level, static_cast<u32>(a), c);
    return 0;
}

// dnp.level_camera{ yaw = 0.125, pitch = 0.05, distance = 30, lock_yaw = 1 }
//
// The camera for everyone who joins this level, as the map's Lua says at load:
// the same fields dnp.camera takes, kept by the server and sent on join, and
// at once to whoever is already here. A script that wants one player's camera
// to differ says so with dnp.camera afterwards.
int l_dnp_level_camera(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    proto::CameraSet c;
    read_camera_table(L, 1, c);
    if (c.count > 0 && ctx.ui != nullptr) ctx.ui->set_level_camera(ctx.level, c);
    return 0;
}

// dnp.sky("strip SKY2 repeat 4") / dnp.sky{ kind = "cube", faces = {...}, yaw = 0.25 }
// dnp.sky(nil)
//
// This level's sky (docs/SKY.md), over the module's: for everyone here now and
// everyone who arrives. Words or a table, the same words a module.txt `sky`
// line takes after the level; nil puts the module's back. A sky it cannot read
// is an error, said with what was wrong, rather than a sky drawn wrongly.
int l_dnp_sky(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (lua_isnoneornil(L, 1) || (lua_isboolean(L, 1) && !lua_toboolean(L, 1))) {
        if (ctx.ui != nullptr) ctx.ui->set_level_sky(ctx.level, nullptr);
        return 0;
    }
    std::vector<std::string> words;
    if (lua_type(L, 1) == LUA_TSTRING) {
        words = server::sky_words(lua_tostring(L, 1));
    } else {
        luaL_checktype(L, 1, LUA_TTABLE);
        const auto str = [&](const char* key) -> std::string {
            lua_getfield(L, 1, key);
            std::string v = lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            return v;
        };
        words.push_back(str("kind"));
        if (!str("image").empty()) words.push_back(str("image"));
        lua_getfield(L, 1, "faces");
        if (lua_istable(L, -1)) {
            for (int i = 1; i <= 6; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING) words.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        for (const char* k : {"repeat", "yaw", "height"}) {
            const std::string v = str(k);
            if (!v.empty()) { words.push_back(k); words.push_back(v); }
        }
    }
    proto::SkySet sky;
    std::string err;
    if (!server::parse_sky(words, sky, err)) return luaL_error(L, "dnp.sky: %s", err.c_str());
    if (ctx.ui != nullptr) ctx.ui->set_level_sky(ctx.level, &sky);
    return 0;
}

// dnp.path_length(tag) -> units along the curve, or nil
//
// HOW A CUTSCENE KNOWS WHEN ITS SHOT ENDS.
//
// The engine has no completion event of any kind -- every hook it calls is a
// physics or input fact -- so a script learns that a flythrough has finished
// the way COND.sector_at_rest learns a door has stopped: by arithmetic done in
// advance. `run_after{ ticks = path_length(1) / speed * TICK_RATE }` is the
// whole mechanism.
//
// The number is the arc length of the CURVE the client rides, not of the
// polyline in the map, and the two differ through every bend. It is baked by
// tools/mapc rather than computed here, because the curve is the client's and
// core deliberately does not implement one -- so this reads the field rather
// than answering the question itself.
//
// nil, not 0, for a tag that is not in the map. Zero is a legal length: a path
// whose points coincide is degenerate rather than absent, and a script dividing
// by the difference should see the difference.
//
// An explicit nil, and RETURN 1 -- not `return 0`, which yields no value at all.
// The two look the same in `local n = dnp.path_length(t)` and are not the same
// anywhere else: `print(dnp.path_length(9))` and `tostring(...)` and passing it
// on all raise "value expected" on a call that returned nothing. Found by
// writing exactly that line against a real map.
int l_dnp_path_length(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 tag = static_cast<u32>(luaL_checkinteger(L, 1));
    // The level's own paths: the camera follows its home level's by tag.
    if (ctx.own_map() == nullptr) { lua_pushnil(L); return 1; }
    const geom::MapData m = ctx.own_map()->view();
    for (u32 i = 0; i < m.path_count; ++i) {
        // FIRST match. Tags are not unique and mapc does not enforce it, so the
        // rule is the same one every other tag lookup here uses.
        if (m.path(i).tag == tag) {
            lua_pushnumber(L, static_cast<double>(m.path(i).curve_length.raw) / 4294967296.0);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
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
    t.side = static_cast<u32>(side);
    t.field = f;
    if (!ui_text(L, 3, t.name, "texture name")) return 0;
    ctx.ui->set_side_texture(ctx.level, t);
    return 0;
}

// --- SECTOR LOOKS (SECTOR_SET; docs/GRAPHS.md "sectors") -------------------
//
// What a sector shows, changed while the game runs: kept by the server for
// whoever arrives, saved with the level, never simulated.

static u32 sector_arg(lua_State* L, WorldContext& ctx, int idx) {
    const geom::MapData* m = ctx.world->map();
    const lua_Number n = luaL_checknumber(L, idx);
    if (m == nullptr || n < 0 || n >= m->sector_count) {
        luaL_error(L, "no sector %d in this map", static_cast<int>(n));
        return 0;
    }
    return static_cast<u32>(n);
}

// dnp.set_sector_texture(sector, "floor" | "ceiling", name)
int l_dnp_set_sector_texture(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (ctx.ui == nullptr || ctx.world->map() == nullptr) return 0;
    proto::SectorSet t;
    t.sector = sector_arg(L, ctx, 1);
    const char* part = luaL_checkstring(L, 2);
    if (std::strcmp(part, "floor") == 0) t.what = proto::SectorWhat::Floor;
    else if (std::strcmp(part, "ceiling") == 0) t.what = proto::SectorWhat::Ceiling;
    else return luaL_error(L, "a sector's picture is its 'floor' or its 'ceiling', got '%s'", part);
    if (!ui_text(L, 3, t.texture, "texture name")) return 0;
    ctx.ui->set_sector(ctx.level, t);
    return 0;
}

// dnp.set_sector_light(sector, level [, seconds]) -- level 0..255, the map's
// own `light` scale; seconds is the fade, on every client at once.
int l_dnp_set_sector_light(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (ctx.ui == nullptr || ctx.world->map() == nullptr) return 0;
    proto::SectorSet t;
    t.sector = sector_arg(L, ctx, 1);
    t.what = proto::SectorWhat::Light;
    const lua_Number v = luaL_checknumber(L, 2);
    t.light = static_cast<u8>(v < 0 ? 0 : (v > 255 ? 255 : std::lround(v)));
    const lua_Number sec = luaL_optnumber(L, 3, 0);
    t.fade_ms = static_cast<u16>(sec <= 0 ? 0 : (sec >= 65.535 ? 65535 : std::lround(sec * 1000)));
    ctx.ui->set_sector(ctx.level, t);
    return 0;
}

// An effect setting from Lua: a number (rendering, so a double is exact
// enough), an fx, or a decimal string; a colour also as "#rrggbb".
static bool fx_setting(lua_State* L, int idx, bool is_color, i64& raw) {
    if (is_color && lua_type(L, idx) == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx);
        if (s[0] == '#') ++s;
        char* end = nullptr;
        const unsigned long v = std::strtoul(s, &end, 16);
        if (end == s || *end != '\0' || v > 0xFFFFFF) return false;
        raw = static_cast<i64>(v);
        return true;
    }
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const lua_Number n = lua_tonumber(L, idx);
        raw = is_color ? static_cast<i64>(n) : static_cast<i64>(std::llround(n * 4294967296.0));
        return !is_color || (n >= 0 && n <= 0xFFFFFF);
    }
    if (is_fx(L, idx) || lua_type(L, idx) == LUA_TSTRING) {
        raw = check_fx(L, idx).raw;
        return true;
    }
    return false;
}

// dnp.set_sector_effect(sector, kind [, settings | false])
//   kind: "rain" "snow" "dust" "embers" "fog" "fill"; settings by the map's own `fx`
//   key names (density, color, ...), each optional; false takes it off.
int l_dnp_set_sector_effect(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (ctx.ui == nullptr || ctx.world->map() == nullptr) return 0;
    proto::SectorSet t;
    t.sector = sector_arg(L, ctx, 1);
    t.what = proto::SectorWhat::Effect;
    const char* kind = luaL_checkstring(L, 2);
    for (u8 k = 1; k <= geom::FX_KIND_COUNT; ++k) if (std::strcmp(kind, geom::fx_kind_name(k)) == 0) t.fx.kind = k;
    if (t.fx.kind == 0) return luaL_error(L, "no sector effect '%s' (rain, snow, dust, embers, fog, fill)", kind);
    t.on = !(lua_isboolean(L, 3) && !lua_toboolean(L, 3));
    if (t.on && lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
            u8 id = 0;
            for (u8 k = 1; k <= geom::FX_KEY_COUNT; ++k) if (std::strcmp(key, geom::FX_KEYS[k - 1].name) == 0) id = k;
            if (id == 0 || !geom::fx_takes(t.fx.kind, id))
                return luaL_error(L, "%s takes no setting '%s'", kind, key);
            i64 raw = 0;
            if (!fx_setting(L, -1, geom::FX_KEYS[id - 1].is_color, raw))
                return luaL_error(L, "%s's %s is not a value it can take", kind, key);
            if (t.fx.count < geom::MAX_FX_PARAMS) t.fx.params[t.fx.count++] = geom::FxParam{id, raw};
            lua_pop(L, 1);
        }
    }
    ctx.ui->set_sector(ctx.level, t);
    return 0;
}

// dnp.music(entity_or_nil, cue [, fade_ms [, at]])
//
// A cue change for whoever is watching `entity`, or for everyone in the level
// when it is nil. The client resolves the NAME to a track from its manifest
// (client/src/audio/music.js, cue), so the wire carries the name and nothing
// else. `at` is "next_bar" (the default), "loop_end" or "now"; anything else
// is a script error, said at once, because a cue landing at some other time
// than asked is the kind of wrong nobody reports.
//
// A stale handle is the usual defined no-op; a monster's handle finds no
// session and sends nothing, which is not an error either.
int l_dnp_music(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (ctx.ui == nullptr) return 0;
    i32 agent = -1;
    if (!lua_isnoneornil(L, 1)) {
        agent = resolve(L, ctx, check_handle(L, 1));
        if (agent < 0) return 0;
    }
    proto::MusicCue m;
    if (!ui_text(L, 2, m.cue, "cue name")) return 0;
    const lua_Number fade = lua_isnoneornil(L, 3) ? 800 : luaL_checknumber(L, 3);
    m.fade_ms = static_cast<u16>(fade < 0 ? 0 : (fade > 65535 ? 65535 : fade));
    const char* at = lua_isnoneornil(L, 4) ? "next_bar" : luaL_checkstring(L, 4);
    if (std::strcmp(at, "next_bar") == 0)      m.at = proto::MusicAt::NextBar;
    else if (std::strcmp(at, "loop_end") == 0) m.at = proto::MusicAt::LoopEnd;
    else if (std::strcmp(at, "now") == 0)      m.at = proto::MusicAt::Now;
    else return luaL_error(L, "dnp.music: `at` is next_bar, loop_end or now, not '%s'", at);
    ctx.ui->music(ctx.level, agent, m);
    return 0;
}

// dnp.screen(entity, { fade = 1, fade_ms = 500, letterbox = 0.12 })
//
// What is drawn OVER everything: a black curtain and letterbox bars. The other
// half of a cutscene, beside the camera.
//
// Named fields and only the ones given, exactly as dnp.camera works: a script
// that wants to raise the bars should not have to restate the fade. `entity`
// nil means everyone in the level, as dnp.music does.
// `quit` (4) is a request, not a setting: the client closes the app (its
// shell's dnp.quit) or, in a plain browser tab, says it may be closed.
static const char* const SCREEN_FIELD_NAMES[] = {
    "fade", "fade_ms", "letterbox", "letterbox_ms", "quit"
};

int l_dnp_screen(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    if (ctx.ui == nullptr) return 0;
    i32 agent = -1;
    if (!lua_isnoneornil(L, 1)) {
        agent = resolve(L, ctx, check_handle(L, 1));
        if (agent < 0) return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    proto::ScreenSet sc;
    for (u8 id = 0; id < static_cast<u8>(sizeof(SCREEN_FIELD_NAMES) / sizeof(char*)); ++id) {
        // The bound CameraSet's loop should have had from the start. It cannot
        // fire while the array is larger than the name list, which is exactly
        // why it is written down rather than left to arithmetic.
        if (sc.count >= proto::SCREEN_MAX_FIELDS) break;
        lua_getfield(L, 2, SCREEN_FIELD_NAMES[id]);
        if (!lua_isnil(L, -1)) {
            const double v = lua_tonumber(L, -1);
            sc.fields[sc.count].id = id;
            sc.fields[sc.count].milli = static_cast<i32>(v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
            ++sc.count;
        }
        lua_pop(L, 1);
    }
    if (sc.count > 0) ctx.ui->screen(ctx.level, agent, sc);
    return 0;
}

// dnp.predict_sounds(e, group, { {on = "jump", sound = name, ...}, ... })
//
// What e's own client plays the moment it predicts its own action, replacing
// what `group` said before (docs/protocol.md §5.7). `on` is jump, land,
// primary, primary_held, secondary, secondary_held or swing; `every` is the
// ticks between two shots, `fall` the speed a landing needs (world units a
// second), `shots` how many are left (absent: no limit). The server's own copy
// of the same sound should be marked `predicted`, so this client drops it.
int l_dnp_predict_sounds(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    size_t gn = 0;
    const char* group = luaL_checklstring(L, 2, &gn);
    luaL_checktype(L, 3, LUA_TTABLE);
    if (a < 0 || ctx.ui == nullptr) return 0;
    static const struct { const char* name; u8 trigger; } ONS[] = {
        {"jump", proto::PREDICT_JUMP}, {"land", proto::PREDICT_LAND},
        {"primary", proto::PREDICT_PRIMARY}, {"primary_held", proto::PREDICT_PRIMARY_HELD},
        {"secondary", proto::PREDICT_SECONDARY}, {"secondary_held", proto::PREDICT_SECONDARY_HELD},
        {"swing", proto::PREDICT_SWING},
    };
    proto::PredictSound list[proto::MAX_PREDICT_SOUNDS];
    u32 n = 0;
    const int count = static_cast<int>(lua_objlen(L, 3));
    for (int i = 1; i <= count && n < proto::MAX_PREDICT_SOUNDS; ++i) {
        lua_rawgeti(L, 3, i);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        proto::PredictSound p;
        lua_getfield(L, -1, "on");
        const char* on = lua_tostring(L, -1);
        for (const auto& o : ONS) if (on && std::strcmp(on, o.name) == 0) p.trigger = o.trigger;
        lua_pop(L, 1);
        lua_getfield(L, -1, "sound");
        size_t sn = 0;
        const char* sname = lua_tolstring(L, -1, &sn);
        if (sname) p.sound = proto::sound_hash(sname, sn);
        lua_pop(L, 1);
        lua_getfield(L, -1, "every");
        if (lua_isnumber(L, -1)) p.param = static_cast<u16>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, -1, "fall");
        if (!lua_isnil(L, -1)) {
            const fx f = is_fx(L, -1) ? check_fx(L, -1) : fx::from_int(static_cast<i64>(lua_tonumber(L, -1)));
            p.param = static_cast<u16>(std::min<i64>(65535, (f * fx::from_int(16)).raw >> 32));
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "shots");
        if (lua_isnumber(L, -1)) p.shots = static_cast<i16>(std::min(32767.0, lua_tonumber(L, -1)));
        lua_pop(L, 1);
        lua_pop(L, 1);
        if (p.trigger == 0 || p.sound == 0) {
            return luaL_error(L, "predict_sounds: entry %d needs a known `on` and a `sound`", i);
        }
        list[n++] = p;
    }
    ctx.ui->predict_sounds(ctx.level, static_cast<u32>(a), proto::sound_hash(group, gn), list, n);
    return 0;
}

// dnp.hud(e_or_nil, { name = value, ... })
//
// Values for the HUD layout's server-known bindings (`hud.<name>`): a whole
// number, a string, or false to clear one. Sent only when a value changes, so a
// script may call this every tick for nothing (docs/UI_LAYOUT.md 4).
int l_dnp_hud(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    i32 agent = -1;
    if (!lua_isnoneornil(L, 1)) {
        agent = resolve(L, ctx, check_handle(L, 1));
        if (agent < 0) return 0;
    }
    luaL_checktype(L, 2, LUA_TTABLE);
    std::vector<HudSet> set;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        size_t n = 0;
        const char* name = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &n) : nullptr;
        if (name == nullptr || n == 0 || n > proto::HUD_NAME_MAX) {
            return luaL_error(L, "dnp.hud: a name must be a string of 1..%d characters",
                              static_cast<int>(proto::HUD_NAME_MAX));
        }
        for (size_t k = 0; k < n; ++k) {
            const char c = name[k];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                return luaL_error(L, "dnp.hud: '%s' -- a name is [a-z0-9_]", name);
            }
        }
        HudSet h;
        h.name.assign(name, n);
        if (lua_isboolean(L, -1)) {
            if (lua_toboolean(L, -1)) { h.kind = proto::HUD_INT; h.i = 1; }
            else h.kind = proto::HUD_CLEAR;
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            h.kind = proto::HUD_INT;
            h.i = static_cast<i32>(lua_tonumber(L, -1));
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            size_t sn = 0;
            const char* s = lua_tolstring(L, -1, &sn);
            h.kind = proto::HUD_STR;
            // Cut at a whole character: half a UTF-8 letter is not text.
            size_t n = std::min<size_t>(sn, proto::HUD_STR_MAX);
            if (n < sn) while (n > 0 && (static_cast<u8>(s[n]) & 0xC0) == 0x80) --n;
            h.s.assign(s, n);
        } else if (is_fx(L, -1)) {
            return luaL_error(L, "dnp.hud: '%s' is an fx; a HUD shows whole numbers -- "
                              "convert it (.int) first", name);
        } else {
            return luaL_error(L, "dnp.hud: '%s' must be a number, a string or false", name);
        }
        set.push_back(std::move(h));
        lua_pop(L, 1);
    }
    if (set.empty() || ctx.ui == nullptr) return 0;
    if (!ctx.ui->hud(ctx.level, agent, set)) {
        return luaL_error(L, "dnp.hud: more than %d names for one player",
                          static_cast<int>(proto::HUD_MAX_NAMES));
    }
    return 0;
}

// --- markers (docs/MARKERS.md) -------------------------------------------------

// A marker's values from {name = number | string | false} at `t`, merged into
// `m`: a name already there is replaced, `false` removes it, a new one is added
// (up to MARK_MAX_VALUES). The names and kinds dnp.hud takes, for the same
// reason -- a marker's template binds them as a HUD layout binds HUD values.
static void mark_values(lua_State* L, int t, proto::MarkSet& m, const char* who) {
    if (lua_isnoneornil(L, t)) return;
    luaL_checktype(L, t, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        size_t n = 0;
        const char* name = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &n) : nullptr;
        if (name == nullptr || n == 0 || n > proto::HUD_NAME_MAX) {
            luaL_error(L, "%s: a value's name must be a string of 1..%d characters", who,
                       static_cast<int>(proto::HUD_NAME_MAX));
        }
        for (size_t k = 0; k < n; ++k) {
            const char c = name[k];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                luaL_error(L, "%s: '%s' -- a name is [a-z0-9_]", who, name);
            }
        }
        i32 at = -1;
        for (u8 i = 0; i < m.count; ++i) {
            if (m.values[i].name_len == n && std::memcmp(m.values[i].name, name, n) == 0) { at = i; break; }
        }
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            if (at >= 0) {
                for (u8 i = static_cast<u8>(at); i + 1 < m.count; ++i) m.values[i] = m.values[i + 1];
                --m.count;
            }
            lua_pop(L, 1);
            continue;
        }
        if (at < 0) {
            if (m.count >= proto::MARK_MAX_VALUES) {
                luaL_error(L, "%s: more than %d values on one marker", who, static_cast<int>(proto::MARK_MAX_VALUES));
            }
            at = m.count++;
            m.values[at] = proto::MarkValue{};
            m.values[at].name_len = static_cast<u8>(n);
            std::memcpy(m.values[at].name, name, n);
        }
        proto::MarkValue& v = m.values[at];
        if (lua_isboolean(L, -1)) {
            v.kind = proto::HUD_INT; v.number = 1;
        } else if (lua_type(L, -1) == LUA_TNUMBER) {
            v.kind = proto::HUD_INT;
            v.number = static_cast<i32>(lua_tonumber(L, -1));
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            size_t sn = 0;
            const char* str = lua_tolstring(L, -1, &sn);
            v.kind = proto::HUD_STR;
            v.text = proto::UiText{};
            v.text.len = static_cast<u16>(std::min<size_t>(sn, proto::UI_MAX_TEXT));
            std::memcpy(v.text.text, str, v.text.len);
        } else if (is_fx(L, -1)) {
            luaL_error(L, "%s: '%s' is an fx; a marker shows whole numbers -- convert it (.int) first", who, name);
        } else {
            luaL_error(L, "%s: '%s' must be a number, a string or false", who, name);
        }
        lua_pop(L, 1);
    }
}

// dnp.mark(on, template, [values], [opts]) -> id | nil
//
// Hang a marker on `on` -- an entity, a prop, or a vec3 (a place in this
// level's frame) -- drawn from the module's UI layout template `template`
// (docs/MARKERS.md), its bindings reading `values`. opts:
//   to             a player entity: only they see it (a quest marker of theirs)
//   height         fx, above the top of the body, or above the point
//   range          fx, shown only within this of the viewer (default: always)
//   debug          true: drawn only with the debug overlay, and not sent at all
//                  when the module has it off (module.txt `debug off`)
//   through_walls  true: not hidden by the map between it and the camera
// Nil when it is not hung: nothing to hang it on, or a debug marker in a game
// that ships without the overlay.
int l_dnp_mark(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    proto::MarkSet m;
    if (is_vec3(L, 1)) {
        m.anchor = proto::MARK_AT_POINT;
        m.pos = ctx.to_world(check_vec3(L, 1));
    } else {
        i32 e = -1;
        if (proto::Handle* h = to_handle(L, 1)) e = resolve(L, ctx, *h);
        else if (proto::Handle* h = to_prop(L, 1)) e = resolve_prop(L, ctx, *h);
        else return luaL_error(L, "dnp.mark: on an entity, a prop or a vec3, not %s", luaL_typename(L, 1));
        if (e < 0) { lua_pushnil(L); return 1; }
        m.anchor = proto::MARK_ON_ENTITY;
        m.handle = proto::Handle::make(static_cast<u32>(e) + 1, ctx.world->generation(static_cast<u32>(e)));
    }
    size_t tn = 0;
    const char* tp = luaL_checklstring(L, 2, &tn);
    if (!proto::valid_template_name(tp, tn)) {
        return luaL_error(L, "dnp.mark: template '%s' must be 1..%d of [a-z0-9_]", tp,
                          static_cast<int>(proto::UI_MAX_TEMPLATE));
    }
    m.template_len = static_cast<u8>(tn);
    std::memcpy(m.template_name, tp, tn);
    mark_values(L, 3, m, "dnp.mark");
    i32 to = -1;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_getfield(L, 4, "to");
        if (!lua_isnil(L, -1)) {
            to = resolve(L, ctx, check_handle(L, -1));
            if (to < 0) { lua_pop(L, 1); lua_pushnil(L); return 1; }
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "height");
        if (!lua_isnil(L, -1)) m.height = check_fx(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "range");
        if (!lua_isnil(L, -1)) m.range = check_fx(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 4, "debug");
        if (lua_toboolean(L, -1)) m.flags |= proto::MARK_FLAG_DEBUG;
        lua_pop(L, 1);
        lua_getfield(L, 4, "through_walls");
        if (lua_toboolean(L, -1)) m.flags |= proto::MARK_FLAG_THROUGH;
        lua_pop(L, 1);
    }
    // Debug markers cost a shipped game nothing, and tell its players nothing.
    if ((m.flags & proto::MARK_FLAG_DEBUG) && !ctx.debug_overlay) { lua_pushnil(L); return 1; }
    const u32 id = ctx.ui ? ctx.ui->mark(ctx.level, m, to) : 0;
    if (id == 0) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, id);
    return 1;
}

// dnp.mark_set(id, values) -> true | false
//
// Change a marker's values: the names given replace theirs, `false` removes
// one, the rest stay. Sent again to whoever sees it. False for a marker that is
// not (any more).
int l_dnp_mark_set(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 id = static_cast<u32>(luaL_checknumber(L, 1));
    proto::MarkSet m;
    if (ctx.ui == nullptr || !ctx.ui->get_mark(ctx.level, id, m)) { lua_pushboolean(L, 0); return 1; }
    mark_values(L, 2, m, "dnp.mark_set");
    lua_pushboolean(L, ctx.ui->remark(ctx.level, m) ? 1 : 0);
    return 1;
}

// dnp.unmark(id): take a marker down, for everyone who sees it.
int l_dnp_unmark(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const u32 id = static_cast<u32>(luaL_checknumber(L, 1));
    if (ctx.ui) ctx.ui->unmark(ctx.level, id);
    return 0;
}

// dnp.debug_overlay() -> bool: whether the module launches with the debug
// overlay (module.txt `debug`), which is when its debug markers are sent.
int l_dnp_debug_overlay(lua_State* L) {
    lua_pushboolean(L, ctx_of(L).debug_overlay ? 1 : 0);
    return 1;
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
    v.pos = ctx.to_world(check_vec3(L, 2));
    v.dir = lua_isnoneornil(L, 3) ? brad(0u) : check_brad(L, 3);
    v.param = lua_isnoneornil(L, 4) ? 0u : static_cast<u32>(luaL_checknumber(L, 4));
    // An optional entity whose own client already drew this one, from its
    // predicted weapon (docs/WEAPON_PREDICTION.md): everybody else is sent it,
    // that client is not, so its shot is not drawn twice.
    if (!lua_isnoneornil(L, 5)) {
        const i32 who = resolve(L, ctx, check_handle(L, 5));
        if (who >= 0) v.skip_for = static_cast<u32>(who) + 1;
    }
    lua_pushboolean(L, ctx.world->push_vfx(v) ? 1 : 0);
    return 1;
}

// dnp.sound(name, where [, opts]) -> bool
//
// A sound, heard by everybody near `where` (docs/protocol.md §5.7). `where` is
// a position, or an entity or prop -- then the sound comes from it and, unless
// `opts.follow` is false, moves with it. `opts.predicted` says the entity's own
// client has already played it (its own jump, its own shot), so that client
// drops this copy rather than hearing it twice, a round trip apart.
//
// The name is sent as its hash and resolved by the client against the bank it
// loaded; a name the bank does not have is silence there, said once.
int l_dnp_sound(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    PendingSound s;
    s.sound = proto::sound_hash(name, n);
    if (proto::Handle* h = to_handle(L, 2)) {
        const i32 a = resolve(L, ctx, *h);
        if (a < 0) { lua_pushboolean(L, 0); return 1; }
        s.pos = ctx.world->agent(static_cast<u32>(a)).pos;
        s.source = h->raw;
        s.flags = proto::SOUND_FOLLOW;
    } else if (proto::Handle* p = to_prop(L, 2)) {
        const i32 e = resolve_prop(L, ctx, *p);
        if (e < 0) { lua_pushboolean(L, 0); return 1; }
        s.pos = ctx.world->prop(static_cast<u32>(e)).pos;
        s.source = p->raw;
        s.flags = proto::SOUND_FOLLOW;
    } else {
        s.pos = ctx.to_world(check_vec3(L, 2));
    }
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "follow");
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) s.flags &= static_cast<u8>(~proto::SOUND_FOLLOW);
        lua_pop(L, 1);
        lua_getfield(L, 3, "predicted");
        if (lua_toboolean(L, -1) && s.source != 0) s.flags |= proto::SOUND_PREDICTED;
        lua_pop(L, 1);
    }
    if (ctx.sounds == nullptr) { lua_pushboolean(L, 0); return 1; }
    if (ctx.sounds->size() >= MAX_PENDING_SOUNDS) {
        ++ctx.sounds_dropped;
        lua_pushboolean(L, 0);
        return 1;
    }
    ctx.sounds->push_back(s);
    lua_pushboolean(L, 1);
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

    // A BODY IS A COLUMN, so both ends of it are read: the looker uses its
    // middle, which is its eye, and the target is asked about from its feet to
    // the top of its head.
    const auto body_of = [&](int idx, fx3& pos, fx& height, i32& sector) {
        if (proto::Handle* h = to_handle(L, idx)) {
            const i32 a = resolve(L, ctx, *h);
            if (a < 0) return false;
            const sim::AgentState& e = ctx.world->agent(static_cast<u32>(a));
            pos = e.pos;
            height = ctx.world->params(static_cast<u32>(a)).height;
            sector = e.mv.sector;
            return true;
        }
        if (to_prop(L, idx)) {
            const i32 e = resolve_prop(L, ctx, check_prop_handle(L, idx));
            if (e < 0) return false;
            const sim::PropState& p = ctx.world->prop(static_cast<u32>(e));
            pos = p.pos;
            height = p.height;
            sector = geom::find_sector_at(*m, fx2{p.pos.x, p.pos.y}, p.pos.z, p.height);
            return true;
        }
        return false;
    };

    fx3 from_pos{}, to_pos{};
    fx from_h{}, to_h{};
    i32 from_sector = -1, to_sector = -1;
    if (!body_of(1, from_pos, from_h, from_sector) ||
        !body_of(2, to_pos, to_h, to_sector)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // At EYE HEIGHT, and SLOPED. Sight is not a floorplan question: a two-sided
    // line with a balcony behind it, or a shut door, is a wall to look through
    // even though it names a neighbouring sector -- and a flat cast at the
    // looker's own height is a different line from the one to the target the
    // moment the two are at different heights, which on a Doom map they nearly
    // always are.
    //
    // The geometry of it lives in core, in physics::can_see, because the native
    // mob loop asks the identical question and a transcription of it is a
    // second answer waiting to drift from this one.
    const fx3 eye{from_pos.x, from_pos.y, from_pos.z + from_h / 2};
    lua_pushboolean(L, physics::can_see(*m, from_sector, eye,
                                        fx2{to_pos.x, to_pos.y},
                                        to_pos.z, to_pos.z + to_h) ? 1 : 0);
    return 1;
}

// dnp.can_reach(a, b) -> bool
//
// IS THERE A WALL BETWEEN TWO BODIES for a blow at arm's length -- a monster's
// `melee`, which measures a distance and so reached through a wall, or a shut
// door, to whoever stood on the far side. physics::reach_clear: stopped by a
// wall or a shut door, not by a ledge (the blow's own height test judges that)
// nor by a ray that cannot be resolved. Agents only: a blow is struck by a body
// and at a body.
int l_dnp_can_reach(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    if (m == nullptr) { lua_pushboolean(L, 1); return 1; }
    proto::Handle* ha = to_handle(L, 1);
    proto::Handle* hb = to_handle(L, 2);
    if (ha == nullptr || hb == nullptr) { lua_pushboolean(L, 0); return 1; }
    const i32 a = resolve(L, ctx, *ha);
    const i32 b = resolve(L, ctx, *hb);
    if (a < 0 || b < 0) { lua_pushboolean(L, 0); return 1; }
    const sim::AgentState& sa = ctx.world->agent(static_cast<u32>(a));
    const sim::AgentState& sb = ctx.world->agent(static_cast<u32>(b));
    const fx mid = sa.pos.z + ctx.world->params(static_cast<u32>(a)).height / 2;
    lua_pushboolean(L, physics::reach_clear(*m, sa.mv.sector, fx2{sa.pos.x, sa.pos.y},
                                            fx2{sb.pos.x, sb.pos.y}, mid) ? 1 : 0);
    return 1;
}

// dnp.length(v) -> fx
int l_dnp_length(lua_State* L) {
    const fx3 v = check_vec3(L, 1);
    push_fx(L, length(fx2{v.x, v.y}));
    return 1;
}

// dnp.place(pos, radius, height, [ignore]) -> pos, sector | nil
//
// WHERE A BODY OF THIS SIZE COULD STAND at `pos`: on the floor under it, with
// the headroom it needs, no wall within its radius and no other solid body
// overlapping it -- or nil. For a teleport (mobs.lua's `teleport`), which must
// not put a monster inside a wall, under a ceiling it does not fit beneath, or
// inside the player it meant to land behind. `pos.z` decides which floor where
// footprints overlap (a bridge over a path), as find_sector_at does; the answer
// is ON that floor. `ignore` is a body not to count (the one being moved).
//
// Whether it could then WALK to somewhere is a separate question, and
// dnp.find_path's: a spot can be perfectly good to stand on and sealed off.
int l_dnp_place(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const geom::MapData* m = ctx.world->map();
    const fx3 p = ctx.to_world(check_vec3(L, 1));
    const fx radius = check_fx(L, 2);
    const fx height = check_fx(L, 3);
    const i32 ignore = lua_isnoneornil(L, 4) ? -1 : entity_agent(L, 4);
    if (m == nullptr || radius < fx::zero() || height <= fx::zero()) { lua_pushnil(L); return 1; }
    const fx2 at{p.x, p.y};
    const physics::SurfaceQuery q = physics::query_surface(*m, at, geom::NO_SECTOR, p.z, height);
    if (q.sector < 0 || q.ceil_z - q.floor_z < height) { lua_pushnil(L); return 1; }
    // A wall the body could not pass, within its radius. Only the sector's own
    // sides: they are what bounds it, and a passable one leads somewhere the
    // body also fits.
    const u128 r_sq = static_cast<u128>(static_cast<i128>(radius.raw) * radius.raw);
    const geom::Sector& sec = m->sectors[q.sector];
    for (u32 k = 0; k < sec.side_count; ++k) {
        const u32 si = sec.first_side + k;
        if (physics::side_is_passable(*m, si, static_cast<u32>(q.sector), q.floor_z, height)) continue;
        const geom::Side& sd = m->side(si);
        if (geom::distance_sq_to_segment(m->point(sd.a), m->point(sd.b), at) < r_sq) {
            lua_pushnil(L);
            return 1;
        }
    }
    // Another solid body in the way: plan distance against the two radii, and
    // only where the two columns overlap in height.
    for (u32 i = 0; i < ctx.world->agent_high_water(); ++i) {
        if (static_cast<i32>(i) == ignore) continue;
        const sim::AgentState& a = ctx.world->agent(i);
        if (!a.active || !a.solid) continue;
        const physics::MoveParams& pm = ctx.world->params(i);
        if (a.pos.z >= q.floor_z + height || a.pos.z + pm.height <= q.floor_z) continue;
        const fx dx = a.pos.x - p.x, dy = a.pos.y - p.y;
        const fx reach = radius + pm.radius;
        if (dx * dx + dy * dy < reach * reach) { lua_pushnil(L); return 1; }
    }
    push_vec3(L, ctx.to_local(fx3{p.x, p.y, q.floor_z}));
    lua_pushnumber(L, ctx.sector_to_local(q.sector));
    return 2;
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
    const fx3 from = ctx.to_world(check_vec3(L, 1));
    const fx3 dir3 = check_vec3(L, 2);
    const fx dist = check_fx(L, 3);
    if (m == nullptr || dist <= fx::zero()) {
        lua_pushboolean(L, 0);
        push_vec3(L, ctx.to_local(from));
        return 2;
    }
    const fx2 dir = normalize(fx2{dir3.x, dir3.y});
    // At its height: where footprints nest (a platform over a path), the
    // smallest one is not necessarily the one it is in.
    const i32 sector = geom::find_sector_at(*m, fx2{from.x, from.y}, from.z, fx::zero());
    // At the projectile's own height, so a fireball is stopped by the step it
    // would actually hit rather than by the map's floorplan.
    const physics::RayHit hit =
        physics::raycast(*m, sector, fx2{from.x, from.y}, dir, dist, 0, from.z);
    if (!hit.blocked) {
        lua_pushboolean(L, 0);
        push_vec3(L, ctx.to_local(from));
        return 2;
    }
    lua_pushboolean(L, 1);
    // Stopped a hair short of the surface, so the thing that explodes is
    // visible against the wall rather than inside it.
    const fx back = fx::from_frac(1, 10);
    const fx d = hit.distance > back ? hit.distance - back : fx::zero();
    push_vec3(L, ctx.to_local(fx3{from.x + dir.x * d, from.y + dir.y * d, from.z}));
    return 2;
}

// dnp.sweep_hit(from, dir, distance, radius, [ignore_a], [ignore_b], [slope])
//   -> nil                     nothing in reach
//   -> false, impact, nx, ny, plane
//                              the map stopped it: a wall's normal (nx, ny, fx),
//                              or `plane` -1 floor / +1 ceiling (0 for a wall)
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
//
// `slope`, a missile's climb per unit: given, a step, a ceiling or the floor
// stops it too (World::sweep_entities), and the impact is at the height it met
// them. A rocket fired at your feet bursts there instead of sinking through.
int l_dnp_sweep_hit(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const fx3 from = ctx.to_world(check_vec3(L, 1));
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
    const fx slope = lua_isnoneornil(L, 7) ? physics::RAY_NO_HEIGHT : check_fx(L, 7);
    const sim::World::SweepHit hit =
        ctx.world->sweep_entities(from, dir, dist, radius, ignore_a, ignore_b, -1, slope);

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
    // The height is the hit's: the start's for a flat sweep, and on a sloped
    // one where the line met what stopped it (on the floor, for a floor).
    push_vec3(L, ctx.to_local(fx3{from.x + dir.x * d, from.y + dir.y * d, hit.point.z}));
    if (hit.entity_hit) return 2;
    // What it met, for a missile that bounces off it.
    push_fx(L, hit.normal.x);
    push_fx(L, hit.normal.y);
    lua_pushnumber(L, hit.plane);
    return 5;
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
    // ONE BUDGET, and it lives on the context because the C++ mob loop spends
    // it too -- see WorldContext::take_path_budget.
    if (!ctx.take_path_budget(ctx.world->tick())) {
        lua_pushnil(L);
        lua_pushliteral(L, "path budget");
        return 2;
    }

    // The level's own sector numbers in and out, positions in its own frame.
    const i32 from = ctx.sector_from_local(static_cast<i32>(luaL_checknumber(L, 1)));
    const i32 to   = ctx.sector_from_local(static_cast<i32>(luaL_checknumber(L, 2)));
    if (from < 0 || to < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "no such sector");
        return 2;
    }

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
        lua_pushnumber(L, ctx.sector_to_local(static_cast<i32>(path[i])));
        lua_setfield(L, -2, "sector");
        const fx3 cw = ctx.to_local(fx3{ctx.nav->node(path[i]).centroid.x,
                                        ctx.nav->node(path[i]).centroid.y, fx::zero()});
        const fx2 c{cw.x, cw.y};
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
// dnp.blast_reach(centre, at, radius, falloff) -> nil | true, share | true
//
// Is `at` in the blast, and by what share when it falls off (sim/blast.hpp).
// In the core, not the script, because a predicting client runs the same rule
// for the blast of its own rocket: a rocket jump felt as it happens.
int l_dnp_blast_reach(lua_State* L) {
    const fx3 centre = check_vec3(L, 1);
    const fx3 at = check_vec3(L, 2);
    const sim::BlastReach r = sim::blast_reach(centre, at, check_fx(L, 3), lua_toboolean(L, 4) != 0);
    if (!r.reached) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, 1);
    if (!r.scaled) return 1;
    push_fx(L, r.share);
    return 2;
}

// dnp.knock_impulse(origin, at, knockback, lift, k) -> vec3
//
// The velocity a shove gives a body at `at` from `origin` (sim/blast.hpp):
// outward in the plane by knockback * k, up by lift * k.
int l_dnp_knock_impulse(lua_State* L) {
    push_vec3(L, sim::knock_impulse(check_vec3(L, 1), check_vec3(L, 2), check_fx(L, 3),
                                    check_fx(L, 4), check_fx(L, 5)));
    return 1;
}

int l_dnp_atan2(lua_State* L) {
    lua_pushnumber(L, fx_atan2(check_fx(L, 1), check_fx(L, 2)).raw);
    return 1;
}

// dnp.dir(brad) -> a unit vec3 in the XY plane
int l_dnp_dir(lua_State* L) {
    const fx2 d = fx_dir(check_brad(L, 1));
    push_vec3(L, fx3{d.x, d.y, fx::zero()});
    return 1;
}

int l_dnp_stale_ops(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(ctx_of(L).stats.stale_handle_ops));
    return 1;
}

// --- persistence (docs/SAVING.md) -----------------------------------------
//
// THE ENCODING, one value at a time, as text:
//
//   T F                 true, false
//   i<integer>;         a number with no fraction (the common case: counts, ids)
//   n<%.17g>;           any other number, exactly as a double reads back
//   s<len>:<bytes>      a string
//   x<raw>;             an fx, by its raw i64: exact, and the reason for this format
//   v<x>;<y>;<z>;       a vec3, the same
//   {<key><value>...}   a table, keys in sorted order of their encoding
//
// Sorted, so the same state always encodes to the same bytes: two saves of an
// unchanged level compare equal, and a test can say what it expects.

namespace {

constexpr int MAX_SAVE_DEPTH = 32;

// `handles`: entity and prop handles are allowed too, by their raw value --
// for a value copied between two VMs looking at the same world (CrossSink),
// never for one saved, where a handle would mean nothing when read back.
bool encode_at(lua_State* L, int idx, std::string& out, std::string& err,
               int depth, const std::string& path, bool handles = false) {
    // Lua 5.1 (LuaJIT) has no lua_absindex.
    if (idx < 0 && idx > LUA_REGISTRYINDEX) idx = lua_gettop(L) + idx + 1;
    // ROOM ON THE LUA STACK for this level's key, value and key copy. Lua
    // promises 20 free slots and no more; at three a level, a table nested
    // seven deep wrote past the stack's end -- unseen under LuaJIT, whose stack
    // starts bigger, and a corrupted heap under PUC Lua 5.1 (the web build).
    if (!lua_checkstack(L, 4)) { err = "out of Lua stack at " + path; return false; }
    switch (lua_type(L, idx)) {
        // Only ever at the top: a table holds no nils. A function returning
        // `nil, x` across two VMs (CrossSink) is the case it is for.
        case LUA_TNIL: out += 'N'; return true;
        case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? 'T' : 'F'; return true;
        case LUA_TNUMBER: {
            const double d = lua_tonumber(L, idx);
            char buf[40];
            if (d == static_cast<double>(static_cast<long long>(d)) && d > -9.0e15 && d < 9.0e15) {
                std::snprintf(buf, sizeof(buf), "i%lld;", static_cast<long long>(d));
            } else {
                std::snprintf(buf, sizeof(buf), "n%.17g;", d);
            }
            out += buf;
            return true;
        }
        case LUA_TSTRING: {
            size_t n = 0;
            const char* s = lua_tolstring(L, idx, &n);
            out += 's';
            out += std::to_string(n);
            out += ':';
            out.append(s, n);
            return true;
        }
        case LUA_TUSERDATA: {
            if (is_fx(L, idx)) {
                out += 'x';
                out += std::to_string(static_cast<long long>(check_fx(L, idx).raw));
                out += ';';
                return true;
            }
            if (is_vec3(L, idx)) {
                const fx3 v = check_vec3(L, idx);
                out += 'v';
                for (const fx c : {v.x, v.y, v.z}) {
                    out += std::to_string(static_cast<long long>(c.raw));
                    out += ';';
                }
                return true;
            }
            if (handles) {
                const proto::Handle* h = to_handle(L, idx);
                const bool prop = h == nullptr;
                if (prop) h = to_prop(L, idx);
                if (h != nullptr) {
                    out += prop ? 'P' : 'E';
                    out += std::to_string(h->raw);
                    out += ';';
                    return true;
                }
            }
            err = "cannot save a handle or other userdata at " + path +
                  " -- save what it stands for (a kind, a position) instead";
            return false;
        }
        case LUA_TTABLE: {
            if (depth >= MAX_SAVE_DEPTH) { err = "tables nested too deep at " + path; return false; }
            struct Pair { std::string k, v; };
            std::vector<Pair> pairs;
            lua_pushnil(L);
            while (lua_next(L, idx) != 0) {
                // key at -2, value at -1
                Pair p;
                const int kt = lua_type(L, -2);
                if (kt != LUA_TNUMBER && kt != LUA_TSTRING && kt != LUA_TBOOLEAN) {
                    lua_pop(L, 2);
                    err = "a table key at " + path + " is not a number, string or boolean";
                    return false;
                }
                // Encode the key from a copy: lua_tolstring on a number key in
                // place would confuse lua_next.
                lua_pushvalue(L, -2);
                std::string kname;
                if (kt == LUA_TSTRING) kname = lua_tostring(L, -1);
                else if (kt == LUA_TNUMBER) { char b[32]; std::snprintf(b, sizeof(b), "%.17g", lua_tonumber(L, -1)); kname = b; }
                else kname = lua_toboolean(L, -1) ? "true" : "false";
                const bool kok = encode_at(L, -1, p.k, err, depth + 1, path, handles);
                lua_pop(L, 1);
                if (!kok) { lua_pop(L, 2); return false; }
                if (!encode_at(L, -1, p.v, err, depth + 1, path + "." + kname, handles)) { lua_pop(L, 2); return false; }
                lua_pop(L, 1);
                pairs.push_back(std::move(p));
            }
            std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.k < b.k; });
            out += '{';
            for (const Pair& p : pairs) { out += p.k; out += p.v; }
            out += '}';
            return true;
        }
        default:
            err = std::string("cannot save a ") + luaL_typename(L, idx) + " at " + path;
            return false;
    }
}

struct Reader {
    const std::string& s;
    size_t i = 0;
    bool at_end() const { return i >= s.size(); }
};

bool read_i64(Reader& r, long long& v) {
    size_t j = r.i;
    if (j < r.s.size() && (r.s[j] == '-' || r.s[j] == '+')) ++j;
    const size_t digits = j;
    while (j < r.s.size() && r.s[j] >= '0' && r.s[j] <= '9') ++j;
    if (j == digits || j >= r.s.size() || r.s[j] != ';') return false;
    v = std::strtoll(r.s.c_str() + r.i, nullptr, 10);
    r.i = j + 1;
    return true;
}

bool decode_at(lua_State* L, Reader& r, std::string& err, int depth, bool handles = false) {
    // As encode_at: a table, a key and a value a level.
    if (!lua_checkstack(L, 4)) { err = "out of Lua stack"; return false; }
    if (r.at_end()) { err = "save data ends early"; return false; }
    if (depth >= MAX_SAVE_DEPTH) { err = "save data nested too deep"; return false; }
    const char c = r.s[r.i++];
    switch (c) {
        case 'N': lua_pushnil(L); return true;
        case 'T': lua_pushboolean(L, 1); return true;
        case 'F': lua_pushboolean(L, 0); return true;
        case 'i': {
            long long v = 0;
            if (!read_i64(r, v)) { err = "bad integer in save data"; return false; }
            lua_pushnumber(L, static_cast<lua_Number>(v));
            return true;
        }
        case 'n': {
            const size_t semi = r.s.find(';', r.i);
            if (semi == std::string::npos) { err = "bad number in save data"; return false; }
            lua_pushnumber(L, std::strtod(r.s.substr(r.i, semi - r.i).c_str(), nullptr));
            r.i = semi + 1;
            return true;
        }
        case 's': {
            const size_t colon = r.s.find(':', r.i);
            if (colon == std::string::npos) { err = "bad string in save data"; return false; }
            const size_t n = static_cast<size_t>(std::strtoull(r.s.c_str() + r.i, nullptr, 10));
            if (colon + 1 + n > r.s.size()) { err = "string runs past the end of the save data"; return false; }
            lua_pushlstring(L, r.s.data() + colon + 1, n);
            r.i = colon + 1 + n;
            return true;
        }
        case 'x': {
            long long v = 0;
            if (!read_i64(r, v)) { err = "bad fx in save data"; return false; }
            push_fx(L, fx::from_raw(v));
            return true;
        }
        case 'v': {
            long long a = 0, b = 0, d = 0;
            if (!read_i64(r, a) || !read_i64(r, b) || !read_i64(r, d)) { err = "bad vec3 in save data"; return false; }
            push_vec3(L, fx3{fx::from_raw(a), fx::from_raw(b), fx::from_raw(d)});
            return true;
        }
        case 'E':
        case 'P': {
            long long v = 0;
            if (!handles || !read_i64(r, v)) { err = "a handle where none may be"; return false; }
            *static_cast<proto::Handle*>(lua_newuserdata(L, sizeof(proto::Handle))) =
                proto::Handle{static_cast<u32>(v)};
            luaL_getmetatable(L, c == 'E' ? ENTITY_MT : PROP_MT);
            lua_setmetatable(L, -2);
            return true;
        }
        case '{': {
            lua_newtable(L);
            while (!r.at_end() && r.s[r.i] != '}') {
                if (!decode_at(L, r, err, depth + 1, handles)) { lua_pop(L, 1); return false; }
                if (!decode_at(L, r, err, depth + 1, handles)) { lua_pop(L, 2); return false; }
                lua_rawset(L, -3);
            }
            if (r.at_end()) { lua_pop(L, 1); err = "a table in the save data is not closed"; return false; }
            ++r.i;
            return true;
        }
        default:
            err = std::string("unexpected '") + c + "' in save data";
            return false;
    }
}

const char* const PERSIST_KEY = "dnp_persist";
const char* const PERSIST_PLAYER_KEY = "dnp_persist_player";

// dnp.persist(name, save, load) and dnp.persist_player(...): register a part.
int register_part(lua_State* L, const char* key) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, key);
    }
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, 2); lua_setfield(L, -2, "save");
    lua_pushvalue(L, 3); lua_setfield(L, -2, "load");
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

// The registered part names, sorted: parts are saved and loaded in one order.
std::vector<std::string> part_names(lua_State* L, const char* key) {
    std::vector<std::string> names;
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) == LUA_TSTRING) names.emplace_back(lua_tostring(L, -1));
        }
    }
    lua_pop(L, 1);
    std::sort(names.begin(), names.end());
    return names;
}

// Push parts[name][field], or return false.
bool push_part_fn(lua_State* L, const char* key, const std::string& name, const char* field) {
    lua_getfield(L, LUA_REGISTRYINDEX, key);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    lua_getfield(L, -1, name.c_str());
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return false; }
    lua_getfield(L, -1, field);
    lua_remove(L, -2);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return false; }
    return true;
}

// Save every part: run each save with `nargs` arguments already pushed (copied
// per call), collect { [name] = value }, encode.
bool capture_parts(lua_State* L, const char* key, int nargs, std::string& out, std::string& err) {
    const int base = lua_gettop(L) - nargs;
    lua_newtable(L);
    const int tab = lua_gettop(L);
    std::string problems;
    for (const std::string& name : part_names(L, key)) {
        if (!push_part_fn(L, key, name, "save")) continue;
        for (int a = 1; a <= nargs; ++a) lua_pushvalue(L, base + a);
        if (lua_pcall(L, nargs, 1, 0) != 0) {
            problems += name + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error") + "; ";
            lua_pop(L, 1);
            continue;
        }
        if (lua_isnil(L, -1)) { lua_pop(L, 1); continue; }
        // Check this part encodes before keeping it, so one bad part is named
        // and left out instead of losing every part with it.
        std::string probe, perr;
        if (!encode_at(L, -1, probe, perr, 0, name)) {
            problems += name + ": " + perr + "; ";
            lua_pop(L, 1);
            continue;
        }
        lua_setfield(L, tab, name.c_str());
    }
    const bool ok = encode_at(L, tab, out, err, 0, "parts");
    lua_settop(L, base);
    if (!problems.empty()) err = problems;
    return ok;
}

bool restore_parts(lua_State* L, const char* key, int nargs, const std::string& in, std::string& err) {
    const int base = lua_gettop(L) - nargs;
    Reader r{in};
    if (!decode_at(L, r, err, 0)) { lua_settop(L, base); return false; }
    if (!lua_istable(L, -1)) { lua_settop(L, base); err = "save data is not a table of parts"; return false; }
    const int tab = lua_gettop(L);
    std::string problems;
    std::vector<std::string> saved;
    lua_pushnil(L);
    while (lua_next(L, tab) != 0) {
        lua_pop(L, 1);
        if (lua_type(L, -1) == LUA_TSTRING) saved.emplace_back(lua_tostring(L, -1));
    }
    std::sort(saved.begin(), saved.end());
    for (const std::string& name : saved) {
        if (!push_part_fn(L, key, name, "load")) { problems += name + ": nothing loads it; "; continue; }
        for (int a = 1; a <= nargs; ++a) lua_pushvalue(L, base + a);
        lua_getfield(L, tab, name.c_str());
        if (lua_pcall(L, nargs + 1, 0, 0) != 0) {
            problems += name + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error") + "; ";
            lua_pop(L, 1);
        }
    }
    lua_settop(L, base);
    if (!problems.empty()) err = problems;
    return true;
}

}  // namespace

int l_dnp_persist(lua_State* L) { return register_part(L, PERSIST_KEY); }

// --- the store: values a game keeps on disk (docs/GRAPHS.md "Saved on disk") --
//
// Not a save game. A save is the world at a moment and a new game forgets it;
// the store is what outlives both -- a high score, what is unlocked, a setting.
// One file per NAME under <state-dir>/store, in the persist encoding (plain
// data only, keys sorted), written whole and swapped in, so a crash mid-write
// leaves the old file rather than half a new one.
namespace {
constexpr size_t STORE_MAX_BYTES = 64 * 1024;

// A name is a file name, so it is held to letters, digits, '_' and '-': no
// path can be smuggled through it, and it means the same file on every system.
bool store_name_ok(const char* s, size_t n) {
    if (n == 0 || n > 48) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The file for a name, or empty (with a reason pushed as `false, why`).
std::string store_path(lua_State* L, int& pushed) {
    size_t n = 0;
    const char* name = luaL_checklstring(L, 1, &n);
    const WorldContext& ctx = ctx_of(L);
    pushed = 0;
    if (!store_name_ok(name, n)) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "'%s' is not a store name: 1-48 letters, digits, _ or -", name);
        pushed = 2;
        return {};
    }
    if (ctx.state_dir == nullptr || ctx.state_dir->empty()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "this server has nowhere to write (no state directory)");
        pushed = 2;
        return {};
    }
    return *ctx.state_dir + "/store/" + std::string(name, n) + ".store";
}
}  // namespace

// dnp.store_save(name, value) -> true | false, why
int l_dnp_store_save(lua_State* L) {
    int pushed = 0;
    const std::string path = store_path(L, pushed);
    if (pushed) return pushed;
    luaL_checkany(L, 2);
    std::string out, err;
    if (!encode_value(L, 2, out, err)) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "cannot be saved: %s", err.c_str());
        return 2;
    }
    if (out.size() > STORE_MAX_BYTES) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "too big to keep (%d bytes; the most is %d)", (int)out.size(), (int)STORE_MAX_BYTES);
        return 2;
    }
    const std::string dir = *ctx_of(L).state_dir + "/store";
    std::string at;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') { if (!at.empty()) make_dir(at.c_str()); }
        if (i < dir.size()) at += dir[i];
    }
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    bool ok = f != nullptr;
    if (f) { ok = std::fwrite(out.data(), 1, out.size(), f) == out.size(); std::fclose(f); }
    if (ok) ok = replace_file(tmp.c_str(), path.c_str());
    if (!ok) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "could not write %s", path.c_str());
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// dnp.store_load(name) -> value | nil[, why]. Nil with no reason: never saved.
int l_dnp_store_load(lua_State* L) {
    int pushed = 0;
    const std::string path = store_path(L, pushed);
    if (pushed) { lua_pushnil(L); lua_replace(L, -3); return 2; }
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { lua_pushnil(L); return 1; }
    std::string in;
    char buf[4096];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0 && in.size() <= STORE_MAX_BYTES) in.append(buf, got);
    std::fclose(f);
    std::string err;
    if (in.size() > STORE_MAX_BYTES || !decode_value(L, in, err)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is damaged: %s", path.c_str(), err.empty() ? "too big" : err.c_str());
        return 2;
    }
    return 1;
}

// dnp.store_delete(name) -> true if there was one
int l_dnp_store_delete(lua_State* L) {
    int pushed = 0;
    const std::string path = store_path(L, pushed);
    if (pushed) return pushed;
    lua_pushboolean(L, std::remove(path.c_str()) == 0 ? 1 : 0);
    return 1;
}
int l_dnp_persist_player(lua_State* L) { return register_part(L, PERSIST_PLAYER_KEY); }
int l_dnp_restoring(lua_State* L) {
    lua_pushboolean(L, ctx_of(L).restoring ? 1 : 0);
    return 1;
}

// ------------------------------------------------- the game VM and the levels'
//
// See CrossSink in world_binding.hpp for what this is for.

const char* const EXPORTS_KEY = "dnp_exports";

// dnp.export(name, fn): what another VM may call by `name`.
int l_dnp_export(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, EXPORTS_KEY);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, EXPORTS_KEY);
    }
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return 0;
}

// Push exports[name], or push nothing and return false.
bool push_export(lua_State* T, const char* name) {
    lua_getfield(T, LUA_REGISTRYINDEX, EXPORTS_KEY);
    if (!lua_istable(T, -1)) { lua_pop(T, 1); return false; }
    lua_getfield(T, -1, name);
    lua_remove(T, -2);
    if (!lua_isfunction(T, -1)) { lua_pop(T, 1); return false; }
    return true;
}

// Copy `n` values, from index `first` of F, onto T.
bool transfer(lua_State* F, int first, int n, lua_State* T, std::string& err) {
    for (int i = 0; i < n; ++i) {
        std::string enc;
        if (!encode_at(F, first + i, enc, err, 0, "value " + std::to_string(i + 1), true)) {
            lua_pop(T, i);
            return false;
        }
        Reader r{enc};
        if (!decode_at(T, r, err, 0, true)) { lua_pop(T, i); return false; }
    }
    return true;
}

// Call T's export `name` with L's values [first, first + nargs) and leave its
// results on L. The number of results, or -1 when there is no such export and
// -2 when the call failed (said on stderr, by name: a call that quietly did
// nothing is a weapon that quietly does not fire).
int call_across(lua_State* L, lua_State* T, const char* name, int first, int nargs) {
    if (T == L) {
        // The same VM: no copy, just the call.
        const int base = lua_gettop(L);
        if (!push_export(L, name)) return -1;
        for (int i = 0; i < nargs; ++i) lua_pushvalue(L, first + i);
        if (lua_pcall(L, nargs, LUA_MULTRET, 0) != 0) {
            std::fprintf(stderr, "[lua] %s: %s\n", name, lua_tostring(L, -1));
            lua_settop(L, base);
            return -2;
        }
        return lua_gettop(L) - base;
    }
    const int base = lua_gettop(T);
    if (!push_export(T, name)) return -1;
    std::string err;
    if (!transfer(L, first, nargs, T, err)) {
        std::fprintf(stderr, "[lua] %s: cannot pass the arguments: %s\n", name, err.c_str());
        lua_settop(T, base);
        return -2;
    }
    if (lua_pcall(T, nargs, LUA_MULTRET, 0) != 0) {
        std::fprintf(stderr, "[lua] %s: %s\n", name, lua_tostring(T, -1));
        lua_settop(T, base);
        return -2;
    }
    const int nret = lua_gettop(T) - base;
    const int top = lua_gettop(L);
    if (!transfer(T, base + 1, nret, L, err)) {
        std::fprintf(stderr, "[lua] %s: cannot pass the results back: %s\n", name, err.c_str());
        lua_settop(T, base);
        lua_settop(L, top);
        return -2;
    }
    lua_settop(T, base);
    return nret;
}

// dnp.call_game(name, ...) -> what the game's export returned, or nothing
// when there is no game VM or it exports no such name.
int l_dnp_call_game(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const char* name = luaL_checkstring(L, 1);
    if (ctx.cross == nullptr) return 0;
    const int nargs = lua_gettop(L) - 1;
    lua_State* T = ctx.cross->enter_game(ctx);
    if (T == nullptr) return 0;
    const int n = call_across(L, T, name, 2, nargs);
    ctx.cross->leave(T);
    return n < 0 ? 0 : n;
}

// dnp.game_has(name) -> whether the game VM exports `name`. So a level script
// can tell "the game handles this" from "nobody does" before it decides.
int l_dnp_game_has(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const char* name = luaL_checkstring(L, 1);
    bool has = false;
    if (ctx.cross != nullptr) {
        if (lua_State* T = ctx.cross->enter_game(ctx)) {
            has = push_export(T, name);
            if (has) lua_pop(T, 1);
            ctx.cross->leave(T);
        }
    }
    lua_pushboolean(L, has ? 1 : 0);
    return 1;
}

// dnp.call_level(e, name, ...) -> what the export of the level the player `e`
// is in returned, or nothing. The export is called with (e, ...): the call is
// about that player, and it is the one thing the level must not have to guess.
int l_dnp_call_level(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const i32 a = resolve(L, ctx, check_handle(L, 1));
    const std::string name = luaL_checkstring(L, 2);
    if (a < 0 || ctx.cross == nullptr) return 0;
    lua_remove(L, 2);
    const int nargs = lua_gettop(L);
    lua_State* T = ctx.cross->enter_level_of(ctx, static_cast<u32>(a));
    if (T == nullptr) return 0;
    const int n = call_across(L, T, name.c_str(), 1, nargs);
    ctx.cross->leave(T);
    return n < 0 ? 0 : n;
}

// dnp.call_owner(e, name, ...) -> what the export of the level that spawned
// `e` returned, or nothing -- when `e` is nobody's, or the caller's own. For a
// region, where a shot fired in one level can reach a monster another level
// keeps the health of. Called with (e, ...).
int l_dnp_call_owner(lua_State* L) {
    WorldContext& ctx = ctx_of(L);
    const std::string name = luaL_checkstring(L, 2);
    proto::Handle h{};
    if (proto::Handle* a = to_handle(L, 1)) h = *a;
    else if (proto::Handle* p = to_prop(L, 1)) h = *p;
    else return luaL_error(L, "call_owner: expected an entity or a prop");
    if (ctx.cross == nullptr || ctx.owner == nullptr || !h.valid()) return 0;
    const u32 entity = h.index() - 1;
    if (entity >= sim::MAX_ENTITIES || ctx.world->generation(entity) != h.generation()) return 0;
    const u32 owner = ctx.owner[entity];
    if (owner == 0xFFFFFFFFu || owner == ctx.level) return 0;
    lua_remove(L, 2);
    const int nargs = lua_gettop(L);
    lua_State* T = ctx.cross->enter_level(ctx, owner);
    if (T == nullptr) return 0;
    const int n = call_across(L, T, name.c_str(), 1, nargs);
    ctx.cross->leave(T);
    return n < 0 ? 0 : n;
}

// dnp.controls() -> "fps" or "platformer": the module's control scheme.
int l_dnp_controls(lua_State* L) {
    lua_pushstring(L, ctx_of(L).controls == 1 ? "fps" : "platformer");
    return 1;
}

// dnp.is_game() -> whether this script runs in the game VM.
int l_dnp_is_game(lua_State* L) {
    lua_pushboolean(L, ctx_of(L).game ? 1 : 0);
    return 1;
}

const luaL_Reg DNP_FUNCS[] = {
    {"persist", l_dnp_persist},
    {"store_save", l_dnp_store_save},
    {"store_load", l_dnp_store_load},
    {"store_delete", l_dnp_store_delete},
    {"persist_player", l_dnp_persist_player},
    {"restoring", l_dnp_restoring},
    {"export", l_dnp_export},
    {"call_game", l_dnp_call_game},
    {"spawn_point", l_dnp_spawn_point},
    {"game_has", l_dnp_game_has},
    {"call_level", l_dnp_call_level},
    {"call_owner", l_dnp_call_owner},
    {"is_game", l_dnp_is_game},
    {"controls", l_dnp_controls},
    {"tick", l_dnp_tick},
    {"entity", l_dnp_entity},
    {"entities", l_dnp_entities},
    {"players", l_dnp_players},
    {"player_count", l_dnp_player_count},
    {"sectors", l_dnp_sectors},
    {"sector", l_dnp_sector},
    {"spawn_prop", l_dnp_spawn_prop},
    {"prop_kind", l_dnp_prop_kind},
    {"spawn_enemy", l_dnp_spawn_enemy},
    {"hitscan", l_dnp_hitscan},
    {"can_see", l_dnp_can_see},
    {"can_reach", l_dnp_can_reach},
    {"length", l_dnp_length},
    {"sweep", l_dnp_sweep},
    {"sweep_hit", l_dnp_sweep_hit},
    {"find_path", l_dnp_find_path},
    {"place", l_dnp_place},
    {"mark", l_dnp_mark},
    {"mark_set", l_dnp_mark_set},
    {"unmark", l_dnp_unmark},
    {"debug_overlay", l_dnp_debug_overlay},
    {"atan2", l_dnp_atan2},
    {"blast_reach", l_dnp_blast_reach},
    {"knock_impulse", l_dnp_knock_impulse},
    {"dir", l_dnp_dir},
    {"vfx", l_dnp_vfx},
    {"sound", l_dnp_sound},
    {"predict_sounds", l_dnp_predict_sounds},
    {"hud", l_dnp_hud},
    {"level", l_dnp_level},
    {"levels", l_dnp_levels},
    {"ui_open", l_dnp_ui_open},
    {"camera", l_dnp_camera},
    {"level_camera", l_dnp_level_camera},
    {"sky", l_dnp_sky},
    {"actions", l_dnp_actions},
    {"set_sector_texture", l_dnp_set_sector_texture},
    {"set_sector_light", l_dnp_set_sector_light},
    {"set_sector_effect", l_dnp_set_sector_effect},
    {"path_length", l_dnp_path_length},
    {"screen", l_dnp_screen},
    {"side_texture", l_dnp_side_texture},
    {"set_side_texture", l_dnp_set_side_texture},
    {"music", l_dnp_music},
    {"ui_set", l_dnp_ui_set},
    {"ui_show", l_dnp_ui_show},
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

WorldContext& context_of(lua_State* L) { return ctx_of(L); }

i32 entity_agent(lua_State* L, int idx) {
    WorldContext& ctx = ctx_of(L);
    return resolve(L, ctx, check_handle(L, idx));
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

    // The mob runtime's own bindings, onto the same table. Separate file
    // because it is a separate subsystem -- this one is about entities and the
    // world, that one is about a behaviour graph -- and separate compilation
    // unit so the mob work does not rebuild this.
    mobs::open_mobs(L);
    // And the weapon machine's (docs/WEAPON_PREDICTION.md), likewise.
    open_weapons(L);

    lua_setglobal(L, "dnp");
}

}  // namespace dnp::server::lua

// --- persistence, the C++ side (see world_binding.hpp) -------------------------
namespace dnp::server::lua {

bool encode_value(lua_State* L, int idx, std::string& out, std::string& err) {
    return encode_at(L, idx, out, err, 0, "value");
}

bool decode_value(lua_State* L, const std::string& in, std::string& err) {
    Reader r{in};
    if (!decode_at(L, r, err, 0)) return false;
    if (!r.at_end()) { lua_pop(L, 1); err = "trailing bytes after the saved value"; return false; }
    return true;
}

bool persist_capture(lua_State* L, std::string& out, std::string& err) {
    return capture_parts(L, PERSIST_KEY, 0, out, err);
}

bool persist_restore(lua_State* L, const std::string& in, std::string& err) {
    return restore_parts(L, PERSIST_KEY, 0, in, err);
}

bool persist_player_capture(lua_State* L, WorldContext& ctx, u32 agent,
                            std::string& out, std::string& err) {
    push_entity(L, ctx, agent);
    return capture_parts(L, PERSIST_PLAYER_KEY, 1, out, err);
}

bool persist_player_restore(lua_State* L, WorldContext& ctx, u32 agent,
                            const std::string& in, std::string& err) {
    push_entity(L, ctx, agent);
    return restore_parts(L, PERSIST_PLAYER_KEY, 1, in, err);
}

}  // namespace dnp::server::lua
