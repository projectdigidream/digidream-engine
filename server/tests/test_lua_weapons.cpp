//
// Weapons -- and the swept collider generally -- against the real bindings and a
// real world.
//
// The collider is shared: a player's melee and a monster's claw are the same
// engine facility, driven from the same replicated parameters, and the last
// section here is about the monster half. It lives in this file rather than in
// a third one because the room, the catalogue, the VM and the server loop below
// are the harness both need, and a second copy of a harness is a second harness
// that drifts.
//
// WHY THIS EXISTS AT ALL. maps/scripts/weapons.lua shipped six weapons, an
// inventory, four ammunition pools, an empty-weapon fallback and a UI panel,
// and had NO TEST OF ANY KIND. Nothing asserted that a pickup granted anything,
// that a refused one stayed on the floor, that ammunition was debited, or that
// a cooldown gated the rate. The rewrite into a phase machine is a much larger
// surface than the flat table it replaces, so it arrives with the tests the
// flat table never had.
//
// AND WHY IT IS HERE RATHER THAN IN tools/weaponcheck. That harness runs under
// LuaJIT against a fake world, which is the right venue for "is this table
// well formed" and the wrong one for everything below: four bugs in the mob
// phase came from the fake being more generous than the engine. Only the real
// bindings can answer what a hitscan actually hits, whether an effect reaches
// the parameter table, or whether one press produces one damage event or two.
//
// THE DOUBLE-FIRE is the headline. Before this phase, ATTACK_PRIMARY drove the
// engine's swept collider AND weapons.lua's own edge detection, so firing a
// shotgun also swung an invisible fist and two independent damage paths ran off
// one press. The test for it is written so that FORCING THE OLD BEHAVIOUR
// reproduces the bug -- a fix asserted only in its fixed state proves nothing.
//
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "dnp/sim/world.hpp"
#include "dnp/physics/params_table.hpp"
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

// Enough of the catalogue to spawn a target and a few pickups. What is under
// test is the weapon runtime, not whether a file parses.
struct WeaponProps final : public server::lua::PropCatalogueView {
    struct Entry { const char* name; u16 kind; fx radius; fx height; fx speed; };
    static constexpr fx du(i64 n) { return fx::from_frac(n, 48); }

    Entry table[7] = {
        {"zombieman",  20, du(20), du(56), du(8 * 35)},
        // The one monster in the shipped bestiary whose bite SWEEPS, so the
        // monster half of this file has something real to drive.
        {"demon",      22, du(30), du(56), du(10 * 35)},
        {"medikit",    12, du(10), du(16), fx::zero()},
        {"w_shotgun",  82, du(10), du(16), fx::zero()},
        {"ammo_shells", 8, du(10), du(16), fx::zero()},
        {"rocket",     53, du(11), du(16), du(20 * 35)},
        {"arachno_plasma", 52, du(13), du(8), du(25 * 35)},
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

// One open room. Nothing in it but what a test puts there, for the same reason
// the shooting range's is empty: a corridor cannot tell you whether a short shot
// hit a wall or a neighbour.
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

static lua_State* L = nullptr;
static server::lua::Vm vm;

// Run a snippet, reporting where it failed. Returns false on a raise, which
// every caller treats as fatal: a test that carries on after its setup raised
// measures a world that was never built.
static bool run(const char* src, const char* what) {
    if (luaL_dostring(L, src) != 0) {
        std::printf("  FAIL %s: %s\n", what, lua_tostring(L, -1));
        ++g_fails;
        return false;
    }
    return true;
}

static double num(const char* global) {
    lua_getglobal(L, global);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
    lua_pop(L, 1);
    return v;
}

static std::string str(const char* global) {
    lua_getglobal(L, global);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 1);
    return out;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && chdir(argv[1]) != 0) { std::printf("  FAIL chdir\n"); return 1; }
    std::printf("== weapons ==\n");

    static Room room;
    static sim::World world;
    world.reset();
    world.set_map(&room.view);
    world.set_map_storage(&room.store);

    const u32 player = world.spawn(fx3{fx::zero(), fx::zero(), fx::zero()});
    CHECK(world.agent(player).kind == 0);

    static WeaponProps props;
    static i32 hp[sim::MAX_AGENTS];
    for (u32 i = 0; i < sim::MAX_AGENTS; ++i) hp[i] = 100;

    static server::lua::CommandQueue commands;
    static server::lua::EffectStack effects;
    static server::lua::WorldContext ctx;
    ctx.world = &world;
    ctx.map_store = &room.store;
    ctx.props = &props;
    ctx.hp = hp;
    ctx.commands = &commands;
    ctx.effects = &effects;
    ctx.rng = Rng(20260927u, 7);

    server::lua::VmConfig cfg;
    if (!vm.open(cfg)) { std::printf("  FAIL vm.open\n"); return 1; }
    server::lua::open_world(vm.state(), &ctx);
    L = vm.state();

    // ------------------------------------------------------------- loading
    //
    // The vocabulary, the mob runtime that owns the shared actions, Doom's
    // monsters for their missile table, the weapon runtime, and Doom's weapons.
    // In that order, and the order is load-bearing: weapons.lua raises outright
    // if the vocabulary is not already a global, and a weapon naming the
    // `rocket` missile can only be checked once the mob file has declared it.
    const char* scripts[] = {
        "maps/scripts/mob_vocab.lua",
        "maps/scripts/mobs.lua",
        "maps/mobs/doom_monsters.lua",
        "maps/scripts/weapon_vocab.lua",
        "maps/scripts/weapons.lua",
        "modules/doom2/maps/weapons_doom2.lua",
    };
    for (const char* s : scripts) {
        if (!vm.run_file(s, s)) {
            std::printf("  FAIL loading %s: %s\n", s, vm.last_error());
            return 1;
        }
    }

    // EVERY definition loaded clean. This is the assertion the whole validating
    // loader exists to make possible, and it covers the vocabulary cross-check
    // in both directions, every field name, every phase name, every ammunition
    // name and every missile name in Doom's table.
    if (!run("loaded_errors = weapons.load_errors()", "reading load errors")) return 1;
    std::printf("  Doom's weapons loaded with %d complaints\n", (int)num("loaded_errors"));
    CHECK(num("loaded_errors") == 0);

    // A player exists and starts armed the way Doom starts you.
    if (!run("local p = dnp.players()[1]\n"
             "start_inv = weapons.get(p:index())\n"
             "start_weapon = start_inv.current\n"
             "start_bullets = start_inv.ammo.bullets\n"
             "start_shells = start_inv.ammo.shells\n", "reading the inventory")) return 1;
    std::printf("  starts with %s, %d bullets, %d shells\n",
                str("start_weapon").c_str(), (int)num("start_bullets"),
                (int)num("start_shells"));
    CHECK(str("start_weapon") == "pistol");
    CHECK(num("start_bullets") == 50);
    CHECK(num("start_shells") == 0);

    // ------------------------------------------------------ the double-fire
    //
    // THE CENTRAL CLAIM: one press of ATTACK_PRIMARY produces ONE damage event.
    //
    // Counted by replacing mobs.hurt, which is where every damage path in this
    // engine ends -- the swept collider's on_attack calls it and the hitscan
    // action calls it -- so a counter there cannot miss one path while catching
    // the other. That is the entire reason it is hooked HERE and not at either
    // call site.
    if (!run(
        "-- SHOTS are counted through mobs.on_shot, the observability hook the\n"
        "-- hitscan action already calls once per ray. It is the honest place:\n"
        "-- wrapping mobs.hurt does NOT work, because acts.hitscan calls the\n"
        "-- file-local `hurt` rather than the module field, so a counter there\n"
        "-- silently sees nothing -- which is how this test first reported that\n"
        "-- no weapon fires at all.\n"
        "shot_log = {}\n"
        "mobs.on_shot = function(m, target, reached)\n"
        "  shot_log[#shot_log + 1] = (target ~= nil) and 1 or 0\n"
        "end\n"
        "-- DAMAGE is measured where it lands, on the target's own hit points.\n"
        "-- Both paths this test is about -- the engine's swept collider through\n"
        "-- on_attack, and the script's hitscan -- end in the same `hurt`, so a\n"
        "-- number read here cannot catch one and miss the other.\n"
        "-- Directly in front of the player and well inside a fist's reach, so\n"
        "-- neither a swing nor a bullet is excused by geometry.\n"
        "target = mobs.spawn('zombieman', vec3.new(fx.frac(40,48), fx.int(0), fx.int(0)))\n"
        "function target_hp()\n"
        "  local rec = mobs.live[target:index()]\n"
        "  return rec and rec.hp or -1\n"
        "end\n",
        "installing the counters")) return 1;
    if (!run("spawned_hp = target_hp()", "reading the target")) return 1;
    std::printf("  a zombieman in front of the player, %d hit points\n",
                (int)num("spawned_hp"));
    CHECK(num("spawned_hp") > 0);

    // The harness that drives a press. `buttons` is what the engine reports the
    // player holding, so this pushes a real input rather than calling fire()
    // directly -- which is the difference between testing the weapon and
    // testing the function under it.

    // BUTTONS ARE DRIVEN THROUGH THE ENGINE, not stubbed.
    //
    // `e:buttons()` reads World::input, which is where a real session's input
    // frames land, so writing there is exactly what a connected player does.
    // Stubbing the handle's method instead would have tested the weapon against
    // a button source no game has -- the mistake this file's own header is
    // about, one level down.
    const auto set_buttons = [&](u16 mask) {
        proto::InputFrame f{};
        f.buttons = mask;
        world.set_input(player, f);
    };

    // Hold ATTACK_PRIMARY for `held` of `ticks` ticks and answer how many
    // damage events came out. The weapon is selected first, and the tick number
    // advances, because a phase machine measures elapsed time against it.
    u32 fake_tick = 1000;
    const auto press_for = [&](int ticks, int held, const char* weapon) {
        std::string setup = "local p = dnp.players()[1]\n"
                            "local inv = weapons.get(p:index())\n"
                            "inv.have['" + std::string(weapon) + "'] = true\n"
                            "inv.current = '" + std::string(weapon) + "'\n"
                            "shot_log = {}\n";
        if (!run(setup.c_str(), "selecting a weapon")) return -1;
        // ONE TICK WITH THE BUTTON UP FIRST.
        //
        // Edge detection is state, and it persists across these calls: without
        // this the button is still down from the previous press as far as the
        // runtime is concerned, `button_pressed` never sees a rising edge, and
        // a semi-automatic weapon reports zero shots. That is CORRECT
        // behaviour -- holding fire through a weapon switch should not fire the
        // new weapon -- and an artefact of a harness that switches weapons
        // without ever letting go.
        set_buttons(0);
        {
            const std::string idle =
                "local p = dnp.players()[1]\n"
                "weapons.tick_player(p, p:index(), " + std::to_string(fake_tick) + ")\n"
                "shot_log = {}\n";
            if (!run(idle.c_str(), "releasing the button")) return -1;
        }
        for (int t = 1; t <= ticks; ++t) {
            set_buttons(t <= held ? proto::button::ATTACK_PRIMARY : 0);
            const std::string step =
                "local p = dnp.players()[1]\n"
                "weapons.tick_player(p, p:index(), " + std::to_string(fake_tick + t) + ")\n";
            if (!run(step.c_str(), "stepping the weapon")) return -1;
        }
        fake_tick += static_cast<u32>(ticks) + 100;
        set_buttons(0);
        if (!run("shot_events = #shot_log", "counting shots")) return -1;
        return static_cast<int>(num("shot_events"));
    };

    // FISTS: a swing, no sequence at all. The damage comes from the engine's
    // swept collider through on_attack, which is the path that had no lookup.
    //
    // Driven through the WORLD, not through weapons.tick_player, because the
    // swing lives in the shared controller: the parameters say whether it may
    // start and the controller starts it.
    if (!run("local p = dnp.players()[1]\n"
             "weapons.get(p:index()).have.fists = true\n"
             "weapons.get(p:index()).current = 'fists'\n"
             "weapons.tick_player(p, p:index(), dnp.tick())\n"
             "fists_enabled = p:param('attack_enabled').raw\n"
             "fists_reach = p:param('attack_reach').raw\n", "selecting fists")) return 1;

    // The effect stack is pumped by main.cpp, not by a test, so the parameters
    // are recomputed here the way the server recomputes them.
    effects.set_base(player, world.params(player));
    // EVERY AGENT, not just the player. The effect pump in main.cpp runs over
    // the session list; a monster's parameters are put in the stack by the spawn
    // binding and recomputed by the same loop, so pumping only the player left
    // every monster on the engine's DEFAULTS -- which is how a demon's declared
    // 1.67-unit reach read back as 2.00 and a zombieman with no swing at all
    // read back as able to swing.
    const auto recompute = [&] {
        // Field by field through the parameter table's own accessors, exactly as
        // main.cpp:1168 does, because that is the only way the Bool and U8
        // fields round-trip correctly.
        for (u32 a = 0; a < world.agent_high_water(); ++a) {
            if (!world.agent(a).active) continue;
            const physics::MoveParams want = effects.compute(a);
            physics::MoveParams& live = world.params_mut(a);
            for (u16 f = 0; f < physics::PARAM_COUNT; ++f) {
                physics::param_set_raw(live, f, physics::param_get_raw(want, f));
            }
        }
        return effects.compute(player);
    };
    physics::MoveParams p_fists = recompute();
    std::printf("  fists: attack_enabled=%d reach=%lld raw\n",
                p_fists.attack_enabled ? 1 : 0,
                static_cast<long long>(p_fists.attack_reach.raw));
    CHECK(p_fists.attack_enabled);
    CHECK(p_fists.attack_reach.raw > 0);

    // SHOTGUN: no `swing` at all, so the collider must be switched OFF.
    if (!run("local p = dnp.players()[1]\n"
             "weapons.get(p:index()).have.shotgun = true\n"
             "weapons.get(p:index()).current = 'shotgun'\n"
             "weapons.get(p:index()).ammo.shells = 20\n"
             "weapons.tick_player(p, p:index(), dnp.tick())\n", "selecting the shotgun"))
        return 1;
    physics::MoveParams p_gun = recompute();
    std::printf("  shotgun: attack_enabled=%d\n", p_gun.attack_enabled ? 1 : 0);
    // THE FIX. A ranged weapon does not swing, and because the parameter is
    // replicated the CLIENT predicts the absence rather than predicting a swing
    // the server then denies.
    CHECK(!p_gun.attack_enabled);

    // And the second, independent gate: even with the collider forced live, a
    // weapon with no `swing` costs nothing.
    if (!run("local p = dnp.players()[1]\n"
             "swing_with_shotgun = weapons.swing_damage(p)\n"
             "weapons.get(p:index()).current = 'fists'\n"
             "swing_with_fists = weapons.swing_damage(p)\n"
             "weapons.get(p:index()).current = 'shotgun'\n", "asking swing_damage"))
        return 1;
    std::printf("  swing_damage: fists=%d shotgun=%s\n",
                (int)num("swing_with_fists"),
                num("swing_with_shotgun") < 0 ? "nil" : "a number");
    CHECK(num("swing_with_fists") == 10);
    // CONTROL for the pair: if this were also a number, the "two independent
    // gates" claim would be one gate and a comment.
    CHECK(num("swing_with_shotgun") < 0);

    // ------------------------------------------------------- one press, one shot
    //
    // The shotgun fires SEVEN rays, so seven damage events is one shot. What is
    // under test is that holding the button does not produce a second shot
    // before the cooldown, which is what `ready` transitioning on
    // `button_pressed` means.
    if (!run("weapons.get(dnp.players()[1]:index()).ammo.shells = 20",
             "loading shells")) return 1;
    // The input really reaches the script. Asserted rather than assumed: every
    // firing-rate claim below rests on it, and a harness whose button never
    // arrives reports "no weapon fires" -- which looks exactly like a broken
    // runtime and is not.
    set_buttons(proto::button::ATTACK_PRIMARY);
    if (!run("probe_buttons = dnp.players()[1]:buttons()", "probing buttons")) return 1;
    CHECK(num("probe_buttons") == proto::button::ATTACK_PRIMARY);
    set_buttons(0);
    if (!run("probe_released = dnp.players()[1]:buttons()", "probing release")) return 1;
    CHECK(num("probe_released") == 0);
    // The shotgun fires SEVEN rays, so seven damage events is ONE shot.
    const int shotgun_events = press_for(20, 20, "shotgun");
    std::printf("  shotgun held 20 ticks: %d damage events (%d shots of 7)\n",
                shotgun_events, shotgun_events / 7);
    // 20 ticks is inside Doom's 30-tic cooldown, which is 51 engine ticks.
    CHECK(shotgun_events == 7);

    // CONTROL: the chaingun, whose ONLY difference is `button_held` where the
    // shotgun has `button_pressed`, fires repeatedly over the same hold.
    // Without it, "a held button is one shot" passes against a weapon that
    // simply cannot fire twice ever -- and the two weapons differ by one word.
    if (!run("weapons.get(dnp.players()[1]:index()).ammo.bullets = 200",
             "loading bullets")) return 1;
    const int auto_events = press_for(40, 40, "chaingun");
    std::printf("  chaingun held 40 ticks: %d shots\n", auto_events);
    CHECK(auto_events > 1);

    // And the pistol over the same 40 ticks is semi-automatic: ONE.
    if (!run("weapons.get(dnp.players()[1]:index()).ammo.bullets = 200",
             "loading bullets")) return 1;
    const int semi_events = press_for(40, 40, "pistol");
    std::printf("  pistol held 40 ticks: %d shots\n", semi_events);
    CHECK(semi_events == 1);

    // ------------------------------------------- the pellets really do scatter
    //
    // THE ASSERTION THAT WAS MISSING. Everything above proves the shotgun casts
    // SEVEN rays; nothing proved they go anywhere different. A `count` that
    // worked and a `spread` that was dropped would have passed every one of
    // them -- and that is not hypothetical, it is precisely the bug
    // maps/scripts/mobs.lua had for its whole life, where every monster
    // declared a spread and the call site passed two arguments.
    //
    // Recorded at the binding, through mobs.on_shot's `reached`: seven pellets
    // down one line reach the same distance, and seven down different lines do
    // not.
    if (!run("shot_reach = {}\n"
             "mobs.on_shot = function(m, target, reached)\n"
             "  shot_reach[#shot_reach + 1] = reached and reached.raw or -1\n"
             "end\n"
             "weapons.get(dnp.players()[1]:index()).ammo.shells = 20\n",
             "recording reach")) return 1;
    press_for(20, 20, "shotgun");
    if (!run("local seen, n = {}, 0\n"
             "for _, r in ipairs(shot_reach) do\n"
             "  if not seen[r] then seen[r] = true; n = n + 1 end\n"
             "end\n"
             "pellets = #shot_reach\n"
             "pellet_lines = n\n", "counting the pellets")) return 1;
    std::printf("  one shotgun blast: %d pellets reaching %d distinct distances\n",
                (int)num("pellets"), (int)num("pellet_lines"));
    CHECK(num("pellets") == 7);
    // MORE THAN ONE. Not seven: two pellets can land on the same wall at the
    // same distance by geometry, and demanding seven would be asserting the
    // room's shape rather than the weapon's.
    CHECK(num("pellet_lines") > 1);

    // CONTROL, and it is the point: the CHAINGUN declares a spread too but fires
    // ONE ray, so its shots must reach one distance each. If this also came back
    // plural, the count above would be measuring something other than the
    // scatter -- the player turning, say.
    if (!run("shot_reach = {}\n"
             "weapons.get(dnp.players()[1]:index()).ammo.bullets = 200\n",
             "recording again")) return 1;
    press_for(3, 3, "chaingun");
    if (!run("single_pellets = #shot_reach", "counting one shot")) return 1;
    std::printf("  CONTROL: a chaingun burst put out %d ray(s) per shot\n",
                (int)num("single_pellets"));
    CHECK(num("single_pellets") >= 1);
    CHECK(num("single_pellets") < 7);
    if (!run("mobs.on_shot = function(m, target, reached)\n"
             "  shot_log[#shot_log + 1] = (target ~= nil) and 1 or 0\n"
             "end\n", "restoring the shot counter")) return 1;

    // ------------------------------------------------------------- ammunition
    if (!run("weapons.get(dnp.players()[1]:index()).ammo.shells = 10",
             "setting shells")) return 1;
    press_for(20, 20, "shotgun");
    if (!run("shells_after = weapons.get(dnp.players()[1]:index()).ammo.shells",
             "debiting ammunition")) return 1;
    std::printf("  shotgun fired once from 10 shells -> %d\n", (int)num("shells_after"));
    CHECK(num("shells_after") == 9);

    // Running dry falls back to the BEST usable weapon, not the first.
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "inv.have = { fists = true, pistol = true, shotgun = true }\n"
             "inv.ammo.shells = 0\n"
             "inv.ammo.bullets = 50\n", "arming for the fallback")) return 1;
    press_for(10, 10, "shotgun");
    if (!run("fallback = weapons.get(dnp.players()[1]:index()).current",
             "the empty-weapon fallback")) return 1;
    std::printf("  shotgun run dry falls back to %s\n", str("fallback").c_str());
    // The PISTOL, order 3, not the fists at order 1. A `first usable` search
    // reaches the fists, which is a player punching something at range with
    // fifty bullets in their pocket.
    CHECK(str("fallback") == "pistol");

    // ------------------------------------------- an empty weapon is still yours
    //
    // THE PANEL BUG. `no_ammo` alone routed a weapon to its `dry` phase while it
    // was merely being HELD, and `dry` auto-switches -- so choosing an empty
    // shotgun from the weapon panel put the pistol back in your hands on the
    // very next tick, every time, and an empty weapon could not be carried at
    // all. Reported as the panel malfunctioning, which is exactly what it looks
    // like from the outside.
    //
    // The fix is a transition that requires the button AND the empty pool, which
    // the vocabulary could not say until `when` learned to take a list.
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "inv.have = { fists = true, pistol = true, shotgun = true }\n"
             "inv.ammo.shells = 0\n"
             "inv.ammo.bullets = 50\n"
             "inv.current = 'shotgun'\n"
             "weapons.rearm_player(dnp.players()[1]:index())\n", "choosing an empty gun"))
        return 1;
    set_buttons(0);
    for (int i = 0; i < 10; ++i) {
        const std::string step =
            "local p = dnp.players()[1]\n"
            "weapons.tick_player(p, p:index(), " + std::to_string(++fake_tick) + ")\n";
        if (!run(step.c_str(), "carrying it")) return 1;
    }
    if (!run("carried = weapons.get(dnp.players()[1]:index()).current",
             "reading what is held")) return 1;
    std::printf("  an empty shotgun, carried for 10 ticks, is still a %s\n",
                str("carried").c_str());
    CHECK(str("carried") == "shotgun");

    // CONTROL: pulling the trigger on it DOES put something usable in your
    // hands. Without this the line above passes against a build where the
    // auto-switch was simply deleted rather than narrowed to a press.
    const int dry_events = press_for(10, 10, "shotgun");
    if (!run("after_dry = weapons.get(dnp.players()[1]:index()).current",
             "reading what is held now")) return 1;
    std::printf("  CONTROL: pulling its trigger switches to %s (%d shots fired)\n",
                str("after_dry").c_str(), dry_events);
    CHECK(str("after_dry") == "pistol");
    CHECK(dry_events == 0);

    // -------------------------------------------------------------- pickups
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have = { fists = true, pistol = true }\n"
             "inv.ammo.shells = 0\n"
             "local gun = dnp.spawn_prop('w_shotgun', p:pos())\n"
             "weapons.pickup(p, gun)\n"
             "got_shotgun = inv.have.shotgun and 1 or 0\n"
             "got_shells = inv.ammo.shells\n"
             "gun_gone = gun:valid() and 0 or 1\n", "a weapon pickup")) return 1;
    std::printf("  walking over w_shotgun: have=%d shells=%d taken=%d\n",
                (int)num("got_shotgun"), (int)num("got_shells"), (int)num("gun_gone"));
    CHECK(num("got_shotgun") == 1);
    CHECK(num("got_shells") == 8);
    CHECK(num("gun_gone") == 1);

    // A REFUSED PICKUP STAYS ON THE FLOOR, asserted for the first time. The
    // "once" semantics were in the runtime from the beginning and nothing ever
    // checked them, so a change that consumed a medikit at full health would
    // have shipped silently.
    if (!run("local p = dnp.players()[1]\n"
             "p:set_hp(100)\n"
             "local kit = dnp.spawn_prop('medikit', p:pos())\n"
             "weapons.pickup(p, kit)\n"
             "kit_left = kit:valid() and 1 or 0\n"
             "hp_full = p:hp()\n"
             "-- CONTROL: hurt, the SAME medikit is taken. Without this, 'a\n"
             "-- refused pickup stays' passes against a pickup that never works.\n"
             "p:set_hp(50)\n"
             "weapons.pickup(p, kit)\n"
             "kit_taken = kit:valid() and 0 or 1\n"
             "hp_healed = p:hp()\n", "the refused pickup")) return 1;
    std::printf("  medikit at full health: left on the floor=%d (hp %d); "
                "at 50: taken=%d (hp %d)\n",
                (int)num("kit_left"), (int)num("hp_full"),
                (int)num("kit_taken"), (int)num("hp_healed"));
    CHECK(num("kit_left") == 1);
    CHECK(num("hp_full") == 100);
    CHECK(num("kit_taken") == 1);
    CHECK(num("hp_healed") == 75);

    // ------------------------------------------------------- the bfg says so
    //
    // A pickup that silently does nothing is indistinguishable from a bug,
    // which is the entire reason `not_yet` is a field rather than an absence.
    if (!run("bfg_declared = weapons.pickups.w_bfg ~= nil and "
             "                weapons.pickups.w_bfg.not_yet or false\n",
             "the BFG")) return 1;
    std::printf("  w_bfg is declared as not-yet: %s\n", str("bfg_declared").c_str());
    CHECK(str("bfg_declared") == "BFG");

    // ==================================================================
    // Everything below runs the SERVER'S OWN LOOP rather than poking the
    // weapon runtime directly: the hooks, then the queued commands, then the
    // effect pump, then the world. Up to here the tests could call
    // weapons.tick_player because what they measured was the weapon; the swept
    // collider, a missile in flight and a recoil impulse are all things only
    // the engine does, and a harness that skipped it would be measuring a
    // sequence no game runs.
    u32 server_tick_n = 0;
    const auto server_tick = [&](u16 buttons) {
        set_buttons(buttons);
        vm.call_hook("on_tick", world.tick());
        commands.apply(world, [](u32) {});
        recompute();
        world.step();
        // SWINGS THAT CONNECTED, dispatched exactly as main.cpp:1119-1132 does.
        // The engine reports a contact and stops; what it COSTS is the script's,
        // through this hook. Without this the collider swept correctly and
        // nothing ever took damage, which reads as the swing not working.
        const sim::AttackLog& hits = world.attacks();
        for (u32 i = 0; i < hits.count(); ++i) {
            const sim::AttackEvent& e = hits.at(i);
            for (u32 h = 0; h < vm.hook_count("on_attack"); ++h) {
                if (!vm.begin_hook_at("on_attack", h)) continue;
                server::lua::push_entity(vm.state(), ctx, e.attacker);
                if (sim::World::is_prop(e.target)) {
                    server::lua::push_prop(vm.state(), ctx, e.target);
                } else {
                    server::lua::push_entity(vm.state(), ctx, e.target);
                }
                vm.end_hook("on_attack", world.tick(), 2);
            }
        }
        ++server_tick_n;
    };

    // ---------------------------------------------- the collider really gates
    //
    // `attack_enabled` is asserted above as a PARAMETER. This asserts the
    // consequence: that the engine's swept collider does not start. They are
    // different claims -- a parameter the controller ignored would satisfy the
    // first and not the second, and that is precisely the bug shape here.
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have = { fists = true, pistol = true, shotgun = true }\n"
             "inv.ammo.shells = 40\n"
             "inv.current = 'fists'\n", "arming with fists")) return 1;
    server_tick(0);
    u8 fists_attack_left = 0;
    for (int i = 0; i < 6; ++i) {
        server_tick(proto::button::ATTACK_PRIMARY);
        fists_attack_left = std::max(fists_attack_left, world.agent(player).mv.attack_left);
    }
    // And the shotgun over the identical sequence -- but ONLY once the fists'
    // swing has finished. A swing is 16 ticks long by design, so switching
    // weapons two ticks after firing one leaves the collider still live and the
    // shotgun inherits a countdown it did not start. That read as the fix not
    // working, and it is the same class of mistake as measuring a per-tick log
    // after the loop: the number was real and it was not the number asked for.
    if (!run("weapons.get(dnp.players()[1]:index()).current = 'shotgun'",
             "switching to the shotgun")) return 1;
    int drain = 0;
    while (world.agent(player).mv.attack_left != 0 && drain < 120) {
        server_tick(0);
        ++drain;
    }
    std::printf("  the fists' swing took %d ticks to finish\n", drain);
    CHECK(world.agent(player).mv.attack_left == 0);
    server_tick(0);
    u8 gun_attack_left = 0;
    for (int i = 0; i < 6; ++i) {
        server_tick(proto::button::ATTACK_PRIMARY);
        gun_attack_left = std::max(gun_attack_left, world.agent(player).mv.attack_left);
    }
    std::printf("  swept collider over 6 ticks of fire: fists reached %d, "
                "shotgun reached %d\n", (int)fists_attack_left, (int)gun_attack_left);
    // THE DOUBLE-FIRE, collapsed. Firing a shotgun no longer swings a fist.
    CHECK(gun_attack_left == 0);
    // CONTROL, and without it the line above passes against a build where the
    // collider never starts for anything -- which would be a far worse bug and
    // would look identical from here.
    CHECK(fists_attack_left > 0);

    // ------------------------------------------------------------ the recoil
    //
    // `impulse` in a sequence really moves the body. Measured through the
    // command queue and the world, because that is the only thing that can
    // answer it: the action queues an AddImpulse and the engine spends it.
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have.supershotgun = true\n"
             "inv.current = 'supershotgun'\n"
             "inv.ammo.shells = 40\n"
             "p:set_velocity(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n",
             "arming the super shotgun")) return 1;
    server_tick(0);
    server_tick(0);
    if (!run("local p = dnp.players()[1]\n"
             "recoil_before = p:pos().x.raw\n", "reading the position")) return 1;
    server_tick(proto::button::ATTACK_PRIMARY);
    server_tick(0);
    server_tick(0);
    if (!run("local p = dnp.players()[1]\n"
             "recoil_after = p:pos().x.raw\n", "reading the position again")) return 1;
    const double moved = num("recoil_before") - num("recoil_after");
    std::printf("  super shotgun recoil pushed the player %.3f units backwards\n",
                moved / 4294967296.0);
    // BACKWARDS, away from the facing, which is +x. A `towards = "facing"`
    // that read the sign the other way would move them forwards, so the
    // DIRECTION is asserted and not just the magnitude.
    CHECK(moved > 0);

    // ------------------------------------------------------------- a missile
    //
    // The rocket launcher, which could not exist before: `rocket` has been a
    // complete missile since the cyberdemon, and the only thing missing was a
    // way for a PLAYER to fire one.
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have.rocketlauncher = true\n"
             "inv.current = 'rocketlauncher'\n"
             "inv.ammo.rockets = 10\n"
             "rockets_before = inv.ammo.rockets\n"
             "-- A FRESH target. The one spawned at the top has been shot at by\n"
             "-- every weapon above and is long dead, and measuring a rocket\n"
             "-- against a corpse's hit points reports no damage from a rocket\n"
             "-- that worked perfectly.\n"
             "target = mobs.spawn('zombieman', vec3.new(fx.frac(40,48), fx.int(0), fx.int(0)))\n"
             "hp_before_rocket = target_hp()\n", "arming the rocket launcher")) return 1;
    CHECK(num("hp_before_rocket") > 0);
    server_tick(0);
    server_tick(0);
    server_tick(proto::button::ATTACK_PRIMARY);
    for (int i = 0; i < 40; ++i) server_tick(0);
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "rockets_after = inv.ammo.rockets\n"
             "hp_after_rocket = target_hp()\n", "reading the result")) return 1;
    std::printf("  rocket launcher: %d -> %d rockets; the target went %d -> %d\n",
                (int)num("rockets_before"), (int)num("rockets_after"),
                (int)num("hp_before_rocket"), (int)num("hp_after_rocket"));
    CHECK(num("rockets_after") == num("rockets_before") - 1);
    // It FLEW AND ARRIVED. The target is a little under a unit away and the
    // blast radius is 128 map units, so a rocket that never left the tube and
    // one that detonated correctly both reduce this -- which is why the recoil
    // test above asserts the launcher's own impulse separately.
    CHECK(num("hp_after_rocket") < num("hp_before_rocket"));

    // ================================================================
    // THE THREE THINGS THE FLAT TABLE COULD NOT EXPRESS.
    //
    // Doom needs none of them, so nothing in modules/doom2 exercises them and
    // a green run there would prove only that Doom still works. These weapons
    // exist to use the parts of the vocabulary Doom does not -- the same
    // argument maps/mobvocab.map makes for the mob vocabulary, which found
    // three real bugs no Doom fixture could.
    static const char* kPlatformer = R"LUA(
      weapons.load {
        weapons = {
          -- CHARGE. `charge` has no `ticks`, so it WAITS; `held_for` above
          -- `button_released` is what makes a long hold take the first branch.
          chargegun = {
            label = "Charge Gun", order = 20, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = "button_held", to = "charge" } } },
              charge = { anim = 5, transitions = {
                { when = "held_for", ticks = 30, to = "fire_big" },
                { when = "button_released", to = "fire_small" } } },
              fire_small = { anim = 2, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "power", value = 1 } } },
              fire_big = { anim = 2, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "power", value = 9 } } },
            },
            vars = { power = 0 },
          },

          -- COMBO. `recover`'s `ticks` ARE the window, and a second press
          -- inside it reaches a different attack. A press one tick late finds
          -- the phase gone, which is what a window means.
          combogun = {
            label = "Combo Blade", order = 21, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = "button_pressed", to = "hit1" } } },
              hit1 = { anim = 2, ticks = 6, next = "recover", sequence = {
                { at = 0, action = "add_var", var = "step", value = 1 } } },
              recover = { anim = 3, ticks = 12, next = "ready", transitions = {
                { when = "button_pressed", to = "hit2" } } },
              hit2 = { anim = 4, ticks = 6, next = "ready", sequence = {
                { at = 0, action = "add_var", var = "step", value = 10 } } },
            },
            vars = { step = 0 },
          },

          -- ALT-FIRE, which is one word: a transition naming the other button.
          altgun = {
            label = "Two Barrels", order = 22, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = "button_pressed", button = "secondary", to = "alt" },
                { when = "button_pressed", button = "primary", to = "main" } } },
              main = { anim = 2, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "barrel", value = 1 } } },
              alt = { anim = 6, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "barrel", value = 2 } } },
            },
            vars = { barrel = 0 },
          },
        },
      }
      platformer_errors = weapons.load_errors()
    )LUA";
    if (!run(kPlatformer, "loading the platformer weapons")) return 1;
    std::printf("  three vocabulary weapons loaded with %d complaints\n",
                (int)num("platformer_errors"));
    CHECK(num("platformer_errors") == 0);

    // Drive one weapon through a written button pattern, one character a tick:
    // '1' primary down, '2' secondary down, '.' nothing. Spelling the input out
    // is what makes "one tick after the window" a thing that can be written.
    u32 pat_tick = 90000;
    const auto pattern = [&](const char* weapon, const char* pat) {
        const std::string setup =
            "local p = dnp.players()[1]\n"
            "local inv = weapons.get(p:index())\n"
            "inv.have['" + std::string(weapon) + "'] = true\n"
            "inv.current = '" + std::string(weapon) + "'\n"
            // FROM SCRATCH. `vars` survive as long as the weapon is held --
            // which is exactly what a combo counter is for -- so two runs of the
            // same weapon accumulate, and the second run's reading is then a
            // difference rather than a value. A real property, measured wrongly.
            "weapons.rearm_player(p:index())\n";
        if (!run(setup.c_str(), "selecting a vocabulary weapon")) return false;
        for (const char* c = pat; *c; ++c) {
            u16 mask = 0;
            if (*c == '1') mask = proto::button::ATTACK_PRIMARY;
            else if (*c == '2') mask = proto::button::ATTACK_SECONDARY;
            set_buttons(mask);
            const std::string step =
                "local p = dnp.players()[1]\n"
                "weapons.tick_player(p, p:index(), " + std::to_string(++pat_tick) + ")\n";
            if (!run(step.c_str(), "stepping a vocabulary weapon")) return false;
        }
        set_buttons(0);
        pat_tick += 200;
        return true;
    };
    const auto var_of = [&](const char* v) {
        const std::string q =
            "local vars = weapons.vars_of(dnp.players()[1]:index())\n"
            "probe_var = vars and vars['" + std::string(v) + "'] or -1\n";
        run(q.c_str(), "reading a weapon variable");
        return static_cast<int>(num("probe_var"));
    };

    // A SHORT hold and a LONG one, through the identical phase graph.
    if (!pattern("chargegun", ".1111111111.")) return 1;          // 10 ticks
    const int short_charge = var_of("power");
    if (!pattern("chargegun", std::string(1, '.').append(40, '1').append(".").c_str()))
        return 1;                                                  // 40 ticks
    const int long_charge = var_of("power");
    std::printf("  charge: 10 ticks held -> power %d; 40 ticks held -> power %d\n",
                short_charge, long_charge);
    CHECK(short_charge == 1);
    // CONTROL. If these were equal, `held_for` would be decorative and the
    // charge would be one branch with two names.
    CHECK(long_charge == 9);

    // A second press INSIDE the recovery window continues the combo.
    // hit1 is 6 ticks, recover is 12, so a press at tick 10 is inside.
    if (!pattern("combogun", ".1........1........................")) return 1;
    const int combo_in = var_of("step");
    // And one AFTER it does not: tick 20 is past 6 + 12.
    if (!pattern("combogun", ".1..................1..............")) return 1;
    const int combo_out = var_of("step");
    std::printf("  combo: a second press at tick 10 -> step %d; at tick 20 -> step %d\n",
                combo_in, combo_out);
    // 1 from hit1, then 10 from hit2.
    CHECK(combo_in == 11);
    // CONTROL, and it is the whole claim: a press one window late is an
    // ordinary first attack, twice. Without it "there is a window" is unproven
    // -- a machine with no window at all gives 11 both times.
    CHECK(combo_out == 2);

    // The other button reaches the other phase.
    if (!pattern("altgun", ".1...")) return 1;
    const int barrel_main = var_of("barrel");
    if (!pattern("altgun", ".2...")) return 1;
    const int barrel_alt = var_of("barrel");
    std::printf("  alt-fire: primary -> barrel %d; secondary -> barrel %d\n",
                barrel_main, barrel_alt);
    CHECK(barrel_main == 1);
    CHECK(barrel_alt == 2);

    // ================================================================
    // EVERY BAD TABLE IS REFUSED, and this is the assertion the validating
    // loader exists for. A loader that accepted everything would pass every
    // test above this line perfectly: Doom's weapons load, they fire, they take
    // ammunition, the pickups work. Nothing so far can tell a checker from a
    // shrug.
    //
    // Run against the REAL loader, not a linter's copy of it, so what is proven
    // is that the server refuses these rather than that something else would.
    struct Bad { const char* why; const char* lua; };
    static const Bad kBad[] = {
        {"a misspelled weapon field",
         "weapons.check_weapon('b1', { label = 'B', order = 90, dammage = 20,"
         "  phases = { ready = {} }, initial = 'ready' })"},
        {"a misspelled swing field",
         "weapons.check_weapon('b2', { label = 'B', order = 90,"
         "  swing = { damage = 5, raech = 2 },"
         "  phases = { ready = {} }, initial = 'ready' })"},
        {"a transition to a phase that does not exist",
         "weapons.check_weapon('b3', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { transitions = {"
         "    { when = 'always', to = 'nowhere' } } } } })"},
        {"an unknown action",
         "weapons.check_weapon('b4', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { sequence = { { at = 0, action = 'teleport' } } } } })"},
        {"an unknown condition",
         "weapons.check_weapon('b5', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { transitions = {"
         "    { when = 'player_is_bored', to = 'ready' } } } } })"},
        {"a missing required action argument",
         "weapons.check_weapon('b6', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { sequence = {"
         "    { at = 0, action = 'hitscan', damage = 5 } } } } })"},
        {"an enum value that is not one of the allowed ones",
         "weapons.check_weapon('b7', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { sequence = {"
         "    { at = 0, action = 'impulse', speed = '2', towards = 'sideways' }"
         "  } } } })"},
        {"an inexact fx literal, which means different things on different builds",
         "weapons.check_weapon('b8', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { sequence = {"
         "    { at = 0, action = 'hitscan', damage = 5, range = 0.1 } } } } })"},
        {"a variable nothing declares",
         "weapons.check_weapon('b9', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { sequence = {"
         "    { at = 0, action = 'add_var', var = 'heat', value = 1 } } } } })"},
        {"an `initial` naming a phase that does not exist",
         "weapons.check_weapon('b10', { label = 'B', order = 90, initial = 'nope',"
         "  phases = { ready = {}, other = {} } })"},
        {"several phases and no `initial`, where declaration order is not a thing "
         "a Lua table has",
         "weapons.check_weapon('b11', { label = 'B', order = 90,"
         "  phases = { ready = {}, other = {} } })"},
        {"a required weapon field left out",
         "weapons.check_weapon('b12', { order = 90, initial = 'ready',"
         "  phases = { ready = {} } })"},
    };
    std::printf("  --- every bad table must be refused ---\n");
    int refused = 0;
    for (const Bad& b : kBad) {
        if (!run("before_bad = weapons.load_errors()", "reading the error count")) return 1;
        const double before = num("before_bad");
        if (!run(b.lua, "checking a bad table")) return 1;
        if (!run("after_bad = weapons.load_errors()", "reading the error count")) return 1;
        const double after = num("after_bad");
        const bool caught = after > before;
        if (caught) ++refused;
        std::printf("  %s  %s\n", caught ? "refused " : "ACCEPTED", b.why);
        CHECK(caught);
    }
    std::printf("  --> %d of %d refused\n", refused, (int)(sizeof(kBad) / sizeof(kBad[0])));

    // And a GOOD table alongside them, so "it refuses everything" is not the
    // explanation for the block above. A checker that complained about every
    // input would satisfy all twelve.
    if (!run("local before = weapons.load_errors()\n"
             "weapons.check_weapon('good', { label = 'Good', order = 91,"
             "  initial = 'ready', vars = { heat = 0 },"
             "  swing = { damage = 5, reach = '1.5', ticks = 8 },"
             "  phases = {"
             "    ready = { anim = 0, transitions = {"
             "      { when = 'button_pressed', to = 'fire' } } },"
             "    fire = { anim = 2, ticks = 8, next = 'ready', sequence = {"
             "      { at = 0, action = 'add_var', var = 'heat', value = 1 },"
             "      { at = 2, action = 'hitscan', damage = 5, range = '20',"
             "        count = 3, spread = 10 } } } } })\n"
             "good_complaints = weapons.load_errors() - before\n",
             "checking a good table")) return 1;
    std::printf("  CONTROL: a well-formed weapon drew %d complaints\n",
                (int)num("good_complaints"));
    CHECK(num("good_complaints") == 0);

    // ------------------------------------------------- the panel is generated
    //
    // The read-out list used to be four names written into the runtime, beside
    // a separate table of four widget ids, beside a separate starting
    // inventory. A fifth ammunition kind could be declared, granted, counted
    // and capped -- and never appear on screen, because nothing tied the three
    // lists together.
    if (!run("local p = dnp.players()[1]\n"
             "local panel = weapons.panel_for(p:index())\n"
             "sliders = 0\n"
             "slider_ids = {}\n"
             "slider_labels = {}\n"
             "for _, w in ipairs(panel.widgets) do\n"
             "  if w.kind == 'slider' then\n"
             "    sliders = sliders + 1\n"
             "    slider_ids[#slider_ids + 1] = w.id\n"
             "    slider_labels[#slider_labels + 1] = w.label\n"
             "  end\n"
             "end\n"
             "ids_distinct = 1\n"
             "for i = 1, #slider_ids do for j = i + 1, #slider_ids do\n"
             "  if slider_ids[i] == slider_ids[j] then ids_distinct = 0 end\n"
             "end end\n"
             "label_list = table.concat(slider_labels, ', ')\n", "reading the panel"))
        return 1;
    std::printf("  the weapon panel shows %d ammunition read-outs: %s\n",
                (int)num("sliders"), str("label_list").c_str());
    CHECK(num("sliders") == 4);
    // Distinct ids, or ui_set updates the wrong counter. They are generated from
    // a base plus an index now, where they used to be four constants.
    CHECK(num("ids_distinct") == 1);
    // IN `order`, which is what the list is sorted by.
    CHECK(str("label_list") == "Bullets, Shells, Rockets, Cells");

    // CONTROL, and it is the whole claim: declare a FIFTH pool and it appears,
    // in its declared position, with an id of its own. Nothing in this file
    // knows the word 'plasma'.
    if (!run("weapons.load { ammo = { goo = { label = 'Goo', max = 9, order = 3 } } }\n"
             "local p = dnp.players()[1]\n"
             "local panel = weapons.panel_for(p:index())\n"
             "sliders2 = 0\n"
             "local names = {}\n"
             "for _, w in ipairs(panel.widgets) do\n"
             "  if w.kind == 'slider' then\n"
             "    sliders2 = sliders2 + 1; names[#names + 1] = w.label end\n"
             "end\n"
             "label_list2 = table.concat(names, ', ')\n", "adding a fifth pool"))
        return 1;
    std::printf("  after declaring a fifth pool: %s\n", str("label_list2").c_str());
    CHECK(num("sliders2") == 5);
    CHECK(str("label_list2") == "Bullets, Shells, Goo, Rockets, Cells");

    // ================================================================
    // PARITY. Three readers of one vocabulary: the runtime, the checked-in
    // JSON, and docs/WEAPONS.md. A name in one and not another is a feature
    // that either does not exist or cannot be found.

    // 1. The vocabulary and the runtime, both directions.
    if (!run("vocab_mismatches = weapons.check_vocabulary()", "checking parity")) return 1;
    std::printf("  the vocabulary and the runtime disagree about %d names\n",
                (int)num("vocab_mismatches"));
    CHECK(num("vocab_mismatches") == 0);

    // CONTROL, and it is the point: take one action OUT of the vocabulary and
    // the runtime must complain that it implements something nobody lists.
    // Without this, the check above passes against a comparison that looks at
    // nothing -- which is what it did until the tables stopped being captured at
    // load.
    if (!run("removed = table.remove(weapon_vocab.actions)\n"
             "vocab_after_removal = weapons.check_vocabulary()\n"
             "table.insert(weapon_vocab.actions, removed)\n"
             "vocab_restored = weapons.check_vocabulary()\n"
             "removed_name = removed.name\n", "the parity control")) return 1;
    std::printf("  removing `%s` from the vocabulary produced %d complaints; "
                "putting it back, %d\n", str("removed_name").c_str(),
                (int)num("vocab_after_removal"), (int)num("vocab_restored"));
    CHECK(num("vocab_after_removal") > 0);
    CHECK(num("vocab_restored") == 0);

    // 2. The checked-in JSON is byte-current. The editor reads it, so a stale
    // copy offers fields the loader refuses -- the exact failure the generator
    // exists to prevent.
    {
        std::string fresh;
        if (FILE* pipe = popen("./build/lua/trigger_vocab_json . "
                               "maps/scripts/weapon_vocab.lua weapon_vocab 2>/dev/null",
                               "r")) {
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) fresh.append(buf, n);
            pclose(pipe);
        }
        std::string on_disk;
        if (FILE* fp = std::fopen("maps/scripts/weapon_vocab.json", "rb")) {
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) on_disk.append(buf, n);
            std::fclose(fp);
        }
        if (fresh.empty()) {
            std::printf("  ..   no build/lua/trigger_vocab_json; the JSON is not compared\n");
        } else {
            std::printf("  weapon_vocab.json: %zu bytes on disk, %zu regenerated\n",
                        on_disk.size(), fresh.size());
            CHECK(!on_disk.empty());
            CHECK(fresh == on_disk);
        }
    }

    // 3. docs/WEAPONS.md documents exactly the vocabulary.
    {
        std::string doc;
        if (FILE* fp = std::fopen("docs/WEAPONS.md", "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) doc.append(buf, n);
            std::fclose(fp);
        }
        CHECK(!doc.empty());

        // The names come from the TABLE, enumerated in Lua. Parsing them out of
        // the document in C++ would make this a second reader of the document
        // rather than a comparison against the vocabulary.
        if (!run("local out = {}\n"
                 "for _, c in ipairs(weapon_vocab.conditions) do out[#out+1] = c.name end\n"
                 "for _, a in ipairs(weapon_vocab.actions)    do out[#out+1] = a.name end\n"
                 "doc_names = table.concat(out, '\\n')\n", "listing the names")) return 1;
        const std::string names = str("doc_names");
        std::vector<std::string> list;
        for (size_t at = 0; at <= names.size();) {
            const size_t nl = names.find('\n', at);
            const std::string one = names.substr(at, nl == std::string::npos
                                                     ? std::string::npos : nl - at);
            if (!one.empty()) list.push_back(one);
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        // The list itself is worth asserting: a bug that produced an EMPTY list
        // would make the loop below pass with nothing checked.
        std::printf("  docs/WEAPONS.md is checked against %zu vocabulary names\n",
                    list.size());
        CHECK(list.size() > 20);

        int undocumented = 0;
        for (const std::string& name : list) {
            if (doc.find("### `" + name + "`") == std::string::npos) {
                ++undocumented;
                std::printf("  FAIL docs/WEAPONS.md has no `%s` section\n", name.c_str());
            }
        }
        CHECK(undocumented == 0);

        // CONTROL: the detector can fail. A name that is NOT in the vocabulary
        // must not be found either -- without it a search matching everything
        // would pass the loop above in silence.
        CHECK(doc.find("### `reload_by_hand`") == std::string::npos);

        // And the other direction: a heading naming nothing.
        int stray = 0;
        size_t at = 0;
        while ((at = doc.find("### `", at)) != std::string::npos) {
            const size_t start = at + 5;
            const size_t end = doc.find('`', start);
            if (end == std::string::npos) break;
            const std::string name = doc.substr(start, end - start);
            bool known = false;
            for (const std::string& k : list) if (k == name) known = true;
            if (!known) {
                ++stray;
                std::printf("  FAIL docs/WEAPONS.md documents `%s`, which the "
                            "vocabulary does not list\n", name.c_str());
            }
            at = end;
        }
        CHECK(stray == 0);
    }

    // ================================================================
    // A MONSTER'S SWING, AND WHY IT IS WORTH HAVING.
    //
    // `melee` is an instant distance test: on the tick it runs, anything inside
    // range is hit and there is nothing the target can do. A swept collider
    // travels through an arc over several ticks, so stepping out of the way part
    // way through WORKS -- which for a monster whose only threat is reaching you
    // is the difference between a fight and a tax.
    //
    // That is the claim, and it is asserted as a pair: the same swing, against a
    // target that stays and a target that moves. One of them must connect and
    // the other must not, or the duration is decorative.
    if (!run("demon = mobs.spawn('demon', vec3.new(fx.int(-3), fx.int(0), fx.int(0)))\n"
             "demon_has_swing = (mobs.live[demon:index()].def.swing ~= nil) and 1 or 0\n"
             "demon_reach = mobs.live[demon:index()].def.swing.reach.raw\n"
             "demon_ticks = mobs.live[demon:index()].def.swing.ticks\n"
             "demon_damage = mobs.live[demon:index()].def.swing.damage\n",
             "spawning a demon")) return 1;
    std::printf("  a demon declares a swing: reach %.2f, live for %d ticks, %d damage\n",
                num("demon_reach") / 4294967296.0, (int)num("demon_ticks"),
                (int)num("demon_damage"));
    CHECK(num("demon_has_swing") == 1);
    // A duration is the whole point. One tick is an instant hit under another
    // name, and the engine re-reads zero as one.
    CHECK(num("demon_ticks") > 1);

    // The shape reached the REPLICATED parameter table, which is what makes a
    // monster's swing predicted by the same machinery a player's is.
    //
    // Pumped first. The spawn binding seeds the effect stack's base and the
    // server's loop recomputes from it; reading the live table before that has
    // happened gives the engine's DEFAULTS, which is how a declared 1.67-unit
    // reach first read back as 2.00.
    recompute();
    if (!run("local d = demon\n"
             "demon_enabled = d:param('attack_enabled').raw\n"
             "demon_param_reach = d:param('attack_reach').raw\n", "reading the demon"))
        return 1;
    std::printf("  and it is in the parameter table: enabled=%d reach=%.2f\n",
                (int)num("demon_enabled"), num("demon_param_reach") / 4294967296.0);
    CHECK(num("demon_enabled") != 0);
    CHECK(num("demon_param_reach") == num("demon_reach"));

    // WHERE IN THE SWEEP a hit lands, which is the thing a duration buys.
    //
    // The collider starts at `sweep_from` and travels to `sweep_to` over the
    // swing's life. So a target at the START of the arc is hit on the first
    // tick and a target at the END is hit only once the sweep arrives -- and
    // the gap between those two ticks IS the window a target has to move.
    //
    // Driven through mobs.acts.swing rather than by waiting for the state graph
    // to reach `melee`, because what is under test is the collider: routing a
    // monster across a room first would make a failure mean either thing.
    //
    // The demon sits at the origin facing +x. `bearing` is degrees off that.
    const auto bite = [&](int bearing_deg, int dodge_at) {
        const double rad = bearing_deg * 3.14159265358979 / 180.0;
        const double dist = 1.15;                  // inside reach, outside the body
        const long long vx = static_cast<long long>(std::llround(std::cos(rad) * dist * 4294967296.0));
        const long long vy = static_cast<long long>(std::llround(std::sin(rad) * dist * 4294967296.0));
        const std::string setup =
            "demon:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
            "demon:set_input(fx.int(0), fx.int(0), 0, 0)\n"
            // DORMANT, so the only swing in this measurement is the one this
            // test starts. Left awake the demon sees the victim, runs its own
            // `melee` state and bites on its own schedule -- which showed up as
            // the same bearing being hit on tick 10 in one run and tick 7 in the
            // next, a number that looked like sweep geometry and was the AI.
            "mobs.acts.sleep(mobs.live[demon:index()])\n"
            "victim = mobs.spawn('zombieman', vec3.new(fx.raw(" +
            std::to_string(vx) + "), fx.raw(" + std::to_string(vy) + "), fx.int(0)))\n"
            "victim:set_solid(false)\n"
            "hit_tick = -1\n"
            "victim_hp0 = mobs.live[victim:index()].hp\n";
        if (!run(setup.c_str(), "placing a victim")) return -2;
        recompute();
        // Let the bodies settle before the swing, so neither is still falling to
        // the floor when the collider is evaluated.
        for (int i = 0; i < 4; ++i) server_tick(0);
        if (!run("mobs.acts.swing(mobs.live[demon:index()])", "starting a bite")) return -2;
        int landed = -1;
        for (int i = 0; i < 20; ++i) {
            if (i == dodge_at) {
                // OUT OF THE ARC. Teleported rather than walked, because what is
                // being tested is whether the collider re-checks where things
                // are -- a walk would also be testing the mob's own pathing.
                if (!run("victim:teleport(vec3.new(fx.int(30), fx.int(30), fx.int(0)))",
                         "dodging")) return -2;
            }
            server_tick(0);
            if (landed < 0) {
                if (!run("local r = mobs.live[victim:index()]\n"
                         "victim_now = r and r.hp or victim_hp0", "reading hp")) return -2;
                if (num("victim_now") < num("victim_hp0")) landed = i;
            }
        }
        run("if victim:valid() then victim:despawn() end", "clearing");
        return landed;
    };

    // A DIAGNOSTIC SWEEP, printed before anything is asserted, because the
    // number this section is about is the table and not the pass.
    std::printf("  --- which bearings a demon's bite reaches, and on which tick ---\n");
    for (int b = -90; b <= 90; b += 15) {
        const int t = bite(b, -1);
        std::printf("  %+4d deg: %s\n", b,
                    t < 0 ? "never reached" : ("tick " + std::to_string(t)).c_str());
    }

    // Two bearings through the identical swing. The arc runs from -60 to +60
    // degrees, so a target behind the start is reached first and one at the far
    // end last.
    const int hit_at_start = bite(-55, -1);
    const int hit_at_end   = bite(55, -1);
    std::printf("  a demon's bite reaches a target at -55 deg on tick %d, "
                "and one at +55 deg on tick %d\n", hit_at_start, hit_at_end);
    // BOTH are reached. If either were never hit, the dodge below would pass for
    // the wrong reason.
    CHECK(hit_at_start >= 0);
    CHECK(hit_at_end >= 0);
    // AND THE COLLIDER TRAVELS. This is the claim: the far side of the arc is
    // reached LATER than the near side, which is what makes the attack a volume
    // moving through time rather than a distance test wearing a duration.
    CHECK(hit_at_end > hit_at_start);

    // So a target at the far end can step away before the sweep arrives.
    const int dodged = bite(55, hit_at_end);
    std::printf("  and one that steps away on tick %d is missed (%s)\n",
                hit_at_end, dodged < 0 ? "never hit" : "still hit");
    // CANNOT BE DONE against `melee`, which is the whole reason this exists.
    CHECK(dodged < 0);

    // And moving away a tick too LATE does not help, or "there is a window"
    // would be a claim about nothing.
    const int too_late = bite(55, hit_at_end + 1);
    std::printf("  stepping away one tick later is too late (hit on tick %d)\n",
                too_late);
    CHECK(too_late >= 0);

    // TWICE, WHILE STANDING STILL, which is the case that nearly shipped broken.
    //
    // `press` ORs a bit into the world's input frame, and the native mob loop
    // only writes an input frame when something IT knows about changed. An
    // attack state sets `move = "stop"`, so a biting monster's axes are
    // constant -- nothing would have written the button back to zero,
    // `attack_held` would have stayed true, and the second swing would never
    // start. It would have worked for a chasing monster and failed for a
    // standing one.
    //
    // Driven through the mob runtime's own on_tick, because that is what
    // releases the press; calling acts.swing twice without it would prove
    // nothing about the thing under test.
    if (!run("demon:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
             "demon:set_input(fx.int(0), fx.int(0), 0, 0)\n"
             "mobs.acts.sleep(mobs.live[demon:index()])\n"
             "-- IN FRONT. A body faces +x by default and the arc runs from\n"
             "-- -120 to +120 degrees off that, so a victim at -x is BEHIND the\n"
             "-- demon and would be missed by a swing that worked perfectly.\n"
             "twice_victim = mobs.spawn('zombieman',"
             "  vec3.new(fx.frac(50,48), fx.int(0), fx.int(0)))\n"
             "twice_victim:set_solid(false)\n"
             "twice_hp0 = mobs.live[twice_victim:index()].hp\n", "placing a victim"))
        return 1;
    recompute();
    for (int i = 0; i < 4; ++i) server_tick(0);

    int swings_landed = 0;
    for (int swing = 0; swing < 3; ++swing) {
        // TOPPED UP FIRST. A demon bites for 20 and a zombieman has 20 hit
        // points, so it dies on the first swing and the next two have nothing
        // left to hit -- which reads exactly like the stuck button this test is
        // about, and is not it.
        if (!run("mobs.live[twice_victim:index()].hp = 200\n"
                 "twice_before = mobs.live[twice_victim:index()].hp\n"
                 "mobs.acts.swing(mobs.live[demon:index()])", "swinging")) return 1;
        const double before = num("twice_before");
        // Long enough for the collider to live and die, with the axes never
        // changing -- which is the condition under test.
        for (int i = 0; i < 20; ++i) server_tick(0);
        if (!run("local r = mobs.live[twice_victim:index()]\n"
                 "twice_hp = r and r.hp or -1", "reading the victim")) return 1;
        const double now = num("twice_hp");
        if (now >= 0 && now < before) ++swings_landed;
    }
    std::printf("  a standing demon told to bite three times landed %d of them\n",
                swings_landed);
    // ALL THREE. Verified to be a real gate by disabling release_presses in
    // mobs.lua and re-running: it lands 1 of 3, which is the bug exactly --
    // the first press works and every one after it is swallowed. That control
    // cannot live in this file, because it needs the runtime broken, so the
    // number it produced is recorded here instead.
    CHECK(swings_landed == 3);
    run("if twice_victim:valid() then twice_victim:despawn() end", "clearing");

    // And a monster with NO declared swing does not swing, by the same two gates
    // a ranged weapon is held to: the parameter says no, and on_attack would
    // answer nothing even if it did not.
    if (!run("nosw = mobs.spawn('zombieman', vec3.new(fx.int(-8), fx.int(0), fx.int(0)))\n"
             "nosw:set_input(fx.int(0), fx.int(0), 0, 0)\n"
             "nosw_declared = (mobs.live[nosw:index()].def.swing ~= nil) and 1 or 0\n"
             "nosw_spawned = 1\n", "a mob with no swing")) return 1;
    recompute();
    if (!run("nosw_enabled = nosw:param('attack_enabled').raw", "reading its collider"))
        return 1;
    std::printf("  a zombieman declares no swing (%d), and swinging is refused\n",
                (int)num("nosw_declared"));
    CHECK(num("nosw_declared") == 0);

    // It is stopped by the ACTION and by on_attack, not by a parameter, and that
    // is deliberate: writing `attack_enabled = 0` into every monster in the world
    // would cost a parameter write per monster to defend against something
    // nothing does -- the native loop only ever presses JUMP. So the gate is
    // asserted where it actually is.
    // IN FRONT OF IT and well inside a demon's reach, so "nothing happened"
    // means the swing was refused rather than that the geometry saved us.
    if (!run("nosw_victim = mobs.spawn('zombieman',"
             "  vec3.new(fx.int(-8) + fx.frac(50,48), fx.int(0), fx.int(0)))\n"
             "nosw_hp0 = mobs.live[nosw_victim:index()].hp\n"
             "-- The action, on a mob that declares nothing. It must do nothing\n"
             "-- rather than start a collider with the engine's defaults.\n"
             "mobs.acts.swing(mobs.live[nosw:index()])\n", "swinging with nothing"))
        return 1;
    for (int i = 0; i < 20; ++i) server_tick(0);
    if (!run("local r = mobs.live[nosw_victim:index()]\n"
             "nosw_hp1 = r and r.hp or nosw_hp0", "reading the victim")) return 1;
    std::printf("  a zombieman told to swing did nothing: %d -> %d hit points\n",
                (int)num("nosw_hp0"), (int)num("nosw_hp1"));
    CHECK(num("nosw_hp1") == num("nosw_hp0"));

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
