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
                          "modules/doom2/maps/weapons_doom2.lua"}) {
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

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
