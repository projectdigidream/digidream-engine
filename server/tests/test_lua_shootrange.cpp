//
// The shooting range: do a monster's shots actually reach the player?
//
// WHY THIS EXISTS. Monsters were reported as damaging each other and never the
// player, and their tracers as stubs drawn inside the shooter. Every hop of
// that path checked out under inspection -- the engine casts along the body's
// facing, the tracer carries that same angle and length, the encoder and the
// client are faithful -- which left "the shots really do stop short" and no way
// to tell a wall from a neighbour from aim lag by reading a description.
//
// AND WHY IT IS HERE RATHER THAN IN tools/mobcheck. That harness is a fake
// world, and four bugs this phase came from it being more generous than the
// engine: set_input taking a table, fx living under `dnp`, dnp.hitscan
// returning four values instead of two, props answering methods only entities
// have. Every one loaded there and raised on a real server. So this test uses
// the REAL bindings, the REAL World, and the REAL hitscan -- the same
// server/src/lua/world_binding.cpp a live game runs -- and the only things
// standing in are the map and the prop catalogue.
//
// The room is empty on purpose. A Doom corridor cannot tell you whether a short
// shot hit a wall or a monster; a bare floor with nothing between the shooter
// and the target can, because there is nothing else for a bullet to find.
//
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "dnp/sim/world.hpp"
#include "dnp/geom/mapstore.hpp"
#include "lua/vm.hpp"
#include "lua/world_binding.hpp"
#include "lua/commands.hpp"
#include "lua/effects.hpp"

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
static fx Ff(i64 a, i64 b) { return fx::from_frac(a, b); }

// Only the hitscan monsters, with Doom's own proportions -- 48 map units to the
// engine's one. Enough of assets/props.txt to place them, and no more: what is
// under test is where the bullets go, not whether a file parses.
struct RangeProps final : public server::lua::PropCatalogueView {
    struct Entry { const char* name; u16 kind; fx radius; fx height; fx speed; };
    static constexpr fx du(i64 n) { return fx::from_frac(n, 48); }

    Entry table[4] = {
        {"zombieman",      20, du(20), du(56), du(8 * 35)},
        {"shotgun_guy",    30, du(20), du(56), du(8 * 35)},
        {"chaingunner",    31, du(20), du(56), du(8 * 35)},
        {"wolfenstein_ss", 45, du(20), du(56), du(8 * 35)},
    };

    const Entry* find(u16 kind) const {
        for (const Entry& e : table) if (e.kind == kind) return &e;
        return nullptr;
    }

    u16 kind_by_name(const char* name, size_t len) const override {
        for (const Entry& e : table) {
            if (std::strlen(e.name) == len && std::strncmp(e.name, name, len) == 0) {
                return e.kind;
            }
        }
        return 0;
    }
    fx radius_of(u16 k) const override { const Entry* e = find(k); return e ? e->radius : Ff(1, 2); }
    fx height_of(u16 k) const override { const Entry* e = find(k); return e ? e->height : F(2); }
    fx speed_of(u16 k)  const override { const Entry* e = find(k); return e ? e->speed : fx::zero(); }
    u8 flags_of(u16)    const override { return 0; }
    bool empty()        const override { return false; }
    const char* name_of(u16 k) const override { const Entry* e = find(k); return e ? e->name : ""; }
};

// One open room, big enough that the furthest monster is well inside it and
// nothing is ever near a wall. 120 across, centred on the origin, where the
// range script puts the player.
struct Room {
    geom::MapStorage store;
    geom::MapData    view{};
    Room() {
        const fx2 p[] = {{-F(60), -F(60)}, {F(60), -F(60)}, {F(60), F(60)}, {-F(60), F(60)}};
        store.add_sector(p, 4, fx::zero(), F(40), 1);
        for (u16 i = 0; i < 4; ++i) {
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
        }
        view = store.view();
    }
};

// Read a number out of a Lua table indexed by entity index, or -1.
static double table_number(lua_State* L, const char* global, u32 idx) {
    lua_getglobal(L, global);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return -1; }
    lua_pushnumber(L, idx);
    lua_rawget(L, -2);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 2);
    return v;
}

int main() {
    // Unbuffered: this test stands up a whole world and a VM, and a crash on
    // the way loses a buffered log entirely -- which is a segfault with no
    // output at all and nothing to say where.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== the shooting range ==\n");

    std::printf("  [1] building the room\n");
    static Room room;
    std::printf("  [2] world\n");
    static sim::World world;
    world.reset();
    world.set_map(&room.view);
    world.set_map_storage(&room.store);

    // THE PLAYER: an agent of kind 0, which is exactly what dnp.players()
    // looks for. World::spawn makes one directly, so this needs no session, no
    // edge and no browser -- which is what lets it be a test rather than a
    // thing somebody runs by hand and squints at.
    std::printf("  [3] spawning the player\n");
    const u32 player = world.spawn(fx3{fx::zero(), fx::zero(), fx::zero()});
    CHECK(world.agent(player).kind == 0);

    static RangeProps props;
    static i32 hp[sim::MAX_AGENTS];
    for (u32 i = 0; i < sim::MAX_AGENTS; ++i) hp[i] = 100;

    // A COMPLETE context, as server/src/main.cpp builds one.
    //
    // Leaving `commands` null crashed on the first spawn: several bindings
    // dereference it without a check, on the reasonable assumption that a
    // server always has one. A test with half a context is testing a
    // configuration nothing runs -- the same mistake as a fake world that is
    // more generous than the engine, one level up.
    static server::lua::CommandQueue commands;
    static server::lua::EffectStack effects;

    static server::lua::WorldContext ctx;
    ctx.world = &world;
    ctx.map_store = &room.store;
    ctx.props = &props;
    ctx.hp = hp;
    ctx.commands = &commands;
    ctx.effects = &effects;
    ctx.rng = Rng(20260830u, 11);

    std::printf("  [4] vm\n");
    static server::lua::Vm vm;
    server::lua::VmConfig cfg;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    std::printf("  [5] open_world\n");
    server::lua::open_world(vm.state(), &ctx);

    const char* scripts[] = {
        "maps/scripts/mob_vocab.lua",
        "maps/scripts/mobs.lua",
        "maps/mobs/doom_monsters.lua",
        "modules/shootrange/scripts/range.lua",
    };
    std::printf("  [6] scripts\n");
    for (const char* s : scripts) {
        std::printf("      loading %s\n", s);
        if (!vm.run_file(s, s)) {
            std::printf("  FAIL loading %s: %s\n", s, vm.last_error());
            return 1;
        }
    }

    // AIM, not scatter. `mobs.spread_scale` multiplies every declared spread,
    // and this test sets it to zero deliberately: the assertions below say every
    // monster's shot reaches the player, which is a claim about whether the ray
    // is pointed at anything. Declared spreads run 5 to 12 degrees, and at the
    // 26 units this fixture places monsters at, 5 degrees is a two-unit lateral
    // miss against a body of radius 0.5 -- so with spread on, "every shot
    // connects" is false BY DESIGN, and stray rays hit other monsters and start
    // infighting, which changes who is even shooting at the player.
    //
    // Weakening the assertions to accommodate that would throw away the thing
    // this test measures. The scatter itself is asserted separately at the end,
    // against the binding that produces it.
    if (luaL_dostring(vm.state(), "mobs.spread_scale = 0") != 0) {
        std::printf("  FAIL disabling spread: %s\n", lua_tostring(vm.state(), -1));
        return 1;
    }

    lua_getglobal(vm.state(), "range_expected");
    const int expected = static_cast<int>(lua_tonumber(vm.state(), -1));
    lua_pop(vm.state(), 1);
    std::printf("  %d monsters placed around the player\n", expected);
    CHECK(expected == 24);

    // Long enough for the slowest cooldown several times over: a zombieman
    // reloads in 86 ticks, so 1200 is a dozen shots each even allowing for the
    // think interval.
    // The server's own order: Lua decides, the queue flushes, the world steps.
    // Anything else would test a sequence no game runs.
    for (u32 t = 0; t < 1200; ++t) {
        vm.call_hook("on_tick", world.tick());
        commands.apply(world, [](u32) {});
        world.step();
    }

    // ---------------------------------------------------------------- results
    //
    // Reported per bearing and range before anything is asserted, because the
    // number this test exists to produce is the table, not the pass.
    int fired = 0, landed = 0, silent = 0, missed = 0;
    lua_State* L = vm.state();

    lua_getglobal(L, "range_placed");
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        const u32 idx = static_cast<u32>(lua_tonumber(L, -2));
        lua_getfield(L, -1, "kind");
        const char* kind = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "bearing");
        const int bearing = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, -1, "dist");
        const int dist = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);

        // The tables are read while `range_placed` is on the stack, which is
        // safe: table_number pushes and pops its own.
        const double shots = table_number(L, "range_shots", idx);
        const double hits  = table_number(L, "range_hits", idx);
        const double reach = table_number(L, "range_reach", idx);

        std::printf("  %-15s %3d deg %2d away: %2d shots, %2d reached, furthest %d\n",
                    kind ? kind : "?", bearing, dist,
                    shots > 0 ? static_cast<int>(shots) : 0,
                    hits > 0 ? static_cast<int>(hits) : 0,
                    reach > 0 ? static_cast<int>(reach) : 0);

        if (shots > 0) ++fired; else ++silent;
        if (hits > 0) ++landed; else if (shots > 0) ++missed;

        lua_pop(L, 1);   // the record; the key stays for lua_next
    }
    lua_pop(L, 1);

    std::printf("  --> %d of %d fired, %d landed at least one, %d fired and never "
                "connected, %d never fired\n",
                fired, expected, landed, missed, silent);

    // EVERY monster gets a shot off. One that never fires is a different
    // failure from one that fires and misses -- a stuck state machine rather
    // than bad aim -- and lumping them together is how "monsters do not damage
    // the player" stayed one question for as long as it did.
    CHECK(silent == 0);

    // And every one of them connects. There is nothing else in the room: no
    // wall within reach, no neighbour on the line, so a shot that does not
    // arrive has no excuse.
    CHECK(landed == expected);
    CHECK(missed == 0);

    // THE PLAYER WAS SHOT, which is the plain version of the report. Health is
    // pinned back to full by the range script on every hit -- otherwise the
    // player dies and the whole geometry changes -- so this counts hits rather
    // than reading a number off the agent.
    CHECK(landed > 0);

    // ------------------------------------------------------------- the tracer
    //
    // `range_reach` is how far the furthest bullet got, which is the length a
    // tracer draws. A monster 26 units away whose best shot reached 2 is the
    // "stub drawn inside the shooter" that was reported, and it would pass
    // every assertion above as long as it eventually connected.
    lua_getglobal(L, "range_placed");
    lua_pushnil(L);
    int stubs = 0, measured = 0;
    while (lua_next(L, -2) != 0) {
        const u32 idx = static_cast<u32>(lua_tonumber(L, -2));
        lua_getfield(L, -1, "dist");
        const int dist = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        const double reach = table_number(L, "range_reach", idx);
        if (reach >= 0) {
            ++measured;
            // Half the distance to the player is a generous floor: a shot that
            // reached the target reaches all of it, and one that stopped at a
            // fraction of a unit is the thing under suspicion.
            if (reach * 2 < dist) ++stubs;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::printf("  --> %d of %d monsters' best shot fell short of half the range\n",
                stubs, measured);
    CHECK(measured == expected);
    CHECK(stubs == 0);

    // ----------------------------------------------------- the effect tag
    //
    // `tag` sat in the vocabulary and in docs/MOBS.md since effects existed and
    // was read by nothing: an argument an author could write, that looked
    // configured and did nothing. It is load-bearing now because selecting a
    // weapon writes its swing shape into these same replicated parameters, so
    // ten weapon switches would otherwise leave ten effects per parameter and
    // the tenth reach would be the product of all of them.
    //
    // Measured through EffectStack::compute, the server's own recomputation
    // (main.cpp:1168), rather than by reading the live parameter back: this test
    // does not run the server's effect pump, so the live value never moves. It
    // would be easy to pump it here instead -- and that would be a second,
    // more generous copy of the loop, which is the mistake the header of this
    // file is about.
    const auto lua_do = [L](const char* src, const char* what) {
        if (luaL_dostring(L, src) != 0) {
            std::printf("  FAIL %s: %s\n", what, lua_tostring(L, -1));
            return false;
        }
        return true;
    };
    effects.set_base(player, world.params(player));
    const fx tag_base = effects.base(player).attack_reach;
    const auto reach_now = [&] { return effects.compute(player).attack_reach; };

    // FIRST, that the measurement works at all. attack_reach cannot legitimately
    // be zero, so a zero here means the readback broke -- and against zero,
    // base*2 and base*16 are also zero, so every assertion below would pass
    // vacuously AND so would the control. That is exactly how this block first
    // ran green while proving nothing.
    CHECK(tag_base.raw > 0);
    CHECK(reach_now().raw == tag_base.raw);

    if (!lua_do("for _ = 1, 4 do"
                "  mobs.tagged_effect(dnp.players()[1], 'weapon',"
                "                     'attack_reach', 'mul', fx.int(2))"
                "end", "tagging")) return 1;
    const fx tag_replaced = reach_now();

    if (!lua_do("mobs.clear_tagged(dnp.players()[1], 'weapon')", "clearing")) return 1;
    const fx tag_cleared = reach_now();

    // CONTROL. The same four calls WITHOUT a tag, which is what this engine did
    // before tagged_effect existed. If these do not compound, the assertion
    // above proves nothing -- a stack that already replaced would satisfy it.
    if (!lua_do("for _ = 1, 4 do"
                "  dnp.players()[1]:add_effect('attack_reach', 'mul', fx.int(2))"
                "end", "stacking")) return 1;
    const fx tag_stacked = reach_now();

    std::printf("  --> attack_reach %lld raw; x2 tagged four times -> %lld; "
                "cleared -> %lld; x2 untagged four times -> %lld\n",
                static_cast<long long>(tag_base.raw),
                static_cast<long long>(tag_replaced.raw),
                static_cast<long long>(tag_cleared.raw),
                static_cast<long long>(tag_stacked.raw));

    // Four tagged doublings are ONE doubling.
    CHECK(tag_replaced.raw == tag_base.raw * 2);
    // And dropping the tag puts the parameter back EXACTLY, with no residue --
    // the property test_lua_effects exists for, here reached through Lua.
    CHECK(tag_cleared.raw == tag_base.raw);
    // CONTROL: untagged, the same four calls compound to sixteen times.
    CHECK(tag_stacked.raw == tag_base.raw * 16);

    // ------------------------------------------------- the offset, for the first time
    //
    // `dnp.hitscan`'s third argument is the angular offset that makes a shotgun
    // a shotgun, and until this block nothing in the repository passed one: the
    // monster path called hitscan with two arguments, so every claim about
    // hitscan came through a ray that could not be deflected. A binding that
    // ignored the argument entirely would have passed every test that existed.
    //
    // Asserted against the real binding, from the real player, into the real
    // room this test has already filled with 24 monsters at known bearings.
    // Sweeping a full turn in sixteenths must reach more than one of them.
    static const char* kSweep = R"LUA(
      local p = dnp.players()[1]
      local turn = 4294967296
      local seen, hits = {}, 0
      for i = 0, 15 do
        local who = dnp.hitscan(p, 64, math.floor(i * turn / 16))
        if who then
          local k = tostring(who)
          if not seen[k] then seen[k] = true; hits = hits + 1 end
        end
      end
      sweep_distinct = hits
      -- CONTROL. The same count of calls, all at the SAME offset, must find
      -- exactly one thing. Without this the assertion above passes against a
      -- binding that ignores the offset and happens to be facing a monster:
      -- sixteen identical rays would report one distinct target, and a version
      -- that deflected nothing would be indistinguishable from one that did.
      local fixed = {}
      local n = 0
      for _ = 1, 16 do
        local who = dnp.hitscan(p, 64, 0)
        if who then
          local k = tostring(who)
          if not fixed[k] then fixed[k] = true; n = n + 1 end
        end
      end
      fixed_distinct = n
    )LUA";
    if (luaL_dostring(L, kSweep) != 0) {
        std::printf("  FAIL sweeping: %s\n", lua_tostring(L, -1));
        return 1;
    }
    lua_getglobal(L, "sweep_distinct");
    const int swept = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_getglobal(L, "fixed_distinct");
    const int fixed = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    std::printf("  --> 16 rays swept round a turn found %d distinct targets; "
                "16 rays at one offset found %d\n", swept, fixed);
    CHECK(swept > 1);
    CHECK(fixed <= 1);

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
