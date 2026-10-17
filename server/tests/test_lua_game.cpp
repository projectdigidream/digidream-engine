//
// GAME SCRIPTS (docs/MODULES.md, "Game scripts"): the weapons in the game's
// VM, the monsters in a level's, one real world under both.
//
// A module may run weapons.lua once for the whole game, so a player's
// inventory and panel follow them from level to level. But what a weapon DOES
// -- the ray, the missile, the blast -- is the mob runtime's, and the mob
// runtime is the level's: it owns the monsters and their hit points. So the
// game asks the level the player is in to fire (dnp.call_level, into
// mobs.lua's weapon_runtime), and the level asks the game what a connected
// swing costs (dnp.call_game, into weapons.lua's weapon_swing).
//
// Only the real bindings can say whether that works, because the whole of it
// is two lua_States agreeing about one World: handles copied from one to the
// other, a hit landing in the VM that keeps the monster's health, an effect
// written by one reaching the parameters the other reads. Each claim has a
// CONTROL that takes the bridge away and watches the claim fail.
//
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

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

struct Props final : public server::lua::PropCatalogueView {
    struct Entry { const char* name; u16 kind; fx radius; fx height; fx speed; };
    static constexpr fx du(i64 n) { return fx::from_frac(n, 48); }
    Entry table[4] = {
        {"zombieman",  20, du(20), du(56), du(8 * 35)},
        {"rocket",     53, du(11), du(16), du(20 * 35)},
        {"ammo_shells", 8, du(10), du(16), fx::zero()},
        {"w_shotgun",  82, du(10), du(16), fx::zero()},
    };
    const Entry* find(u16 kind) const {
        for (const Entry& e : table) if (e.kind == kind) return &e;
        return nullptr;
    }
    u16 kind_by_name(const char* name, size_t len) const override {
        for (const Entry& e : table) {
            if (std::strlen(e.name) == len && std::strncmp(e.name, name, len) == 0) return e.kind;
        }
        return 0;
    }
    fx radius_of(u16 k) const override { const Entry* e = find(k); return e ? e->radius : fx::from_frac(1, 2); }
    fx height_of(u16 k) const override { const Entry* e = find(k); return e ? e->height : F(2); }
    fx speed_of(u16 k)  const override { const Entry* e = find(k); return e ? e->speed : fx::zero(); }
    u8 flags_of(u16)    const override { return 0; }
    bool empty()        const override { return false; }
    const char* name_of(u16 k) const override { const Entry* e = find(k); return e ? e->name : ""; }
};

struct Room {
    geom::MapStorage store;
    geom::MapData    view{};
    Room() {
        const fx2 p[] = {{-F(60), -F(60)}, {F(60), -F(60)}, {F(60), F(60)}, {-F(60), F(60)}};
        store.add_sector(p, 4, fx::zero(), F(40), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
        view = store.view();
    }
};

// What main.cpp is, reduced to the two calls: one game VM, one level VM.
struct Cross final : server::lua::CrossSink {
    lua_State* game = nullptr;
    lua_State* level = nullptr;
    bool level_reachable = true;   // the CONTROL turns the level off
    lua_State* enter_game(const server::lua::WorldContext&) override { return game; }
    lua_State* enter_level_of(const server::lua::WorldContext&, u32) override {
        return level_reachable ? level : nullptr;
    }
    lua_State* neighbour = nullptr;     // level 1, for the region's hand-over
    lua_State* enter_level(const server::lua::WorldContext&, u32 li) override {
        return (li == 1 && level_reachable) ? neighbour : nullptr;
    }
    void leave(lua_State*) override {}
};

static bool run(lua_State* L, const char* src, const char* what) {
    if (luaL_dostring(L, src) != 0) {
        std::printf("  FAIL %s: %s\n", what, lua_tostring(L, -1));
        ++g_fails;
        lua_pop(L, 1);
        return false;
    }
    return true;
}
static double num(lua_State* L, const char* global) {
    lua_getglobal(L, global);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 1);
    return v;
}
static std::string str(lua_State* L, const char* global) {
    lua_getglobal(L, global);
    const char* v = lua_tostring(L, -1);
    std::string out = v ? v : "";
    lua_pop(L, 1);
    return out;
}

static bool truthy(lua_State* L, const char* global) {
    lua_getglobal(L, global);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && chdir(argv[1]) != 0) { std::printf("  FAIL chdir\n"); return 1; }
    std::printf("== game scripts ==\n");

    static Room room;
    static sim::World world;
    world.reset();
    world.set_map(&room.view);
    world.set_map_storage(&room.store);
    const u32 player = world.spawn(fx3{fx::zero(), fx::zero(), fx::zero()});

    static Props props;
    static i32 hp[sim::MAX_AGENTS];
    for (u32 i = 0; i < sim::MAX_AGENTS; ++i) hp[i] = 100;
    // Who each body is: the player is player 7, in every world it is ever in.
    static u32 players[sim::MAX_AGENTS]{};
    players[player] = 7;
    static server::lua::CommandQueue commands;
    static server::lua::EffectStack effects;
    static Cross cross;

    // The two contexts look at the same world, as main.cpp binds them.
    static server::lua::WorldContext level_ctx, game_ctx;
    for (server::lua::WorldContext* c : {&level_ctx, &game_ctx}) {
        c->world = &world;
        c->map_store = &room.store;
        c->props = &props;
        c->hp = hp;
        c->commands = &commands;
        c->effects = &effects;
        c->player = players;
        c->cross = &cross;
    }
    level_ctx.rng = Rng(1, 1);
    game_ctx.rng = Rng(2, 2);
    game_ctx.game = true;

    static server::lua::Vm level_vm, game_vm;
    server::lua::VmConfig cfg;
    if (!level_vm.open(cfg) || !game_vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    server::lua::open_world(level_vm.state(), &level_ctx);
    server::lua::open_world(game_vm.state(), &game_ctx);
    lua_State* LV = level_vm.state();
    lua_State* GV = game_vm.state();
    cross.level = LV;
    cross.game = GV;

    // The level: the monsters. The game: the weapons -- the same files a doom2
    // map lists, split the way `game` lines in module.txt split them.
    for (const char* s : {"maps/scripts/mob_vocab.lua", "maps/scripts/mobs.lua",
                          "maps/mobs/doom_monsters.lua"}) {
        if (!level_vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, level_vm.last_error()); return 1; }
    }
    for (const char* s : {"maps/scripts/weapon_vocab.lua", "maps/scripts/weapons.lua",
                          "../modules/doom2/maps/weapons_doom2.lua"}) {
        if (!game_vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, game_vm.last_error()); return 1; }
    }

    // ------------------------------------------------------------ loading
    if (!run(GV, "errs = weapons.load_errors(); is_game = weapons.game", "reading the game")) return 1;
    std::printf("  the game VM loaded Doom's weapons with %d complaints\n", (int)num(GV, "errs"));
    CHECK(num(GV, "errs") == 0);
    CHECK(truthy(GV, "is_game"));
    // CONTROL: the level's VM is not the game's.
    if (!run(LV, "level_is_game = dnp.is_game()", "asking the level")) return 1;
    CHECK(!truthy(LV, "level_is_game"));

    // WHAT THE PLAYER'S OWN CLIENT PREDICTS, read off DOOM's weapons' phases:
    // a press for the pistol, a hold for the chaingun, the swing for the fists,
    // each with the ticks until it can fire again.
    if (!run(GV, "local function p(w) local e = weapons.defs[w].predict; return e and e[1] end\n"
                 "pistol_on = p('pistol') and p('pistol').on\n"
                 "pistol_every = p('pistol') and p('pistol').every or -1\n"
                 "chaingun_on = p('chaingun') and p('chaingun').on\n"
                 "fists_on = p('fists') and p('fists').on\n"
                 "ssg_every = p('supershotgun') and p('supershotgun').every or -1\n"
                 "ssg_fire = weapons.defs.supershotgun.phases.fire.ticks\n", "reading predict")) return 1;
    {
        const auto s = [&](const char* g) {
            lua_getglobal(GV, g);
            std::string v = lua_tostring(GV, -1) ? lua_tostring(GV, -1) : "nil";
            lua_pop(GV, 1);
            return v;
        };
        std::printf("  predicted: pistol on %s every %d, chaingun on %s, fists on %s\n",
                    s("pistol_on").c_str(), (int)num(GV, "pistol_every"),
                    s("chaingun_on").c_str(), s("fists_on").c_str());
        CHECK(s("pistol_on") == "primary");
        CHECK(num(GV, "pistol_every") > 0);
        CHECK(s("chaingun_on") == "primary_held");
        CHECK(s("fists_on") == "swing");
        // The super shotgun's reload counts: it cannot fire again until it ends.
        CHECK(num(GV, "ssg_every") > num(GV, "ssg_fire"));
    }

    // A player is keyed by who they are, not by the slot their body is in.
    if (!run(GV, "local p = dnp.players()[1]\n"
                 "key = weapons.key_of(p)\n"
                 "start = weapons.get(key).current\n", "reading the player")) return 1;
    lua_getglobal(GV, "key");
    const std::string key = lua_tostring(GV, -1) ? lua_tostring(GV, -1) : "";
    lua_pop(GV, 1);
    std::printf("  the player is '%s' to the game\n", key.c_str());
    CHECK(key == "p7");

    // ------------------------------------------------- a shot, across the VMs
    //
    // A zombieman in front of the player, placed by the LEVEL -- its record and
    // its hit points are the level's. The pistol is the game's.
    if (!run(LV, "target = mobs.spawn('zombieman', vec3.new(fx.frac(40,48), fx.int(0), fx.int(0)))\n"
                 "function target_hp() local r = mobs.live[target:index()]; return r and r.hp or -1 end\n"
                 "hp0 = target_hp()\n", "placing the target")) return 1;
    const double hp0 = num(LV, "hp0");
    CHECK(hp0 > 0);

    u32 tick = 1000;
    const auto press_pistol = [&]() {
        // Up, then down for a tick, then up: one press of a semi-automatic.
        for (int t = 0; t < 30; ++t) {
            proto::InputFrame f{};
            f.buttons = (t == 1) ? proto::button::ATTACK_PRIMARY : 0;
            world.set_input(player, f);
            const std::string step = "local p = dnp.players()[1]\n"
                                     "weapons.tick_player(p, weapons.key_of(p), " + std::to_string(tick++) + ")\n";
            if (!run(GV, step.c_str(), "stepping the weapon")) return;
        }
    };

    press_pistol();
    if (!run(LV, "hp1 = target_hp()", "reading the target")) return 1;
    if (!run(GV, "bullets1 = weapons.get(key).ammo.bullets", "reading the ammunition")) return 1;
    std::printf("  one pistol shot from the game: %d -> %d hit points, %d bullets left\n",
                (int)hp0, (int)num(LV, "hp1"), (int)num(GV, "bullets1"));
    CHECK(num(LV, "hp1") < hp0);
    CHECK(num(GV, "bullets1") == 49);

    // CONTROL: the level unreachable -- the player in no level at all. The
    // weapon still fires as far as the game knows (the bullet is spent), and
    // nothing is hurt, because the shooting is the level's.
    cross.level_reachable = false;
    press_pistol();
    cross.level_reachable = true;
    if (!run(LV, "hp2 = target_hp()", "reading the target")) return 1;
    if (!run(GV, "bullets2 = weapons.get(key).ammo.bullets", "reading the ammunition")) return 1;
    std::printf("  CONTROL, no level to fire in: %d hit points, %d bullets\n",
                (int)num(LV, "hp2"), (int)num(GV, "bullets2"));
    CHECK(num(LV, "hp2") == num(LV, "hp1"));
    CHECK(num(GV, "bullets2") == 48);

    // A ROCKET: fired by the game, flown by the level -- its flight loop, its
    // impact and its blast are the mob runtime's, so the missile is in the
    // level's table of things in flight.
    if (!run(GV, "local inv = weapons.get(key)\n"
                 "inv.have.rocketlauncher = true; inv.current = 'rocketlauncher'; inv.ammo.rockets = 5\n",
             "arming the rocket launcher")) return 1;
    if (!run(LV, "local n = 0; for _ in pairs(mobs.flying) do n = n + 1 end; flying0 = n", "counting missiles")) return 1;
    for (int t = 0; t < 30; ++t) {
        proto::InputFrame f{};
        f.buttons = (t == 1) ? proto::button::ATTACK_PRIMARY : 0;
        world.set_input(player, f);
        const std::string step = "local p = dnp.players()[1]\n"
                                 "weapons.tick_player(p, key, " + std::to_string(tick++) + ")\n";
        if (!run(GV, step.c_str(), "stepping the launcher")) return 1;
    }
    if (!run(LV, "local n = 0; for _ in pairs(mobs.flying) do n = n + 1 end; flying1 = n", "counting missiles")) return 1;
    if (!run(GV, "rockets1 = weapons.get(key).ammo.rockets", "reading the ammunition")) return 1;
    std::printf("  a rocket from the game: %d in flight in the level (was %d), %d rockets left\n",
                (int)num(LV, "flying1"), (int)num(LV, "flying0"), (int)num(GV, "rockets1"));
    CHECK(num(LV, "flying1") == num(LV, "flying0") + 1);
    CHECK(num(GV, "rockets1") == 4);

    // ------------------------------------------------- a swing, the other way
    //
    // The level's on_attack asks the game what the weapon in the player's
    // hands costs. Fists swing; a pistol does not.
    if (!run(GV, "weapons.get(key).have.fists = true; weapons.get(key).current = 'fists'",
             "selecting fists")) return 1;
    if (!run(LV, "local p = dnp.players()[1]\n"
                 "has = dnp.game_has('weapon_swing')\n"
                 "local sw = dnp.call_game('weapon_swing', p)\n"
                 "fist_damage = sw and sw.damage or -1\n", "asking the game")) return 1;
    std::printf("  the level asked the game: fists cost %d\n", (int)num(LV, "fist_damage"));
    CHECK(truthy(LV, "has"));
    CHECK(num(LV, "fist_damage") > 0);
    if (!run(GV, "weapons.get(key).current = 'pistol'", "selecting the pistol")) return 1;
    if (!run(LV, "local p = dnp.players()[1]\n"
                 "pistol_swing = dnp.call_game('weapon_swing', p)\n", "asking the game")) return 1;
    CHECK(!truthy(LV, "pistol_swing"));
    // CONTROL: a name the game does not export is "nobody handles it".
    if (!run(LV, "nobody = dnp.game_has('no_such_export')", "asking for nothing")) return 1;
    CHECK(!truthy(LV, "nobody"));

    // And the weapon's swing shape reached the player's parameters from the
    // game, through the game's own tagged effects.
    if (!run(GV, "weapons.get(key).current = 'fists'\n"
                 "local p = dnp.players()[1]\n"
                 "weapons.tick_player(p, key, 5000)\n", "drawing the fists")) return 1;
    {
        const physics::MoveParams want = effects.compute(player);
        std::printf("  fists drawn by the game: attack_enabled=%d\n", want.attack_enabled ? 1 : 0);
        CHECK(want.attack_enabled);
    }
    if (!run(GV, "weapons.get(key).current = 'pistol'\n"
                 "local p = dnp.players()[1]\n"
                 "weapons.tick_player(p, key, 5001)\n", "drawing the pistol")) return 1;
    {
        const physics::MoveParams want = effects.compute(player);
        std::printf("  pistol drawn by the game: attack_enabled=%d\n", want.attack_enabled ? 1 : 0);
        CHECK(!want.attack_enabled);
    }

    // ------------------------------------------------ the values that cross
    //
    // Handles, fx, vec3, nested tables and a nil in the middle of the results
    // come back as themselves; a function is refused by name and the call
    // gives nothing rather than nonsense.
    if (!run(LV, "dnp.export('echo', function(...) return ... end)", "exporting")) return 1;
    if (!run(GV, "local p = dnp.players()[1]\n"
                 "-- the export is called with the player first, then what was passed\n"
                 "local who, a, b, c, d = dnp.call_level(p, 'echo', p, nil, { x = fx.frac(1, 3), v = vec3.new(1, 2, 3) }, 'four')\n"
                 "same_handle = (who == p and a == p)\n"
                 "nil_kept = (b == nil)\n"
                 "fx_exact = c ~= nil and c.x == fx.frac(1, 3) and c.v.z == fx.int(3)\n"
                 "fourth = d\n"
                 "refused = select('#', dnp.call_level(p, 'echo', function() end))\n",
             "copying values")) return 1;
    CHECK(truthy(GV, "same_handle"));
    CHECK(truthy(GV, "nil_kept"));
    CHECK(truthy(GV, "fx_exact"));
    lua_getglobal(GV, "fourth");
    CHECK(lua_tostring(GV, -1) != nullptr && std::string(lua_tostring(GV, -1)) == "four");
    lua_pop(GV, 1);
    CHECK(num(GV, "refused") == 0);

    // ----------------------------------------- a neighbour's monster (region)
    //
    // In a region every level's monsters share one world, so a shot fired in
    // level 0 can reach a monster level 1 keeps the health of. Level 0's mob
    // runtime does not know it; the hit is handed to the level that spawned it.
    {
        static u32 owner[sim::MAX_ENTITIES];
        for (u32 i = 0; i < sim::MAX_ENTITIES; ++i) owner[i] = 0xFFFFFFFFu;
        static server::lua::WorldContext nb_ctx;
        nb_ctx = level_ctx;
        nb_ctx.level = 1;
        nb_ctx.owner = owner;
        level_ctx.owner = owner;   // level 0 sees the same table
        static server::lua::Vm nb_vm;
        if (!nb_vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
        server::lua::open_world(nb_vm.state(), &nb_ctx);
        lua_State* NV = nb_vm.state();
        cross.neighbour = NV;
        for (const char* s : {"maps/scripts/mob_vocab.lua", "maps/scripts/mobs.lua",
                              "maps/mobs/doom_monsters.lua"}) {
            if (!nb_vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, nb_vm.last_error()); return 1; }
        }
        if (!run(NV, "theirs = mobs.spawn('zombieman', vec3.new(fx.int(-3), fx.int(0), fx.int(0)))\n"
                     "function theirs_hp() local r = mobs.live[theirs:index()]; return r and r.hp or -1 end\n"
                     "nb0 = theirs_hp(); theirs_index = theirs:index()\n", "the neighbour's monster")) return 1;
        const u32 theirs = static_cast<u32>(num(NV, "theirs_index"));
        CHECK(owner[theirs] == 1);
        const std::string hit = "local t = dnp.entity(" + std::to_string(theirs) + ")\n"
                                "mobs.hurt(t, 7, nil)\n";
        if (!run(LV, hit.c_str(), "hurting it from level 0")) return 1;
        if (!run(NV, "nb1 = theirs_hp()", "reading it")) return 1;
        std::printf("  level 0 hit level 1's monster: %d -> %d, kept by level 1\n",
                    (int)num(NV, "nb0"), (int)num(NV, "nb1"));
        CHECK(num(NV, "nb1") == num(NV, "nb0") - 7);
        // CONTROL: the owner out of reach -- the hit is not handed over, and
        // level 1's record of the monster does not move.
        cross.level_reachable = false;
        if (!run(LV, hit.c_str(), "hurting it with nobody to hand it to")) return 1;
        cross.level_reachable = true;
        if (!run(NV, "nb2 = theirs_hp()", "reading it")) return 1;
        CHECK(num(NV, "nb2") == num(NV, "nb1"));
        level_ctx.owner = nullptr;
    }

    // -------------------------------------------- what the game keeps, saved
    std::string saved, err;
    CHECK(server::lua::persist_player_capture(GV, game_ctx, player, saved, err));
    CHECK(saved.find("bullets") != std::string::npos);
    if (!run(GV, "weapons.forget(key)", "forgetting the player")) return 1;
    CHECK(server::lua::persist_player_restore(GV, game_ctx, player, saved, err));
    if (!run(GV, "bullets3 = weapons.get(key).ammo.bullets", "reading the ammunition")) return 1;
    CHECK(num(GV, "bullets3") == 48);

    // ================================================================
    // GRAPHS CALL ACROSS (docs/GRAPHS.md, `call` / `on_call`): a level's graph
    // calls the game's with a value; the game's adds one and calls back into
    // the level the player is in. Both ends are graphs.lua, in two VMs.
    std::printf("-- graphs call between the level and the game --\n");
    for (const char* f : {"maps/scripts/trigger_vocab.lua", "maps/scripts/triggers.lua",
                          "maps/scripts/graph_vocab.lua", "maps/scripts/graphs.lua"}) {
        if (!level_vm.run_file(f, f) || !game_vm.run_file(f, f)) {
            std::printf("  FAIL loading %s\n", f); return 1;
        }
    }
    CHECK(run(LV, R"LUA(
      gl_ok = graphs.load({ graph = 1, name = "lvl", vars = { back = 0 },
        nodes = {
          { id = "t",  type = "event", event = "on_touch" },
          { id = "up", type = "call", to = "game", name = "from_level", value = 7 },
          { id = "in", type = "on_call", name = "from_game" },
          { id = "s",  type = "set_var", var = "back" } },
        links = { { "t.then", "up.in" }, { "in.then", "s.in" }, { "in.value", "s.value" } } }, "lvl")
    )LUA", "the level's graph"));
    CHECK(run(GV, R"LUA(
      gg_ok = graphs.load({ graph = 1, name = "game", vars = { got = 0 },
        nodes = {
          { id = "in",   type = "on_call", name = "from_level" },
          { id = "s",    type = "set_var", var = "got" },
          { id = "one",  type = "literal", value = 1 },
          { id = "plus", type = "math", op = "+" },
          { id = "down", type = "call", to = "level", name = "from_game" } },
        links = { { "in.then", "s.in" }, { "in.value", "s.value" }, { "s.then", "down.in" },
                  { "in.value", "plus.a" }, { "one.value", "plus.b" },
                  { "plus.value", "down.value" }, { "in.entity", "down.entity" } } }, "game")
    )LUA", "the game's graph"));
    CHECK(truthy(LV, "gl_ok") && truthy(GV, "gg_ok"));
    CHECK(run(LV, "graphs.fire('on_touch', { entity = dnp.players()[1] })", "touching"));
    CHECK(run(GV, "x_got = graphs.var('game', 'got')", "reading the game"));
    CHECK(run(LV, "x_back = graphs.var('lvl', 'back')", "reading the level"));
    std::printf("  the game heard %g from the level; the level heard %g back\n",
                num(GV, "x_got"), num(LV, "x_back"));
    CHECK(num(GV, "x_got") == 7);
    CHECK(num(LV, "x_back") == 8);

    // ================================================================
    // A GAME GRAPH USES WHAT THE GAME HAS (graph_vocab.game): a level's verb
    // or event is refused at load in the game VM, by node; the same graph
    // loads in a level (CONTROL); a game-OK verb runs.
    std::printf("-- a game graph is refused a level's verbs and events --\n");
    CHECK(run(GV, R"LUA(
      local function door() return { graph = 1, name = "door",
        nodes = { { id = "t", type = "event", event = "on_touch" },
                  { id = "m", type = "action", a = "sector_move", sector = 0, surface = "floor", offset = "1", speed = "1" } },
        links = { { "t.then", "m.in" } } } end
      g_door = graphs.load(door(), "door")
      g_cross = graphs.load({ graph = 1, name = "cross",
        nodes = { { id = "c", type = "event", event = "on_cross" },
                  { id = "n", type = "action", a = "counter_add", counter = "crossed" } },
        links = { { "c.then", "n.in" } } }, "cross")
      g_fine = graphs.load({ graph = 1, name = "fine",
        nodes = { { id = "t", type = "event", event = "on_touch" },
                  { id = "n", type = "action", a = "counter_add", counter = "game_touched" } },
        links = { { "t.then", "n.in" } } }, "fine")
      graphs.fire("on_touch", { entity = dnp.players()[1] })
      g_count = triggers.counter("game_touched")
    )LUA", "game graphs"));
    CHECK(run(LV, R"LUA(
      l_door = graphs.load({ graph = 1, name = "door_l",
        nodes = { { id = "t", type = "event", event = "on_touch" },
                  { id = "m", type = "action", a = "sector_move", sector = 0, surface = "floor", offset = "1", speed = "1" } },
        links = { { "t.then", "m.in" } } }, "door_l")
    )LUA", "the same in a level"));
    CHECK(!truthy(GV, "g_door"));          // sector_move: a level's verb
    CHECK(!truthy(GV, "g_cross"));         // on_cross never reaches the game
    CHECK(truthy(GV, "g_fine"));           // counter_add, on_touch: the game's
    CHECK(num(GV, "g_count") == 1);        // and it ran
    CHECK(truthy(LV, "l_door"));           // CONTROL: in a level, the same graph loads
    // CONTROL: the game's lists are checked against the trigger vocabulary.
    CHECK(run(GV, "table.insert(graph_vocab.game.actions, 'open_portal')\n"
                  "g_wrong = #graphs.check_vocabulary()\n"
                  "table.remove(graph_vocab.game.actions)", "a bad game list"));
    CHECK(num(GV, "g_wrong") == 1);

    // ================================================================
    // THE WORLD'S NODES ON MONSTERS: spawn one by name where the player is,
    // hurt it through the mob runtime (so it feels it), read its health, kill
    // it. And a sector node in a game graph is refused (level_only).
    std::printf("-- the world's nodes on a monster --\n");
    CHECK(run(LV, R"LUA(
      d_ok = graphs.load({ graph = 1, name = "deaths",
        nodes = {
          { id = "h",  type = "when_hurt", what = "monsters", kind = "zombieman" },
          { id = "hc", type = "action", a = "counter_add", counter = "z_hurt" },
          { id = "d",  type = "when_dies", kind = "zombieman" },
          { id = "dc", type = "action", a = "counter_add", counter = "z_died" },
          { id = "i",  type = "when_dies", kind = "imp" },
          { id = "ic", type = "action", a = "counter_add", counter = "imp_died" } },
        links = { { "h.then", "hc.in" }, { "d.then", "dc.in" }, { "i.then", "ic.in" } } }, "deaths")
    )LUA", "a graph listening for deaths"));
    CHECK(run(LV, R"LUA(
      m_ok = graphs.load({ graph = 1, name = "mon", vars = { hp_after = -1, hp_dead = -1 },
        nodes = {
          { id = "t",    type = "event", event = "on_touch" },
          { id = "make", type = "spawn", what = "zombieman" },
          { id = "hit",  type = "damage", amount = 5 },
          { id = "hp",   type = "health_of" },
          { id = "keep", type = "set_var", var = "hp_after" },
          { id = "end",  type = "kill" },
          { id = "hp2",  type = "health_of" },
          { id = "keep2",type = "set_var", var = "hp_dead" } },
        links = { { "t.then", "make.in" }, { "make.then", "hit.in" }, { "hit.then", "keep.in" },
                  { "keep.then", "end.in" }, { "end.then", "keep2.in" },
                  { "make.entity", "hit.entity" }, { "make.entity", "hp.entity" }, { "hp.value", "keep.value" },
                  { "make.entity", "end.entity" }, { "make.entity", "hp2.entity" }, { "hp2.value", "keep2.value" } } }, "mon")
      graphs.fire("on_touch", { entity = dnp.players()[1] })
      m_after = graphs.var("mon", "hp_after"); m_dead = graphs.var("mon", "hp_dead")
    )LUA", "a monster's graph"));
    std::printf("  spawned a zombieman: after 5 damage its health read %g; killed, %g\n",
                num(LV, "m_after"), num(LV, "m_dead"));
    CHECK(truthy(LV, "m_ok"));
    CHECK(run(LV, "z_hurt = triggers.counter('z_hurt'); z_died = triggers.counter('z_died'); imp_died = triggers.counter('imp_died')", "reading deaths"));
    std::printf("  when_hurt fired %g, when_dies %g (imp: %g)\n", num(LV, "z_hurt"), num(LV, "z_died"), num(LV, "imp_died"));
    CHECK(truthy(LV, "d_ok"));
    CHECK(num(LV, "z_hurt") >= 1);       // the 5 damage, and the killing blow
    CHECK(num(LV, "z_died") == 1);
    CHECK(num(LV, "imp_died") == 0);     // CONTROL: filtered by kind
    CHECK(num(LV, "m_after") == 15);       // DOOM's zombieman has 20
    CHECK(num(LV, "m_dead") <= 0);
    CHECK(run(GV, R"LUA(
      g_doorw = graphs.load({ graph = 1, name = "gdoor",
        nodes = { { id = "t", type = "event", event = "on_touch" }, { id = "d", type = "open_door", sector = 0 } },
        links = { { "t.then", "d.in" } } }, "gdoor")
      g_pushw = graphs.load({ graph = 1, name = "gpush",
        nodes = { { id = "t", type = "event", event = "on_touch" }, { id = "p", type = "push", direction = "up", up = 3 } },
        links = { { "t.then", "p.in" } } }, "gpush")
    )LUA", "game graphs with world nodes"));
    CHECK(!truthy(GV, "g_doorw"));         // a door is a level's
    CHECK(truthy(GV, "g_pushw"));          // CONTROL: a push is anybody's

    // ================================================================
    // A DEATH ON ANY MAP, HEARD BY THE GAME: when_dies and when_hurt in a game
    // graph, told by the level's mob runtime -- so one graph of the module's
    // answers a player's death everywhere (a death screen), a map drawn later
    // included.
    std::printf("-- the game hears a player die in a level --\n");
    CHECK(run(GV, R"LUA(
      gd_ok, gd_err = graphs.load({ graph = 1, name = "gdeath", vars = { died = 0, hurt = 0 },
        nodes = {
          { id = "d", type = "when_dies", what = "players" },
          { id = "dc", type = "add_var", var = "died", value = 1 },
          { id = "h", type = "when_hurt", what = "players" },
          { id = "hc", type = "add_var", var = "hurt", value = 1 } },
        links = { { "d.then", "dc.in" }, { "h.then", "hc.in" } } }, "gdeath")
      gd_err = gd_err and table.concat(gd_err, "; ") or ""
    )LUA", "a game graph listening for deaths"));
    std::printf("  a game graph with when_dies/when_hurt players: %s %s\n",
                truthy(GV, "gd_ok") ? "loaded" : "refused", str(GV, "gd_err").c_str());
    CHECK(truthy(GV, "gd_ok"));
    // A monster killed in the level: not a player, so not heard (CONTROL that
    // the filter is the graph's and not "every death").
    CHECK(run(LV, R"LUA(
      local p = dnp.players()[1]
      local pp = p:pos()
      local z = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(9), pp.y, pp.z))
      mobs.hurt(z, 1000, nil)
      p:set_hp(100)
      mobs.hurt(p, 30, nil)
      mobs.hurt(p, 1000, nil)
    )LUA", "a player hurt, then killed, in the level"));
    CHECK(run(GV, "gd_died = graphs.var('gdeath', 'died'); gd_hurt = graphs.var('gdeath', 'hurt')", "reading the game"));
    std::printf("  the game heard %g death(s) and %g hurt(s)\n", num(GV, "gd_died"), num(GV, "gd_hurt"));
    CHECK(num(GV, "gd_died") == 1);        // the player's, not the zombieman's
    CHECK(num(GV, "gd_hurt") == 2);        // the 30, and the killing blow

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
