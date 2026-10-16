//
// WHAT THE MONSTER EDITOR NEEDS OF THE ENGINE (docs/ENGINE_NEEDS.md), proven
// against the real bindings and a real World rather than read off the code.
//
// Each entry in that document is either something the engine did not do or
// something it was believed to do. This file is where both kinds are settled:
// a new action is shown working with a control that would fail without it,
// and a belief is turned into an assertion that says which way it went.
//
// The same venue as test_lua_shootrange and for the same reason: the fake world
// in tools/mobcheck has been more generous than the engine before, and every
// question here is about what the engine actually does.
//
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "dnp/sim/world.hpp"
#include "dnp/geom/mapstore.hpp"
#include "dnp/nav/navgraph.hpp"
#include "lua/vm.hpp"
#include "lua/world_binding.hpp"
#include "lua/commands.hpp"
#include "lua/effects.hpp"
#include "snapshot_history.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

using namespace dnp;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static fx F(i64 n) { return fx::from_int(n); }

// One body, a zombieman's: 20 by 56 Doom units, 48 to the engine's one.
struct Props final : public server::lua::PropCatalogueView {
    static constexpr fx du(i64 n) { return fx::from_frac(n, 48); }
    // And a glob of something thrown, kind 21.
    u16 kind_by_name(const char* name, size_t len) const override {
        if (len == 9 && std::strncmp(name, "zombieman", 9) == 0) return 20;
        if (len == 4 && std::strncmp(name, "glob", 4) == 0) return 21;
        if (len == 5 && std::strncmp(name, "shard", 5) == 0) return 22;
        return 0;
    }
    fx radius_of(u16 k) const override { return k == 20 ? du(20) : du(8); }
    fx height_of(u16 k) const override { return k == 20 ? du(56) : du(16); }
    fx speed_of(u16 k)  const override { return k == 20 ? du(8 * 35) : fx::zero(); }
    u8 flags_of(u16)    const override { return 0; }
    bool empty()        const override { return false; }
    const char* name_of(u16 k) const override {
        return k == 20 ? "zombieman" : k == 21 ? "glob" : k == 22 ? "shard" : "";
    }
};

// THREE SECTORS, none joined to another:
//   0  the room, 20 across, centred on the origin, ceiling 10;
//   1  a closet east of it, x 11..14: somewhere a body STANDS but cannot walk
//      to from the room, which is what `teleport near` must refuse;
//   2  a crawlspace west of it, x -14..-11, ceiling 1: too low for the body.
struct Map {
    geom::MapStorage store;
    geom::MapData    view{};
    void box(fx x0, fx y0, fx x1, fx y1, fx ceil) {
        const fx2 p[] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        const u32 s = store.add_sector(p, 4, fx::zero(), ceil, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(s, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
    }
    Map() {
        box(-F(10), -F(10), F(10), F(10), F(10));
        box(F(11), -F(8), F(14), F(8), F(10));
        box(-F(14), -F(3), -F(11), F(3), F(1));
        view = store.view();
    }
};

static lua_State* L = nullptr;

static bool run(const std::string& src, const char* what) {
    if (luaL_dostring(L, src.c_str()) != 0) {
        std::printf("  FAIL %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
        ++g_fails;
        return false;
    }
    return true;
}
static double num(const char* global) {
    lua_getglobal(L, global);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -9999;
    lua_pop(L, 1);
    return v;
}
static bool truthy(const char* global) {
    lua_getglobal(L, global);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== what the monster editor needs ==\n");

    static Map map;
    static sim::World world;
    world.reset();
    world.set_map(&map.view);
    world.set_map_storage(&map.store);
    static nav::NavGraph nav;
    nav.build(map.view);

    static Props props;
    static i32 hp[sim::MAX_AGENTS];
    for (u32 i = 0; i < sim::MAX_AGENTS; ++i) hp[i] = 100;
    static server::lua::CommandQueue commands;
    static server::lua::EffectStack effects;
    static server::lua::WorldContext ctx;
    ctx.world = &world;
    ctx.map_store = &map.store;
    ctx.props = &props;
    ctx.hp = hp;
    ctx.commands = &commands;
    ctx.effects = &effects;
    ctx.nav = &nav;
    ctx.rng = Rng(20261016u, 3);

    static server::lua::Vm vm;
    server::lua::VmConfig cfg;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    server::lua::open_world(vm.state(), &ctx);
    L = vm.state();
    // The persistence hooks, kept as they are registered, so a test can hand
    // one a save of an older shape directly.
    if (!run("persist_hooks = {}\n"
             "local persist = dnp.persist\n"
             "dnp.persist = function(name, save, load)\n"
             "  persist_hooks[name] = { save = save, load = load }\n"
             "  return persist(name, save, load)\n"
             "end", "keeping the persistence hooks")) return 1;
    for (const char* s : {"maps/scripts/mob_vocab.lua", "maps/scripts/mobs.lua"}) {
        if (!vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, vm.last_error()); return 1; }
    }

    // ONE LOAD, every kind below: mobs.load refuses a second call. Each kind is
    // a graph that makes one question observable and does nothing else; `sight`
    // is 1 where the answer must not depend on the player being found.
    if (!run(R"LUA(
      local function still(t) t.anim = t.anim or 0; t.move = 'stop'; t.face = 'hold'; return t end
      local DEATH = { anim = 4, move = 'stop', face = 'hold', solid = false, ticks = 10, next = 'death' }
      mobs.load{ mobs = {
        -- A monster with nothing to do: `idle` waits forever, so whatever
        -- moves it is the action under test.
        blinker = { hp = 100, prop = 'zombieman', initial = 'idle', sight = '100',
          states = { idle = still{}, death = DEATH } },
        -- A BRANCH AS A STATE ENDS: `wait` runs out at 20 into `plain`, unless
        -- `go` is 1, when the transition that also asks for 20 ticks wins.
        brancher = { hp = 100, prop = 'zombieman', initial = 'wait', sight = '1',
          vars = { go = 0 },
          states = {
            wait = still{ ticks = 20, next = 'plain', transitions = {
              { to = 'branch', when = { { cond = 'state_elapsed', ticks = 20 },
                                        { cond = 'var_is', var = 'go', value = 1 } } } } },
            plain = still{}, branch = still{}, death = DEATH } },
        -- "FURTHER THAN 5", with no target at all.
        seeker = { hp = 100, prop = 'zombieman', initial = 'idle', sight = '1',
          states = {
            idle = still{ transitions = {
              { to = 'far', when = { { cond = 'target_within', distance = '5', negate = true } } } } },
            far = still{}, death = DEATH } },
        -- And the same condition not negated: the control.
        seeker_plain = { hp = 100, prop = 'zombieman', initial = 'idle', sight = '1',
          states = {
            idle = still{ transitions = {
              { to = 'near', when = { { cond = 'target_within', distance = '5' } } } } },
            near = still{}, death = DEATH } },
        -- A FASTER PHASE: `set_param move_speed` on entering each of two
        -- states that alternate every think, hundreds of times; and a flinch.
        sprinter = { hp = 100000, prop = 'zombieman', initial = 'a', sight = '1',
          pain_chance = 255,
          states = {
            a = still{ ticks = 1, next = 'b', sequence = {
              { at = 0, action = 'set_param', param = 'move_speed', value = '6' } } },
            b = still{ ticks = 1, next = 'a', sequence = {
              { at = 0, action = 'set_param', param = 'move_speed', value = '6' } } },
            pain = still{ anim = 3, ticks = 8, next = '@previous' },
            death = DEATH } },
        -- A BOSS WITH TWO PHASES, the "only once" way: `phase2` is the hidden
        -- flag, `p2` the phase it leads to, `cd` a cooldown set on the way in.
        phaser = { hp = 100, prop = 'zombieman', initial = 'p1', sight = '1',
          vars = { phase2 = 0, cd = 0 },
          states = {
            p1 = still{ transitions = {
              { to = 'p2', when = { { cond = 'var_is', var = 'phase2', value = 1 } } } } },
            p2 = still{ anim = 1, sequence = {
              { at = 0, action = 'set_cooldown', var = 'cd', ticks = 600 } } },
            death = DEATH } },
      },
      -- A THROWN GLOB: falls at 9 units a second squared, 6 a second across.
      missiles = {
        glob = { prop = 'glob', speed = '6', gravity = '9', life = 400, z_offset = '0.6667',
                 on_hit = { { action = 'damage', amount = 7 } } },
        -- MISSILES THAT MAKE MISSILES. A shard is what the others make.
        shard = { prop = 'shard', speed = '8', life = 60, z_offset = '0.6667' },
        -- Splits in five after 20 ticks, where it is.
        cluster = { prop = 'glob', speed = '6', life = 20, z_offset = '0.6667',
                    on_expire = { { action = 'missile', kind = 'shard', count = 5, arc = 20 } } },
        -- Drops a shard behind it at 10 and 20 ticks, and flies on.
        dropper = { prop = 'glob', speed = '4', life = 100, z_offset = '0.6667',
                    sequence = { { at = 20, action = 'missile', kind = 'shard', towards = 'back' },
                                 { at = 10, action = 'missile', kind = 'shard', towards = 'back' } } },
        -- Splits in two every 10 ticks: 2, 4, 8, ... unless something stops it.
        hydra = { prop = 'glob', speed = '3', life = 10, z_offset = '0.6667', max_children = 10,
                  on_expire = { { action = 'missile', kind = 'hydra', count = 2, arc = 40 } } },
        hydra_free = { prop = 'glob', speed = '3', life = 10, z_offset = '0.6667', max_children = 1000,
                       on_expire = { { action = 'missile', kind = 'hydra_free', count = 2, arc = 40 } } },
        -- AIMING: a fast dart, and three that home.
        dart = { prop = 'glob', speed = '12', life = 200, z_offset = '0.6667',
                 on_hit = { { action = 'damage', amount = 5 } } },
        homer = { prop = 'glob', speed = '8', life = 200, z_offset = '0.6667', homing = 10,
                  on_hit = { { action = 'damage', amount = 5 } } },
        homer_late = { prop = 'glob', speed = '8', life = 200, z_offset = '0.6667', homing = 10,
                       homing_after = 300, on_hit = { { action = 'damage', amount = 5 } } },
        homer_narrow = { prop = 'glob', speed = '8', life = 200, z_offset = '0.6667', homing = 10,
                         homing_cone = 2, on_hit = { { action = 'damage', amount = 5 } } },
        -- BOUNCING: once off a wall at half speed, then it bursts on the next.
        pinger = { prop = 'glob', speed = '6', life = 900, z_offset = '0.6667', bounces = 1,
                   bounce_keep = '0.5', on_bounce = { { action = 'vfx', kind = 9, at = 'impact' } },
                   on_hit = { { action = 'vfx', kind = 8, at = 'impact' } } },
        -- And a grenade, off the floor twice.
        grenade = { prop = 'glob', speed = '3', gravity = '9', life = 900, z_offset = '0.6667',
                    bounces = 2, on_bounce = { { action = 'vfx', kind = 9, at = 'impact' } },
                    on_hit = { { action = 'vfx', kind = 8, at = 'impact' } } },
        -- THROWS AT A LAUNCH SPEED: the flat one and the lob, and one that
        -- cannot reach.
        lob_low = { prop = 'glob', speed = '9', gravity = '9.8', throw = 'low', life = 400,
                    z_offset = '0.6667', on_hit = { { action = 'damage', amount = 3 } } },
        lob_high = { prop = 'glob', speed = '9', gravity = '9.8', throw = 'high', life = 400,
                     z_offset = '0.6667', on_hit = { { action = 'damage', amount = 3 } } },
        lob_weak = { prop = 'glob', speed = '3', gravity = '9.8', throw = 'low', life = 400,
                     z_offset = '0.6667', on_hit = { { action = 'damage', amount = 3 } } },
      } }
      need_problems = #mobs.problems
      local function n(v) return tonumber(tostring(v)) end
      -- `cpp` picks the runtime: true the C++ loop, false mobs.lua's own.
      function spawn_at(kind, x, y, cpp)
        mobs.use_cpp = cpp
        local e = mobs.spawn(kind, vec3.new(fx.parse(x), fx.parse(y), fx.int(0)))
        mobs.use_cpp = nil
        return e
      end
      function mob_of(e) return mobs.live[e:index()] end
      function setvar(e, k, v)
        local m = mob_of(e)
        m.vars[k] = v
        if m.cpp then dnp.mob_var(m.idx, k, v) end
      end
      function getvar(e, k)
        local m = mob_of(e)
        if m.cpp then return dnp.mob_var(m.idx, k) end
        return m.vars[k]
      end
      function state_of(e)
        local m = mob_of(e)
        if m == nil then return nil end
        if m.cpp then return (dnp.mob_state(m.idx)) end
        return m.state
      end
      function speed_of(e) return n(e:param('move_speed')) end
      -- Every effect drawn, by kind, with where.
      drawn = {}
      local vfx = dnp.vfx
      dnp.vfx = function(kind, p, ...)
        drawn[#drawn + 1] = { kind = kind, x = tonumber(tostring(p.x)), y = tonumber(tostring(p.y)) }
        return vfx(kind, p, ...)
      end
    )LUA", "loading the blinker")) return 1;
    CHECK(num("need_problems") == 0);

    const u32 player = world.spawn(fx3{fx::zero(), fx::zero(), fx::zero()});
    CHECK(world.agent(player).kind == 0);
    // The server's own order, once: Lua, the queue, the world.
    const auto settle = [&]() {
        commands.apply(world, [](u32) {});
        world.step();
    };

    // ------------------------------------------------------------ teleport
    std::printf("teleport\n");
    // The blinker, and the Lua half of one blink: target the player, act, and
    // say where it went. The C++ half puts the two bodies where a case wants
    // them, and turns the player -- whose facing is its input's, so the input
    // says it too or the next step turns it back.
    if (!run(R"LUA(
      blinker = mobs.spawn('blinker', vec3.new(fx.int(6), fx.int(0), fx.int(0)))
      blinker_idx = blinker:index()
      local function n(v) return tonumber(tostring(v)) end
      function blink_go(args)
        local m = mobs.live[blinker_idx]
        m.target, m.target_prop = dnp.players()[1], false
        drawn = {}
        mobs.acts.teleport(m, args)
        last_m = m
      end
      function where()
        local q = blinker:pos()
        bx, by = n(q.x), n(q.y)
        aim = last_m.aim or -1
        effects_drawn = #drawn
      end
    )LUA", "the blink harness")) return 1;
    CHECK(truthy("blinker"));
    const u32 blinker = static_cast<u32>(num("blinker_idx"));
    const auto parse = [](const char* s) { return fx::from_raw(static_cast<i64>(std::llround(std::atof(s) * 4294967296.0))); };
    const auto blink = [&](const char* args, const char* px, const char* py, int facing,
                           const char* mx, const char* my) {
        world.teleport(player, fx3{parse(px), parse(py), fx::zero()});
        const brad turn(static_cast<u32>(static_cast<u64>(facing % 360) * 4294967296ull / 360));
        world.agent_mut(player).facing = turn;
        proto::InputFrame f;
        f.aim = turn;
        world.set_input(player, f);
        world.teleport(blinker, fx3{parse(mx), parse(my), fx::zero()});
        world.step();
        if (!run(std::string("blink_go(") + args + ")", "blinking")) return false;
        settle();
        return run("where()", "reading where it went");
    };

    // BEHIND: the player at the origin facing +x; two behind is (-2, 0).
    if (!blink("{ to = 'behind', distance = '2', effect = 7 }", "0", "0", 0, "6", "0")) return 1;
    std::printf("  behind: arrived at (%.2f, %.2f), %d effects\n", num("bx"), num("by"), (int)num("effects_drawn"));
    CHECK(std::fabs(num("bx") + 2) < 0.01 && std::fabs(num("by")) < 0.01);
    CHECK(num("effects_drawn") == 2);                      // where it left, where it arrived
    // Facing what it came for: from (-2, 0) the player is due +x, heading 0.
    CHECK(num("aim") == 0);

    // DRAWN AS A JUMP. Each teleport is counted on the body, and a snapshot
    // whose baseline is older than the count says TELEPORTED beside the
    // position -- so a client puts it there instead of sliding it there.
    {
        const u8 before = world.teleports(blinker);
        if (!run("blink_go({ to = 'behind', distance = '2' })", "blinking again")) return 1;
        settle();
        CHECK(world.teleports(blinker) == static_cast<u8>(before + 1));
        server::EntitySample was, now;
        was.present = now.present = true;
        was.pos = fx3{F(6), fx::zero(), fx::zero()};
        now.pos = world.agent(blinker).pos;
        was.teleports = before;
        now.teleports = world.teleports(blinker);
        const u32 jump = server::delta_mask(was, now);
        CHECK((jump & proto::ent_field::TELEPORTED) && (jump & proto::ent_field::POS));
        // CONTROL: the same distance walked, with no teleport, is just a move.
        was.teleports = now.teleports;
        const u32 walk = server::delta_mask(was, now);
        CHECK(!(walk & proto::ent_field::TELEPORTED) && (walk & proto::ent_field::POS));
        // And a baseline taken after the jump does not say it again.
        was.pos = now.pos;
        CHECK(server::delta_mask(was, now) == 0);
    }

    // A WALL AT THE PLAYER'S BACK: straight behind is in it, so a little to one
    // side. Facing +x at x = -8, two behind is x = -10, the wall itself.
    if (!blink("{ to = 'behind', distance = '2' }", "-8", "0", 0, "6", "0")) return 1;
    std::printf("  behind, wall at its back: (%.2f, %.2f)\n", num("bx"), num("by"));
    CHECK(num("bx") < -8.5 && std::fabs(num("by")) > 0.9);  // behind, and off to a side
    // CONTROL: flush to the wall there is nowhere behind at all. It stays put,
    // and draws nothing.
    if (!blink("{ to = 'behind', distance = '2', effect = 7 }", "-9.5", "0", 0, "6", "0")) return 1;
    std::printf("  behind, flush to the wall: stayed at (%.2f, %.2f), %d effects\n",
                num("bx"), num("by"), (int)num("effects_drawn"));
    CHECK(std::fabs(num("bx") - 6) < 0.01 && num("effects_drawn") == 0);

    // NEAR: a hundred blinks between 4 and 14 of the player. The closet's west
    // half is inside that ring and is somewhere a body stands -- but not
    // somewhere it could walk to the player from.
    int room = 0, closet = 0, ring_ok = 0, moved = 0;
    const auto many = [&](int n) {
        room = closet = ring_ok = moved = 0;
        for (int i = 0; i < n; ++i) {
            if (!blink("{ to = 'near', distance = '14', min = '4' }", "0", "0", 0, "6", "0")) return false;
            const double x = num("bx"), y = num("by");
            if (std::fabs(x - 6) < 1e-6 && std::fabs(y) < 1e-6) continue;   // found nowhere
            ++moved;
            if (x <= 10) ++room; else if (x >= 11) ++closet;
            const double d = std::sqrt(x * x + y * y);
            if (d >= 4 - 1e-6 && d <= 14 + 1e-6) ++ring_ok;
        }
        return true;
    };
    if (!many(100)) return 1;
    std::printf("  near x100: %d moved, %d into the room, %d into the closet, %d within 4..14\n",
                moved, room, closet, ring_ok);
    CHECK(moved >= 95);
    CHECK(closet == 0);
    CHECK(ring_ok == moved);
    // CONTROL: the closet IS somewhere to stand, so its absence above is the
    // path check and not the placement...
    CHECK(run("closet_spot = dnp.place(vec3.new(fx.int(12), fx.int(0), fx.int(0)), "
              "fx.frac(20, 48), fx.frac(56, 48))", "placing in the closet") && truthy("closet_spot"));
    // ...and with the path check answering "yes" to everything, blinks do land
    // there. Without this the line above could be passing by luck.
    if (!run("real_find_path = dnp.find_path; dnp.find_path = function() return {} end",
             "faking the path check")) return 1;
    if (!many(100)) return 1;
    std::printf("  CONTROL, every spot reachable: %d into the closet\n", closet);
    CHECK(closet > 0);
    if (!run("dnp.find_path = real_find_path", "restoring the path check")) return 1;

    // WHERE A BODY CAN STAND (dnp.place), each refusal with its control.
    std::printf("dnp.place\n");
    CHECK(run(R"LUA(
      local r, h = fx.frac(20, 48), fx.frac(56, 48)
      local function at(x, y, hh) return dnp.place(vec3.new(fx.parse(x), fx.parse(y), fx.int(0)), r, hh or h) ~= nil end
      place_wall      = at('9.8', '0')      -- the wall within its radius
      place_wall_ok   = at('9.5', '0')
      place_low       = at('-12.5', '0')    -- a ceiling of 1 over a body of 1.17
      place_low_ok    = at('-12.5', '0', fx.frac(1, 2))
      place_outside   = at('10.5', '0')     -- between the room and the closet: no floor
      place_body      = at('0.2', '0')      -- inside the player
      place_body_ok   = at('3', '0')
    )LUA", "asking dnp.place"));
    CHECK(!truthy("place_wall") && truthy("place_wall_ok"));
    CHECK(!truthy("place_low") && truthy("place_low_ok"));
    CHECK(!truthy("place_outside"));
    CHECK(!truthy("place_body") && truthy("place_body_ok"));

    // The server's loop, `n` ticks: Lua, the queue, the world. The effect
    // stack is pumped by main.cpp, not by any binding, so the bodies whose
    // parameters a case reads are pumped here the same way.
    std::vector<u32> pumped;
    const auto ticks = [&](int n) {
        for (int t = 0; t < n; ++t) {
            vm.call_hook("on_tick", world.tick());
            commands.apply(world, [](u32) {});
            for (u32 i : pumped) world.params_mut(i) = effects.compute(i);
            world.step();
        }
    };
    const auto str = [&](const std::string& expr) {
        if (!run("probe = tostring(" + expr + ")", "probing")) return std::string("?");
        lua_getglobal(L, "probe");
        const std::string v = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        return v;
    };

    // GRAVITY: an arc, and a throw that lands. The blinker throws a glob at
    // the player ten units away. It must rise and come down on them; and one
    // thrown level by a player (the launch's own slope, 0) must FALL -- the
    // old `gravity` was a steady climb, which put Cinderfall's slag glob on
    // the ceiling.
    std::printf("gravity\n");
    {
        world.teleport(player, fx3{-F(5), fx::zero(), fx::zero()});
        world.teleport(blinker, fx3{F(5), fx::zero(), fx::zero()});
        world.step();
        hp[player] = 100;
        if (!run(R"LUA(
          local function n(v) return tonumber(tostring(v)) end
          local m = mobs.live[blinker_idx]
          m.target, m.target_prop = dnp.players()[1], false
          m.aim = math.floor(180 * 4294967296 / 360)          -- facing -x, at the player
          glob_peak, glob_from = -1, n(blinker:pos().z) + 0.6667
          mobs.acts.missile(m, { kind = 'glob' })
          function glob_watch()
            for _, s in pairs(mobs.flying) do
              if s.p:valid() then
                local q = s.p:pos()
                glob_peak = math.max(glob_peak, n(q.z))
                glob_x = n(q.x)
              end
            end
          end
        )LUA", "throwing a glob")) return 1;
        for (int t = 0; t < 200; ++t) {
            vm.call_hook("on_tick", world.tick());
            commands.apply(world, [](u32) {});
            world.step();
            run("glob_watch()", "watching the glob");
        }
        std::printf("  thrown at the player 10 away: peak %.2f above the launch, player hp %d\n",
                    num("glob_peak") - num("glob_from"), hp[player]);
        CHECK(num("glob_peak") - num("glob_from") > 1);    // an arc, not a line
        CHECK(hp[player] == 93);                           // and it came down on them
        // Level, from a player: no target, slope 0. From 0.67 up at g = 9 it
        // is on the floor in sqrt(2 * 0.67 / 9) = 0.385 s, 2.3 units on.
        if (!run(R"LUA(
          glob_x = nil
          mobs.launch_from(blinker, 'glob', 0, nil, fx.int(0))
        )LUA", "dropping a glob")) return 1;
        for (int t = 0; t < 120; ++t) {
            vm.call_hook("on_tick", world.tick());
            commands.apply(world, [](u32) {});
            world.step();
            run("glob_watch()", "watching the glob");
        }
        const double fell = num("glob_x") - 5;
        std::printf("  thrown level: came down %.2f on (2.31 by the formula)\n", fell);
        CHECK(fell > 2.0 && fell < 2.6);
    }

    // MISSILES THAT MAKE MISSILES: from where the parent is, on its heading,
    // on a budget. Every spawn is counted by prop name.
    std::printf("missiles that make missiles\n");
    const auto fly = [&](int n) {
        for (int t = 0; t < n; ++t) {
            vm.call_hook("on_tick", world.tick());
            commands.apply(world, [](u32) {});
            world.step();
        }
    };
    const auto turn_of = [](int deg) { return brad(static_cast<u32>(static_cast<u64>((deg % 360 + 360) % 360) * 4294967296ull / 360)); };
    world.teleport(player, fx3{fx::zero(), F(8), fx::zero()});
    world.teleport(blinker, fx3{-F(8), fx::zero(), fx::zero()});
    world.step();
    if (!run(R"LUA(
      local function n(v) return tonumber(tostring(v)) end
      spawned = {}
      local sp = dnp.spawn_prop
      dnp.spawn_prop = function(name, pos, ...)
        local p = sp(name, pos, ...)
        if p then spawned[#spawned + 1] = { name = name, x = n(pos.x), y = n(pos.y), t = dnp.tick() } end
        return p
      end
      function count(name)
        local k = 0
        for _, s in ipairs(spawned) do if s.name == name then k = k + 1 end end
        return k
      end
      -- From the blinker, east, at nothing in particular.
      function fire(kind)
        local m = mobs.live[blinker_idx]
        m.target, m.target_prop, m.aim = nil, false, 0
        spawned = {}
        mobs.acts.missile(m, { kind = kind })
      end
      -- Where the flying shards are heading, and how far apart they spread.
      function shard_report()
        local defs = mobs.missile_defs()
        local ys, angs = {}, {}
        shard_xmin, shard_xmax, shard_ys, back = 1e9, -1e9, 0, 0
        for _, s in pairs(mobs.flying) do
          if s.def == defs.shard and s.p:valid() then
            local y = math.floor(n(s.p:pos().y) * 10 + 0.5)
            if not ys[y] then ys[y] = true; shard_ys = shard_ys + 1 end
            if math.abs(s.ang - 2147483648) < 4294967296 / 360 then back = back + 1 end
          end
        end
        for _, s in ipairs(spawned) do
          if s.name == 'shard' then
            shard_xmin = math.min(shard_xmin, s.x); shard_xmax = math.max(shard_xmax, s.x)
          end
        end
      end
    )LUA", "the missile harness")) return 1;

    // A SPLIT: the cluster expires 20 ticks out (about 2.1 units east of the
    // blinker at x = -8) and five shards leave from THERE, spread by the arc.
    if (!run("fire('cluster')", "a cluster")) return 1;
    fly(22);
    fly(10);
    run("shard_report()", "the shards");
    run("cluster_shards = count('shard')", "counting");
    std::printf("  split: %d shards, born at x %.2f..%.2f, %d distinct lines\n",
                (int)num("cluster_shards"), num("shard_xmin"), num("shard_xmax"), (int)num("shard_ys"));
    CHECK(num("cluster_shards") == 5);
    CHECK(num("shard_xmin") > -6.5 && num("shard_xmax") < -5.4);   // where it was, not at the owner (-8)
    CHECK(num("shard_ys") == 5);                                    // each on its own heading
    fly(80);

    // ON THE WAY: the dropper leaves a shard behind it at 10 and at 20 ticks,
    // heading back the way it came, and flies on.
    if (!run("fire('dropper')", "a dropper")) return 1;
    fly(12);
    run("shard_report(); dropped_10 = count('shard'); back_10 = back", "after 12");
    fly(10);
    run("dropped_22 = count('shard'); local defs = mobs.missile_defs(); still = 0\n"
        "for _, s in pairs(mobs.flying) do if s.def == defs.dropper and s.p:valid() then still = still + 1 end end",
        "after 22");
    std::printf("  sequence: %d shard(s) by 12 ticks (%d heading back), %d by 22; the dropper still flying: %d\n",
                (int)num("dropped_10"), (int)num("back_10"), (int)num("dropped_22"), (int)num("still"));
    CHECK(num("dropped_10") == 1 && num("back_10") == 1);
    CHECK(num("dropped_22") == 2 && num("still") == 1);
    fly(120);

    // THE BUDGET: a hydra splits in two every 10 ticks. Ten children in all,
    // then no more -- one shot, eleven missiles.
    if (!run("fire('hydra')", "a hydra")) return 1;
    fly(70);
    run("hydras = count('glob')", "counting hydras");
    if (!run("fire('hydra_free')", "an unbudgeted hydra")) return 1;
    fly(70);
    run("hydras_free = count('glob')", "counting hydras");
    std::printf("  budget: max_children 10 made %d in all; with 1000, %d\n",
                (int)num("hydras"), (int)num("hydras_free"));
    CHECK(num("hydras") == 11);
    CHECK(num("hydras_free") > 20);                                 // CONTROL: the budget is what stopped it
    fly(120);

    // LEADING: the player runs east along y = 6; the blinker at (0, -6)
    // throws darts at it. Led, one lands; aimed where it is, it goes behind.
    std::printf("aiming and homing\n");
    if (!run(R"LUA(
      function fire_at_player(kind, lead)
        local m = mobs.live[blinker_idx]
        local p = dnp.players()[1]
        m.target, m.target_prop = p, false
        local a, b = blinker:pos(), p:pos()
        m.aim = dnp.atan2(b.y - a.y, b.x - a.x)
        mobs.acts.missile(m, { kind = kind, lead = lead })
      end
    )LUA", "the dart harness")) return 1;
    const auto dart = [&](bool lead) {
        world.teleport(player, fx3{-F(7), F(6), fx::zero()});
        world.teleport(blinker, fx3{fx::zero(), -F(6), fx::zero()});
        hp[player] = 100;
        proto::InputFrame go;
        go.move_x = 32767;
        for (int t = 0; t < 30; ++t) { world.set_input(player, go); fly(1); }
        run(std::string("fire_at_player('dart', ") + (lead ? "true" : "false") + ")", "a dart");
        for (int t = 0; t < 100; ++t) { world.set_input(player, go); fly(1); }
        world.set_input(player, proto::InputFrame{});
        fly(30);
        return hp[player];
    };
    const int led = dart(true), unled = dart(false);
    std::printf("  lead: led, the runner is at %d hp; aimed where it was, %d\n", led, unled);
    CHECK(led == 95);
    CHECK(unled == 100);                                            // CONTROL: without it, behind them

    // A PLAYER'S HOMING SHOT locks onto what their auto-aim picks. The player
    // faces 5 degrees off the blinker, ten away: straight, it misses.
    const auto homing = [&](const char* kind) {
        world.teleport(player, fx3{-F(6), fx::zero(), fx::zero()});
        world.teleport(blinker, fx3{F(4), fx::zero(), fx::zero()});
        const brad off = turn_of(5);
        world.agent_mut(player).facing = off;
        proto::InputFrame f;
        f.aim = off;
        world.set_input(player, f);
        fly(2);
        run("mobs.live[blinker_idx].hp = 100", "healing the blinker");
        run(std::string("mobs.launch_from(dnp.players()[1], '") + kind + "', " +
            std::to_string(off.raw) + ", fx.parse('0.6'), fx.int(0))", "a homing shot");
        fly(150);
        run("blinker_hp = mobs.live[blinker_idx].hp", "reading the blinker");
        return static_cast<int>(num("blinker_hp"));
    };
    const int homed = homing("homer"), late = homing("homer_late"), narrow = homing("homer_narrow");
    std::printf("  homing: locked on, the blinker is at %d; homing after 300 ticks, %d; a 2-degree cone, %d\n",
                homed, late, narrow);
    CHECK(homed == 95);
    CHECK(late == 100);                                             // CONTROL: the delay is real
    CHECK(narrow == 100);                                           // CONTROL: and so is the cone
    world.set_input(player, proto::InputFrame{});

    // BOUNCING, off the map only.
    std::printf("bouncing and throws\n");
    world.teleport(player, fx3{fx::zero(), F(8), fx::zero()});
    world.teleport(blinker, fx3{-F(8), fx::zero(), fx::zero()});
    fly(2);
    if (!run(R"LUA(
      local function n(v) return tonumber(tostring(v)) end
      function fire_at(kind, deg)
        local m = mobs.live[blinker_idx]
        m.target, m.target_prop = nil, false
        m.aim = math.floor(deg * 4294967296 / 360) % 4294967296
        drawn = {}
        drawn_at = {}
        mobs.acts.missile(m, { kind = kind })
      end
      -- When each effect was drawn, by kind.
      local vfx = dnp.vfx
      dnp.vfx = function(kind, p, ...)
        drawn_at[#drawn_at + 1] = { kind = kind, t = dnp.tick(), x = n(p.x), y = n(p.y), z = n(p.z) }
        return vfx(kind, p, ...)
      end
      function effects(kind)
        local k, first = 0, nil
        for _, e in ipairs(drawn_at) do if e.kind == kind then k = k + 1; first = first or e end end
        return k, first
      end
      -- The one of this kind in the air.
      function flying_of(kind)
        local def = mobs.missile_defs()[kind]
        for _, s in pairs(mobs.flying) do if s.def == def and s.p:valid() then return s end end
      end
      function flying_ang(kind) local s = flying_of(kind); return s and s.ang or -1 end
      function flying_z(kind) local s = flying_of(kind); return s and n(s.p:pos().z) or -1 end
    )LUA", "the bounce harness")) return 1;
    // East from x = -8 at 6 a second: the east wall at 10 is three seconds off.
    if (!run("fire_at('pinger', 0)", "a pinger")) return 1;
    fly(1000);
    run("bounced, b1 = effects(9); burst, b2 = effects(8)\n"
        "bounce_x, bounce_t = b1 and b1.x or 0, b1 and b1.t or 0\n"
        "burst_x, burst_t = b2 and b2.x or 0, b2 and b2.t or 0", "reading the pinger");
    const double out_ticks = num("bounce_t"), back_ticks = num("burst_t") - num("bounce_t");
    std::printf("  a wall: %d bounce at x %.2f, then %d burst at x %.2f, %.0f ticks after\n",
                (int)num("bounced"), num("bounce_x"), (int)num("burst"), num("burst_x"), back_ticks);
    CHECK(num("bounced") == 1 && num("bounce_x") > 9);
    CHECK(num("burst") == 1 && num("burst_x") < -9);               // it came back, and burst on the next
    // At half speed: the way back (about 19 units at 3 a second) takes twice
    // the way out would (about 18 at 6), give or take a tick or two.
    CHECK(back_ticks > 2 * (19.0 * 60 / 6) * 0.9);
    (void)out_ticks;
    // A WALL MET AT AN ANGLE: 30 degrees north of east, off the east wall, is
    // 150 degrees on the way back.
    world.teleport(blinker, fx3{fx::zero(), -F(4), fx::zero()});
    fly(2);
    if (!run("fire_at('pinger', 30)", "an angled pinger")) return 1;
    int t_to_wall = 0;
    for (; t_to_wall < 400; ++t_to_wall) {
        fly(1);
        run("bounced_now = effects(9)", "");
        if (num("bounced_now") > 0) break;
    }
    fly(2);
    run("after_ang = flying_ang('pinger')", "reading the heading");
    const double deg = num("after_ang") * 360.0 / 4294967296.0;
    std::printf("  at an angle: 30 degrees in, %.1f degrees out\n", deg);
    CHECK(std::fabs(deg - 150) < 1.0);
    fly(800);
    // A GRENADE off the floor, twice, and up again after the first.
    world.teleport(blinker, fx3{-F(8), fx::zero(), fx::zero()});
    fly(2);
    if (!run("fire_at('grenade', 0)", "a grenade")) return 1;
    double rose = 0;
    for (int t = 0; t < 120; ++t) {
        fly(1);
        run("bounced_now = effects(9); gz = flying_z('grenade')", "");
        if (num("bounced_now") == 1) rose = std::max(rose, num("gz"));
    }
    fly(400);
    run("grenade_bounces = effects(9); grenade_bursts = effects(8)", "reading the grenade");
    std::printf("  a grenade: %d bounces, up to %.2f after the first, then %d burst\n",
                (int)num("grenade_bounces"), rose, (int)num("grenade_bursts"));
    CHECK(num("grenade_bounces") == 2 && num("grenade_bursts") == 1);
    CHECK(rose > 0.1);                                             // it went up off the floor
    // CONTROL: no bounces, the pinger's first wall is where it bursts.
    // (`dart` is the same flight without them.)
    fly(10);

    // THROWS AT A LAUNCH SPEED. The player six away; the low throw and the
    // lob both land, the lob higher. One too slow to reach falls short.
    const auto lob = [&](const char* kind, double& peak) {
        world.teleport(player, fx3{-F(2), fx::zero(), fx::zero()});
        world.teleport(blinker, fx3{-F(8), fx::zero(), fx::zero()});
        world.set_input(player, proto::InputFrame{});
        fly(2);
        hp[player] = 100;
        run(std::string("fire_at_player('") + kind + "', false)", "a lob");
        peak = -1;
        for (int t = 0; t < 300; ++t) {
            fly(1);
            run(std::string("gz = flying_z('") + kind + "')", "");
            peak = std::max(peak, num("gz"));
        }
        return hp[player];
    };
    double peak_low = 0, peak_high = 0, peak_weak = 0;
    const int low = lob("lob_low", peak_low), high = lob("lob_high", peak_high),
              weak = lob("lob_weak", peak_weak);
    std::printf("  throws at 9 a second, six away: low hp %d (peak %.2f), high hp %d (peak %.2f); "
                "at 3 a second, out of reach: hp %d\n", low, peak_low, high, peak_high, weak);
    CHECK(low == 97 && high == 97);
    CHECK(peak_high > peak_low + 1);                               // the lob is the lob
    CHECK(weak == 100);                                            // CONTROL: out of reach, short

    for (const bool cpp : {true, false}) {
        const char* rt = cpp ? "true" : "false";
        std::printf("--- the %s runtime ---\n", cpp ? "C++" : "Lua");

        // A BRANCH AS A STATE ENDS, for a monster that thinks every 8th tick.
        // Eight of them, spawned on eight consecutive ticks, so between them
        // every alignment of a think against the 20-tick deadline is covered.
        // The ninth is the control: `go` left at 0 runs out into `plain`.
        if (!run("branchers = {}", "the branchers")) return 1;
        for (int k = 0; k < 9; ++k) {
            const std::string one = "branchers[" + std::to_string(k + 1) + "] = spawn_at('brancher', '" +
                                    std::to_string(-8 + k) + "', '-8', " + rt + ")\n" +
                                    (k < 8 ? "setvar(branchers[" + std::to_string(k + 1) + "], 'go', 1)\n" : "");
            if (!run(one, "a brancher")) return 1;
            ticks(1);
        }
        ticks(60);
        int branched = 0;
        for (int k = 1; k <= 8; ++k) branched += str("state_of(branchers[" + std::to_string(k) + "])") == "branch";
        const std::string control = str("state_of(branchers[9])");
        std::printf("  branch as a state ends: %d of 8 took the branch; the control is in %s\n",
                    branched, control.c_str());
        CHECK(branched == 8);
        CHECK(control == "plain");

        // A NEGATED target_within WITH NO TARGET. It holds: "not within 5 of a
        // target" is true of no target. The editor's preview says otherwise,
        // so the studio must put `has_target` beside it when it means "a seen
        // target further than 5".
        if (!run(std::string("seek = spawn_at('seeker', '-8', '8', ") + rt + ")\n"
                 "seek_plain = spawn_at('seeker_plain', '-6', '8', " + rt + ")", "the seekers")) return 1;
        ticks(30);
        const std::string seek = str("state_of(seek)"), seek_plain = str("state_of(seek_plain)");
        const std::string has = str("mob_of(seek).target ~= nil");
        std::printf("  negated target_within, no target: %s (has a target: %s); not negated: %s\n",
                    seek.c_str(), has.c_str(), seek_plain.c_str());
        CHECK(has == "false");
        CHECK(seek == "far");
        CHECK(seek_plain == "idle");                       // CONTROL: the plain one does not hold

        // SET_PARAM LASTS, REPLACES ITSELF, AND SURVIVES PAIN.
        const u32 effects_before = effects.count();
        if (!run(std::string("sprint = spawn_at('sprinter', '6', '8', ") + rt + ")\n"
                 "sprint_idx = sprint:index()", "the sprinter")) return 1;
        pumped.push_back(static_cast<u32>(num("sprint_idx")));
        ticks(3000);                                       // a state entry every think: ~375 of them
        const double speed = std::atof(str("speed_of(sprint)").c_str());
        const u32 held = effects.count() - effects_before;
        std::printf("  set_param x~375: move_speed %.3f, %u effect(s) held\n", speed, held);
        CHECK(std::fabs(speed - 6) < 1e-6);
        CHECK(held == 1);                                  // one, replaced each time; it used to be one per entry
        if (!run("for _ = 1, 20 do if state_of(sprint) == 'pain' then break end; "
                 "mobs.hurt(sprint, 1, nil) end\n"
                 "sprint_pained = state_of(sprint)", "hurting the sprinter")) return 1;
        CHECK(str("sprint_pained") == "pain");
        ticks(40);
        const double after = std::atof(str("speed_of(sprint)").c_str());
        std::printf("  after a flinch: %s, move_speed %.3f\n", str("state_of(sprint)").c_str(), after);
        CHECK(std::fabs(after - 6) < 1e-6);
        CHECK(str("state_of(sprint)") != "pain");
        pumped.clear();

        // THE HIDDEN VAR, THE STATE IT LED TO, AND A COOLDOWN, THROUGH A SAVE.
        if (!run(std::string("boss = spawn_at('phaser', '-8', '4', ") + rt + ")\n"
                 "setvar(boss, 'phase2', 1)", "the boss")) return 1;
        ticks(20);
        CHECK(str("state_of(boss)") == "p2");
        ticks(100);
        const double left_before = std::atof(str("getvar(boss, 'cd') - dnp.tick()").c_str());
        std::string saved, err;
        CHECK(server::lua::persist_capture(L, saved, err));
        // Everything gone, as a level being reloaded: the monsters despawned and
        // the runtimes' tables cleared, and some time passing.
        if (!run("for idx, m in pairs(mobs.live) do if m.e:valid() then m.e:despawn() end end\n"
                 "mobs.reset()", "clearing the level")) return 1;
        ticks(50);
        if (!run(std::string("mobs.use_cpp = ") + rt, "choosing the runtime")) return 1;
        err.clear();
        CHECK(server::lua::persist_restore(L, saved, err));
        if (!run("mobs.use_cpp = nil\n"
                 "for _, m in pairs(mobs.live) do if m.def.name == 'phaser' then boss = m.e end end",
                 "finding the boss again")) return 1;
        ticks(1);
        const std::string st = str("state_of(boss)");
        const std::string flag = str("getvar(boss, 'phase2')");
        const double left_after = std::atof(str("getvar(boss, 'cd') - dnp.tick()").c_str());
        std::printf("  through a save: state %s, phase2 %s, cooldown %.0f left before, %.0f after\n",
                    st.c_str(), flag.c_str(), left_before, left_after);
        CHECK(st == "p2");
        CHECK(flag == "1");
        CHECK(left_after > 400 && std::fabs(left_after - (left_before - 1)) <= 1);
        // CONTROL: the same save with the state and vars cut out is what used to
        // be written, and it brings the boss back in phase 1 with the flag clear.
        if (!run("for idx, m in pairs(mobs.live) do if m.e:valid() then m.e:despawn() end end\n"
                 "mobs.reset()", "clearing the level again")) return 1;
        ticks(5);
        if (!run(std::string("mobs.use_cpp = ") + rt + "\n"
                 "persist_hooks.mobs.load({ { kind = 'phaser', hp = 100,\n"
                 "  at = vec3.new(fx.int(-8), fx.int(4), fx.int(0)) } })\n"
                 "mobs.use_cpp = nil\n"
                 "for _, m in pairs(mobs.live) do if m.def.name == 'phaser' then boss = m.e end end",
                 "restoring an old save")) return 1;
        ticks(1);
        std::printf("  CONTROL, the old save: state %s, phase2 %s\n",
                    str("state_of(boss)").c_str(), str("getvar(boss, 'phase2')").c_str());
        CHECK(str("state_of(boss)") == "p1" && str("getvar(boss, 'phase2')") == "0");
        if (!run("for idx, m in pairs(mobs.live) do if m.e:valid() then m.e:despawn() end end\n"
                 "mobs.reset()", "clearing the level for the next runtime")) return 1;
        ticks(5);
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
