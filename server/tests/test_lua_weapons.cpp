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

    Entry table[9] = {
        {"zombieman",  20, du(20), du(56), du(8 * 35)},
        // The one monster in the shipped bestiary whose bite SWEEPS, so the
        // monster half of this file has something real to drive.
        {"demon",      22, du(30), du(56), du(10 * 35)},
        {"medikit",    12, du(10), du(16), fx::zero()},
        {"w_shotgun",  82, du(10), du(16), fx::zero()},
        {"ammo_shells", 8, du(10), du(16), fx::zero()},
        {"rocket",     53, du(11), du(16), du(20 * 35)},
        {"arachno_plasma", 52, du(13), du(8), du(25 * 35)},
        // What Doom's humans drop (doom_monsters.lua `drops`).
        {"ammo_clip",  80, du(10), du(16), fx::zero()},
        {"w_chaingun", 73, du(10), du(16), fx::zero()},
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
static bool truthy(const char* global) {
    lua_getglobal(L, global);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
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
    // Which particle kind follows each entity (e:set_trail), as the server keeps it.
    static u16 trail[sim::MAX_ENTITIES];
    static u8 trail_gen[sim::MAX_ENTITIES];
    ctx.trail = trail;
    ctx.trail_gen = trail_gen;
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
        "../modules/doom2/maps/weapons_doom2.lua",
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

    // WHICH MACHINE runs them (docs/WEAPON_PREDICTION.md): the core's, unless
    // DNP_WEAPONS_LUA=1 asks for the Lua one -- and every check below passes on
    // both, which is half of the parity argument. Asserted rather than assumed:
    // a compile that quietly failed would put every check here on the Lua path
    // and this file would still pass.
    {
        const char* env = std::getenv("DNP_WEAPONS_LUA");
        const bool want_lua = env != nullptr && std::strcmp(env, "1") == 0;
        if (!run("weapons_path = weapons.core_index() ~= nil and 'core' or 'lua'\n"
                 "weapons_why = tostring(weapons.core_why)", "asking which machine")) return 1;
        std::printf("  the weapon machine runs in %s (%s)\n", str("weapons_path").c_str(),
                    str("weapons_why").c_str());
        CHECK(str("weapons_path") == (want_lua ? "lua" : "core"));
    }

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
    // The aim rides along with the buttons, because they are one input frame:
    // setting them separately means whichever was written last wins and the
    // other is silently zeroed.
    u32 aim_raw = 0;
    const auto set_buttons = [&](u16 mask) {
        proto::InputFrame f{};
        f.buttons = mask;
        f.aim = brad(aim_raw);
        world.set_input(player, f);
    };
    // Turns, the engine's own unit. Facing is a RATE (turn_speed), so the body
    // takes a few ticks to come round -- a caller has to step the world after
    // this, not just set it.
    const auto set_aim = [&](double turns) {
        aim_raw = static_cast<u32>(static_cast<i64>(turns * 4294967296.0) & 0xFFFFFFFF);
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
             "-- THE WHOLE SWING TABLE, not just the number: knockback and slow\n"
             "-- are properties of the blow and travel with what it costs.\n"
             "swing_with_fists = weapons.swing_damage(p).damage\n"
             "weapons.get(p:index()).current = 'shotgun'\n", "asking swing_damage"))
        return 1;
    std::printf("  swing_damage: fists=%d shotgun=%s\n",
                (int)num("swing_with_fists"),
                num("swing_with_shotgun") < 0 ? "nil" : "a table");
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

    // ------------------------------------- one tick's worth of events fits
    //
    // MAX_PENDING_VFX is what the engine can queue in a tick and proto::MAX_VFX
    // is what a snapshot can carry. They have to agree, because an event the
    // engine refuses never reaches the writer that would have dropped it -- and
    // the engine's refusal is SILENT while the writer's is logged.
    //
    // This got tight the moment every hit also drew blood: a super shotgun is
    // twenty pellets, and twenty tracers beside twenty sprays is forty events
    // from one press.
    //
    // Counted straight off the world, because press_for drives the weapon
    // rather than the server loop and so nothing drains the queue under it --
    // which is exactly the condition being measured.
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "inv.have.supershotgun = true\n"
             "inv.ammo.shells = 40\n", "arming the super shotgun")) return 1;
    world.clear_vfx();
    press_for(20, 20, "supershotgun");
    std::printf("  one super shotgun blast queued %u events (the engine holds %u)\n",
                world.vfx_count(), sim::MAX_PENDING_VFX);
    // Twenty tracers and twenty sprays, plus a muzzle flash. If this is capped
    // at the limit, events are being thrown away before anything can send them.
    CHECK(world.vfx_count() >= 40);
    CHECK(world.vfx_count() < sim::MAX_PENDING_VFX);
    world.clear_vfx();

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

    // AND SEVEN TRACERS, EACH DOWN ITS OWN LINE.
    //
    // This is what a player actually sees, and it was wrong while everything
    // else was right: the tracer carried the body's FACING with no pellet
    // offset, so seven rays were cast, seven scattered, seven landed in
    // different places -- and seven tracers were drawn on top of each other
    // down the centre line. One visible ray. The damage was correct and the
    // picture was a lie, which is the worst way round, because nothing that
    // measures damage can notice.
    //
    // Recorded by replacing dnp.vfx, which is an ordinary field on the `dnp`
    // table, so everything up to the binding is the real code path.
    if (!run("vfx_log = {}\n"
             "local real_vfx = dnp.vfx\n"
             "dnp.vfx = function(style, pos, angle, len)\n"
             "  -- BY KIND. A pellet now emits its tracer AND a hit burst, so\n"
             "  -- counting every vfx call counts two things and the shotgun\n"
             "  -- looks like it fires fourteen rays. The tracer is kind 1.\n"
             "  if style == 1 then vfx_log[#vfx_log + 1] = angle end\n"
             "  return real_vfx(style, pos, angle, len)\n"
             "end\n"
             "weapons.get(dnp.players()[1]:index()).ammo.shells = 20\n",
             "recording tracers")) return 1;
    press_for(20, 20, "shotgun");
    if (!run("local seen, n = {}, 0\n"
             "for _, a in ipairs(vfx_log) do\n"
             "  if not seen[a] then seen[a] = true; n = n + 1 end\n"
             "end\n"
             "tracers = #vfx_log\n"
             "tracer_angles = n\n", "counting tracers")) return 1;
    std::printf("  one blast drew %d tracers along %d distinct angles\n",
                (int)num("tracers"), (int)num("tracer_angles"));
    CHECK(num("tracers") == 7);
    // SEVEN DISTINCT, not "more than one": the angles are the spread rolls
    // themselves, so two pellets sharing one is a coincidence of the generator
    // rather than of the room, and asserting the full count is fair.
    CHECK(num("tracer_angles") == 7);

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

    // A weapon can also run dry in the MIDDLE of a burst, with the trigger
    // never released -- a chaingun empties while the press edge is long past.
    // Asking for the edge would leave the player holding an inert gun with
    // bullets in their pocket, which is the exact complaint the auto-switch was
    // written for, so the condition is `button_held` and not `button_pressed`.
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "inv.have = { fists = true, pistol = true, chaingun = true }\n"
             "inv.ammo.bullets = 3\n"
             "inv.current = 'chaingun'\n", "arming a nearly-empty chaingun")) return 1;
    press_for(80, 80, "chaingun");
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "after_burst = inv.current\n"
             "bullets_left = inv.ammo.bullets\n", "reading the result")) return 1;
    std::printf("  a chaingun run dry mid-burst leaves %d bullets and the %s\n",
                (int)num("bullets_left"), str("after_burst").c_str());
    CHECK(num("bullets_left") == 0);
    // NOT the chaingun. The fists, in fact, because the pistol draws on the same
    // empty pool -- which is switch_to_usable picking the highest order that can
    // actually fire rather than the highest order full stop.
    CHECK(str("after_burst") == "fists");

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
    // A touch whose prop something else removed first (a graph's `remove`, the
    // same tick) arrives as nil: nothing to pick up, and no error.
    CHECK(run("gone_ok = pcall(weapons.pickup, dnp.players()[1], nil)", "a touch of nothing"));
    CHECK(truthy("gone_ok"));

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
    // What the ENGINE queued for the wire, tick by tick. Kept because the queue
    // is cleared every tick, below.
    std::vector<u16> queued_vfx;
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
        // AND THE PENDING VFX ARE DRAINED, exactly as main.cpp:2089 drains them
        // once the snapshots are written. The queue holds MAX_PENDING_VFX = 32
        // and a server empties it every tick; a harness that never did
        // saturated after the first few shots and then silently rejected
        // everything -- so a script that fired its event perfectly looked like
        // an engine that had not queued one.
        for (u32 i = 0; i < world.vfx_count(); ++i) queued_vfx.push_back(world.vfx(i).kind);
        world.clear_vfx();
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
             "-- IN FRONT OF WHEREVER THE PLAYER IS. Absolute coordinates were\n"
             "-- fine while the player sat on the origin and stopped being fine\n"
             "-- the moment another block moved them: the rocket then flew off\n"
             "-- past a target twenty units away and the test read as a rocket\n"
             "-- that does no damage.\n"
             "local pp = p:pos()\n"
             "target = mobs.spawn('zombieman',\n"
             "  vec3.new(pp.x + fx.frac(40,48), pp.y, pp.z))\n"
             "hp_before_rocket = target_hp()\n"
             "-- Every effect the shot sends, by kind: the blast is 11 and its\n"
             "-- smoke 13 (modules/doom2/maps/doom_monsters.lua, rocket.on_hit).\n"
             "rocket_vfx = {}\n"
             "local prior_vfx = dnp.vfx\n"
             "dnp.vfx = function(style, pos, angle, len)\n"
             "  rocket_vfx[style] = (rocket_vfx[style] or 0) + 1\n"
             "  return prior_vfx(style, pos, angle, len)\n"
             "end\n", "arming the rocket launcher")) return 1;
    CHECK(num("hp_before_rocket") > 0);
    server_tick(0);
    server_tick(0);
    server_tick(proto::button::ATTACK_PRIMARY);
    for (int i = 0; i < 40; ++i) server_tick(0);
    if (!run("local inv = weapons.get(dnp.players()[1]:index())\n"
             "rockets_after = inv.ammo.rockets\n"
             "hp_after_rocket = target_hp()\n"
             "blast_vfx = rocket_vfx[11] or 0\n"
             "smoke_vfx = rocket_vfx[13] or 0\n", "reading the result")) return 1;
    std::printf("  the rocket's impact sent %d blast and %d smoke effects\n",
                (int)num("blast_vfx"), (int)num("smoke_vfx"));
    std::printf("  rocket launcher: %d -> %d rockets; the target went %d -> %d\n",
                (int)num("rockets_before"), (int)num("rockets_after"),
                (int)num("hp_before_rocket"), (int)num("hp_after_rocket"));
    CHECK(num("rockets_after") == num("rockets_before") - 1);
    // It FLEW AND ARRIVED. The target is a little under a unit away and the
    // blast radius is 128 map units, so a rocket that never left the tube and
    // one that detonated correctly both reduce this -- which is why the recoil
    // test above asserts the launcher's own impulse separately.
    CHECK(num("hp_after_rocket") < num("hp_before_rocket"));
    // AND IT WAS SEEN TO: the explosion draws itself at the impact.
    CHECK(num("blast_vfx") == 1 && num("smoke_vfx") == 1);

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

          -- NEGATE, in a list: a press while `heat` is NOT 0 vents instead of
          -- firing. Written the way an author writes "is not": `var_is` with
          -- `negate`, there being no `var_is_not`.
          ventgun = {
            label = "Vent Gun", order = 23, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = { { cond = "button_pressed" },
                           { cond = "var_is", var = "heat", value = 0, negate = true } },
                  to = "vent" },
                { when = "button_pressed", to = "fire" } } },
              fire = { anim = 2, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "add_var", var = "heat", value = 1 } } },
              vent = { anim = 3, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "heat", value = 0 },
                { at = 0, action = "add_var", var = "vents", value = 1 } } },
            },
            vars = { heat = 0, vents = 0 },
          },

          -- NEGATE on the one-condition form, whose arguments sit on the
          -- transition: `check` goes to `b` when `mode` is not 0.
          togglegun = {
            label = "Toggle", order = 24, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = "button_pressed", to = "check" } } },
              check = { transitions = {
                { when = "var_is", var = "mode", value = 0, negate = true, to = "b" },
                { when = "always", to = "a" } } },
              a = { anim = 2, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "mode", value = 1 },
                { at = 0, action = "add_var", var = "path", value = 1 } } },
              b = { anim = 3, ticks = 4, next = "ready", sequence = {
                { at = 0, action = "set_var", var = "mode", value = 0 },
                { at = 0, action = "add_var", var = "path", value = 10 } } },
            },
            vars = { mode = 0, path = 0 },
          },

          -- COOLDOWN: once every 30 ticks, without holding the player in
          -- `fire` that long (which is what stops them switching weapons).
          coolgun = {
            label = "Cool Gun", order = 25, initial = "ready",
            phases = {
              ready = { transitions = {
                { when = { { cond = "button_pressed" },
                           { cond = "cooldown_ready", var = "cd" } }, to = "fire" } } },
              fire = { anim = 2, ticks = 2, next = "ready", sequence = {
                { at = 0, action = "set_cooldown", var = "cd", ticks = 30 },
                { at = 0, action = "add_var", var = "shots", value = 1 } } },
            },
            vars = { cd = 0, shots = 0 },
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
    // '1' primary down, '2' secondary down, '.' nothing, 'x' put it away for
    // the alt-fire gun, 'y' take it out again. Spelling the input out is what
    // makes "one tick after the window" a thing that can be written.
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
            if (*c == 'x' || *c == 'y') {
                const std::string sw =
                    "local inv = weapons.get(dnp.players()[1]:index())\n"
                    "inv.have.altgun = true\n"
                    "inv.current = '" + std::string(*c == 'x' ? "altgun" : weapon) + "'\n";
                if (!run(sw.c_str(), "switching weapons")) return false;
            }
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

    // NEGATE. Three presses: fire (heat 1), vent (heat back to 0), fire.
    if (!pattern("ventgun", ".1....1....1....")) return 1;
    const int heat = var_of("heat"), vents = var_of("vents");
    std::printf("  negate: three presses -> heat %d, vents %d\n", heat, vents);
    // CONTROL in the numbers: a machine that ignored `negate` would vent on
    // every press (heat 0, vents 3), and one that dropped the negated
    // condition would never vent (heat 3, vents 0).
    CHECK(heat == 1 && vents == 1);
    // The one-condition form. The first press finds `mode` at 0, so the
    // negated `var_is` does not hold and `always` takes it to `a` (+1); the
    // second finds it at 1, which the negation makes hold, so `b` (+10).
    if (!pattern("togglegun", ".1....1....")) return 1;
    const int path = var_of("path");
    std::printf("  negate on one condition: two presses -> path %d\n", path);
    // CONTROL in the number: ignoring `negate` goes to `b` both times (20).
    CHECK(path == 11);

    // COOLDOWN. A second press 5 ticks after the first finds it cooling...
    if (!pattern("coolgun", ".1....1....")) return 1;
    const int cool_soon = var_of("shots");
    // ...and one 36 ticks after does not.
    if (!pattern("coolgun", std::string(".1").append(35, '.').append("1.").c_str())) return 1;
    const int cool_late = var_of("shots");
    // PUT AWAY AND TAKEN OUT AGAIN, then pressed at tick 10 and at tick 41.
    // The switch starts `shots` again, as any var; the cooldown carries on, so
    // the press at 10 is refused and the one at 41 fires: one shot. Had the
    // switch cleared the cooldown, both would fire.
    if (!pattern("coolgun", std::string(".1...x..y.1").append(30, '.').append("1.").c_str())) return 1;
    const int cool_switched = var_of("shots");
    std::printf("  cooldown: shots %d (pressed again at 5), %d (at 36), %d (switched away and back)\n",
                cool_soon, cool_late, cool_switched);
    CHECK(cool_soon == 1);
    CHECK(cool_late == 2);                                 // CONTROL: it does end
    CHECK(cool_switched == 1);

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
        {"a `negate` that is not true or false",
         "weapons.check_weapon('b13', { label = 'B', order = 90, initial = 'ready',"
         "  phases = { ready = { transitions = {"
         "    { when = { { cond = 'always', negate = 'yes' } }, to = 'ready' } } } } })"},
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
    // input would satisfy all thirteen.
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
    // A swing's fraction as the studio writes it, {n, d}, is read as that exact
    // number: left a table, it reached add_effect and the arc was never set.
    if (!run("local d = { label = 'Arc', order = 92, initial = 'ready',"
             "  swing = { damage = 5, half_arc = { 1, 8 }, sweep_from = { -1, 8 } },"
             "  phases = { ready = { anim = 0 } } }\n"
             "local before = weapons.load_errors()\n"
             "weapons.check_weapon('arced', d)\n"
             "arc_complaints = weapons.load_errors() - before\n"
             "arc_exact = (type(d.swing.half_arc) ~= 'table' and d.swing.half_arc == fx.frac(1, 8)"
             "  and d.swing.sweep_from == fx.frac(-1, 8)) and 1 or 0\n", "a fraction")) return 1;
    CHECK(num("arc_complaints") == 0);
    CHECK(num("arc_exact") == 1);

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

    // A SELECT'S VALUE IS A ZERO-BASED INDEX, and choosing each option must
    // land on the weapon whose label is at that index.
    //
    // The bug this replaces: Lua counts from one and this panel sent Lua's
    // number, so choosing the FIRST weapon did nothing at all (index 0 reached
    // list[0], which is nil) and choosing the second gave you the first. It
    // reads exactly like an unnamed "no weapon" entry occupying the top of the
    // list -- because that is what index 0 was.
    //
    // Round-tripped through on_ui_event, the same entry point the client's
    // message reaches, rather than by inspecting the panel: the panel was
    // already self-consistent and wrong.
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have = { fists = true, pistol = true, shotgun = true }\n"
             "local panel = weapons.panel_for(p:index())\n"
             "sel_widget = nil\n"
             "for _, w in ipairs(panel.widgets) do\n"
             "  if w.kind == 'select' then sel_widget = w end\n"
             "end\n"
             "sel_lo, sel_hi = sel_widget.lo, sel_widget.hi\n"
             "sel_opts = #sel_widget.options\n"
             "sel_labels = table.concat(sel_widget.options, ', ')\n",
             "reading the select")) return 1;
    std::printf("  the weapon select offers %d options [%d..%d]: %s\n",
                (int)num("sel_opts"), (int)num("sel_lo"), (int)num("sel_hi"),
                str("sel_labels").c_str());
    // The range the wire expects: zero to count-1. The server DROPS anything
    // outside it rather than clamping (main.cpp:912), so an off-by-one here is
    // a selection that silently never happens.
    CHECK(num("sel_lo") == 0);
    CHECK(num("sel_hi") == num("sel_opts") - 1);

    // Every index, in order, lands on its own label.
    if (!run("local p = dnp.players()[1]\n"
             "local panel = weapons.panel_for(p:index())\n"
             "local sel\n"
             "for _, w in ipairs(panel.widgets) do\n"
             "  if w.kind == 'select' then sel = w end\n"
             "end\n"
             "local ok = 0\n"
             "local got = {}\n"
             "for i = 0, #sel.options - 1 do\n"
             "  weapons.choose(p, i)\n"
             "  local now = weapons.get(p:index()).current\n"
             "  got[#got + 1] = now\n"
             "  if weapons.defs[now] and weapons.defs[now].label == sel.options[i + 1] then\n"
             "    ok = ok + 1\n"
             "  end\n"
             "end\n"
             "sel_matched = ok\n"
             "sel_got = table.concat(got, ', ')\n", "round-tripping the select"))
        return 1;
    std::printf("  choosing options 0..%d selects: %s\n",
                (int)num("sel_opts") - 1, str("sel_got").c_str());
    CHECK(num("sel_matched") == num("sel_opts"));

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

    // --------------------------------------------- a rocket fired at a WALL
    //
    // THE ONE THAT WAS REPORTED. `on_hit` ran only when a missile reached an
    // ENTITY, so a rocket that stopped against geometry -- which is most of
    // them -- played its burst animation and ran none of its on_hit: no damage,
    // no blast, no explosion. Doom's whole rocket jump is a rocket fired at the
    // floor, and a rocket into a wall is what a player mostly does.
    //
    // Fired at the room's edge with the victim beside the impact rather than in
    // the rocket's path, so what is measured is the BLAST and not a direct hit.
    // A player's input comes from their session, so it is driven through the
    // world the way every other press in this file is.
    if (!run("dnp.players()[1]:teleport(vec3.new(fx.int(-55), fx.int(0), fx.int(0)))",
             "facing a wall")) return 1;
    // Round to face -X, where the wall is five units away. Turning is a rate,
    // so it is given long enough to actually get there before anything fires.
    set_aim(0.5);
    for (int i = 0; i < 30; ++i) server_tick(0);
    if (!run("wall_facing = (dnp.players()[1]:facing() / 4294967296)",
             "reading the facing")) return 1;
    std::printf("  the player faces %.3f of a turn (0.5 is at the near wall)\n",
                num("wall_facing"));
    CHECK(std::abs(num("wall_facing") - 0.5) < 0.02);
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have.rocketlauncher = true\n"
             "inv.current = 'rocketlauncher'\n"
             "inv.ammo.rockets = 10\n"
             "weapons.rearm_player(p:index())\n"
             "wall_vfx = {}\n"
             "local real_vfx = dnp.vfx\n"
             "dnp.vfx = function(kind, pos, ang, param)\n"
             "  wall_vfx[#wall_vfx + 1] = kind\n"
             "  return real_vfx(kind, pos, ang, param)\n"
             "end\n"
             "-- BESIDE the wall, not in the rocket's path: a direct hit would\n"
             "-- prove the old code path, which already worked.\n"
             "local pp = p:pos()\n"
             "bystander = mobs.spawn('zombieman',\n"
             "  vec3.new(fx.int(-59), pp.y + fx.int(1), pp.z))\n"
             "mobs.live[bystander:index()].hp = 200\n"
             "wall_hp0 = mobs.live[bystander:index()].hp\n",
             "placing a bystander")) return 1;
    queued_vfx.clear();
    server_tick(0);
    server_tick(proto::button::ATTACK_PRIMARY);
    for (int i = 0; i < 60; ++i) server_tick(0);
    if (!run("local r = mobs.live[bystander:index()]\n"
             "wall_hp1 = r and r.hp or -1\n"
             "local fire, smoke = 0, 0\n"
             "for _, k in ipairs(wall_vfx) do\n"
             "  if k == 11 then fire = fire + 1 end\n"
             "  if k == 13 then smoke = smoke + 1 end\n"
             "end\n"
             "wall_fire, wall_smoke = fire, smoke\n", "reading the blast")) return 1;
    // AND IT REACHED THE ENGINE'S EVENT LIST, which is what actually gets
    // encoded into the snapshot. Counting the Lua calls proves the script fired
    // and nothing more: a binding that rejected the event, or a world that had
    // no room for it, would leave the script looking perfectly correct and the
    // player looking at nothing.
    int engine_fire = 0;
    for (const u16 k : queued_vfx) if (k == 11) ++engine_fire;
    std::printf("  a rocket into a wall: %d fireball and %d smoke bursts, "
                "a bystander went %d -> %d; the engine queued %d fireball "
                "event(s) for the wire\n", (int)num("wall_fire"),
                (int)num("wall_smoke"), (int)num("wall_hp0"), (int)num("wall_hp1"),
                engine_fire);
    CHECK(engine_fire >= 1);
    // IT EXPLODED. Both kinds its on_hit declares, drawn at the impact whether
    // or not anything was standing there.
    CHECK(num("wall_fire") >= 1);
    CHECK(num("wall_smoke") >= 1);
    // AND IT HURT SOMEBODY STANDING NEARBY, which is the half a burst animation
    // cannot fake.
    CHECK(num("wall_hp1") < num("wall_hp0"));
    if (!run("dnp.vfx = real_vfx_restore or dnp.vfx\n"
             "if bystander:valid() then bystander:despawn() end\n"
             "dnp.players()[1]:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n",
             "clearing up")) return 1;

    // ================================================================
    // A COMBO ESCALATES, which one shared `swing` could not express.
    //
    // Three presses reaching jab1, jab2, hook was already the shape the phase
    // graph gave -- and all three swung the identical volume for the identical
    // damage, because `swing` was a property of the WEAPON and there was one of
    // it. A phase carries its own now, so the finisher hits for three times the
    // first and throws what it lands on.
    if (!run("local p = dnp.players()[1]\n"
             "local inv = weapons.get(p:index())\n"
             "inv.have = { fists = true }\n"
             "inv.current = 'fists'\n"
             "weapons.rearm_player(p:index())\n"
             "combo_damage = {}\n", "arming the fists")) return 1;
    {
        // One press, then a second inside recover1, then a third inside
        // recover2 -- the window is what `ticks` on a recover phase IS.
        // Presses at 1, 17 and 33. jab1 runs ticks 1-11 and has no
        // transitions, so a press during it is ignored -- the second has to
        // land in recover1 (11-25) and the third in recover2 (29-45). Getting
        // that wrong reads as "the combo does not work" and is a mistimed
        // fixture.
        const char* pat = ".1...............1...............1...............";
        u32 t = 200000;
        for (const char* ch = pat; *ch; ++ch) {
            set_buttons(*ch == '1' ? proto::button::ATTACK_PRIMARY : 0);
            const std::string step =
                "local p = dnp.players()[1]\n"
                "weapons.tick_player(p, p:index(), " + std::to_string(++t) + ")\n"
                "local sw = weapons.swing_damage(p)\n"
                "local ph = weapons.phase_of(p:index())\n"
                "if ph and combo_damage[ph] == nil then\n"
                "  combo_damage[ph] = (sw and sw.damage) or -1\n"
                "end\n";
            if (!run(step.c_str(), "stepping the combo")) return 1;
        }
        set_buttons(0);
    }
    if (!run("jab1_dmg = combo_damage.jab1 or -1\n"
             "jab2_dmg = combo_damage.jab2 or -1\n"
             "hook_dmg = combo_damage.hook or -1\n", "reading the combo")) return 1;
    std::printf("  a three-hit combo escalates: jab1=%d jab2=%d hook=%d\n",
                (int)num("jab1_dmg"), (int)num("jab2_dmg"), (int)num("hook_dmg"));
    // All three REACHED, or the window is not a window.
    CHECK(num("jab1_dmg") > 0);
    CHECK(num("jab2_dmg") > 0);
    CHECK(num("hook_dmg") > 0);
    // AND THEY DIFFER. Without a per-phase swing all three are the weapon's
    // one number, which is what this whole block is about.
    CHECK(num("jab2_dmg") > num("jab1_dmg"));
    CHECK(num("hook_dmg") > num("jab2_dmg"));

    // ------------------------------------------------- the same button, airborne
    //
    // A down-slash while off the ground is a transition, not a feature: the
    // airborne branch sits above the grounded one in `ready` and the same
    // button reaches a different phase.
    if (!run("local p = dnp.players()[1]\n"
             "weapons.rearm_player(p:index())\n"
             "p:teleport(vec3.new(fx.int(20), fx.int(20), fx.int(6)))\n"
             "p:set_velocity(vec3.new(fx.int(0), fx.int(0), fx.int(4)))\n",
             "putting the player in the air")) return 1;
    server_tick(0);
    if (!run("airborne_now = dnp.players()[1]:grounded() and 0 or 1",
             "checking it is airborne")) return 1;
    // The fixture has to actually leave the ground, or the branch below is
    // untested and passes for the wrong reason.
    CHECK(num("airborne_now") == 1);
    set_buttons(proto::button::ATTACK_PRIMARY);
    if (!run("local p = dnp.players()[1]\n"
             "weapons.tick_player(p, p:index(), 300001)\n"
             "air_phase = weapons.phase_of(p:index())\n"
             "air_damage = (weapons.swing_damage(p) or {}).damage or -1\n",
             "attacking in the air")) return 1;
    set_buttons(0);
    std::printf("  pressing attack while airborne reaches '%s' (%d damage)\n",
                str("air_phase").c_str(), (int)num("air_damage"));
    CHECK(str("air_phase") == "slam");
    // CONTROL: on the ground the identical press reaches the jab instead.
    if (!run("local p = dnp.players()[1]\n"
             "-- NOT the origin: the demon section below spawns there, and two\n"
             "-- bodies in one place shove each other, which moved the demon and\n"
             "-- shifted its arc by a tick.\n"
             "p:teleport(vec3.new(fx.int(20), fx.int(20), fx.int(0)))\n"
             "p:set_velocity(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n",
             "putting it back down")) return 1;
    for (int i = 0; i < 20; ++i) server_tick(0);
    if (!run("grounded_now = dnp.players()[1]:grounded() and 1 or 0",
             "checking it landed")) return 1;
    CHECK(num("grounded_now") == 1);
    if (!run("weapons.rearm_player(dnp.players()[1]:index())", "rearming")) return 1;
    set_buttons(proto::button::ATTACK_PRIMARY);
    if (!run("local p = dnp.players()[1]\n"
             "weapons.tick_player(p, p:index(), 300100)\n"
             "ground_phase = weapons.phase_of(p:index())\n", "attacking on the ground"))
        return 1;
    set_buttons(0);
    std::printf("  CONTROL: the same press on the ground reaches '%s'\n",
                str("ground_phase").c_str());
    CHECK(str("ground_phase") == "jab1");

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
        if (FILE* fp = std::fopen("../docs/WEAPONS.md", "rb")) {
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
    // Two ticks EARLY, not exactly on the landing tick: the arc's timing moves
    // by a tick with the fixture, and a dodge that races the hit it is dodging
    // measures the ordering of two lines rather than the window.
    const int dodged = bite(55, hit_at_end - 2);
    std::printf("  and one that steps away on tick %d is missed (%s)\n",
                hit_at_end - 2, dodged < 0 ? "never hit" : "still hit");
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
    // Driven through the mob runtime's own on_tick, as a fight would be.
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
    // ALL THREE. Verified to be a real gate, when a swing was a press, by
    // disabling the release that followed it in mobs.lua: it landed 1 of 3,
    // the bug exactly -- the first press worked and every one after it was
    // swallowed. A swing step no longer presses at all (it calls e:swing, which
    // starts the World's swing outright), so this now holds by construction;
    // it stays as the regression it was.
    CHECK(swings_landed == 3);
    run("if twice_victim:valid() then twice_victim:despawn() end", "clearing");

    // ------------------------------------------------- the arc has a TOP and a BOTTOM
    //
    // `half_pitch` is what makes an overhead chop a different volume from a
    // horizontal slash rather than the same one drawn differently. Zero means
    // "do not check", which is every swing written before it existed, so the
    // claim has to be made in both directions: a pitched swing misses what a
    // flat one hits.
    if (!run("demon:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
             "demon:set_input(fx.int(0), fx.int(0), 0, 0)\n"
             "mobs.acts.sleep(mobs.live[demon:index()])\n", "resetting the demon"))
        return 1;
    const auto bite_at_height = [&](bool pitched, long long z_raw) {
        const std::string setup =
            std::string("local d = demon\n") +
            // A victim ABOVE the demon, in front of it. A flat swing reaches it
            // because the z band is generous; a swing pitched downward must not.
            "high = mobs.spawn('zombieman', vec3.new(fx.frac(50,48), fx.int(0), "
            "                                        fx.raw(" + std::to_string(z_raw) + ")))\n"
            "high:set_solid(false)\n"
            "mobs.live[high:index()].hp = 200\n"
            "high_hp0 = mobs.live[high:index()].hp\n"
            // Pitched DOWNWARD, away from the target above.
            + (pitched
               ? "d:set_body_param('attack_half_pitch', fx.frac(1,16))\n"
                 "d:set_body_param('attack_pitch_from', fx.frac(-1,8))\n"
                 "d:set_body_param('attack_pitch_to', fx.frac(-1,8))\n"
               : "d:set_body_param('attack_half_pitch', fx.int(0))\n");
        if (!run(setup.c_str(), "placing a high victim")) return -1;
        recompute();
        for (int i = 0; i < 4; ++i) server_tick(0);
        if (!run("mobs.acts.swing(mobs.live[demon:index()])", "swinging high")) return -1;
        for (int i = 0; i < 24; ++i) server_tick(0);
        if (!run("local r = mobs.live[high:index()]\n"
                 "high_hp1 = r and r.hp or high_hp0", "reading the high victim")) return -1;
        const int lost = static_cast<int>(num("high_hp0") - num("high_hp1"));
        run("if high:valid() then high:despawn() end", "clearing");
        return lost;
    };
    // Roughly a unit and a quarter up: inside the z band, above a downward arc.
    const long long kUp = static_cast<long long>(1.25 * 4294967296.0);
    const int flat_hit = bite_at_height(false, kUp);
    const int pitched_hit = bite_at_height(true, kUp);
    std::printf("  a target %.2f above: a flat swing costs it %d, "
                "one pitched downward costs it %d\n", 1.25, flat_hit, pitched_hit);
    // CONTROL: unconstrained, it connects -- so the miss below is the pitch and
    // not the geometry being out of reach all along.
    CHECK(flat_hit > 0);
    // AND THE PITCHED ONE MISSES, which is the whole of the vertical arc.
    CHECK(pitched_hit == 0);
    if (!run("demon:set_body_param('attack_half_pitch', fx.int(0))",
             "restoring the demon")) return 1;

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

    // ==================================================================
    // AUTO-AIM
    //
    // A command carries ONE brad and a brad is a heading, so this engine has
    // never had a vertical aim -- and every bullet and every rocket travelled
    // dead level. Anything standing on a step or in a pit was unhittable while
    // `can_see` said plainly that it was visible, because sight learned to
    // slope and shooting never did.
    //
    // So the vertical correction is total and the horizontal one is slight, and
    // both are asserted against a control that must fail: a claim about an aim
    // assist is worthless unless the unassisted shot is shown to miss.

    // A CLEAN STAGE. Every earlier block left a zombieman somewhere, dead or
    // alive, and an auto-aim is precisely a thing that finds the nearest body
    // it can see -- so a leftover corpse two units away is not background, it
    // is the answer.
    if (!run("local p = dnp.players()[1]\n"
             "for _, e in ipairs(dnp.entities()) do\n"
             "  if e:index() ~= p:index() then e:despawn() end\n"
             "end\n"
             "mobs.reset()\n"
             "p:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
             "p:set_velocity(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n",
             "clearing the stage")) return 1;
    server_tick(0);
    server_tick(0);

    // Two units ahead and three units up: a balcony, in the units this room is
    // sixty across in. Placed along the player's OWN facing, because several
    // blocks above have turned it and a hardcoded +x would be aiming at a wall.
    if (!run("local p = dnp.players()[1]\n"
             "local pp, d = p:pos(), dnp.dir(p:facing())\n"
             "aim_up = mobs.spawn('zombieman',\n"
             "  vec3.new(pp.x + d.x * fx.int(2), pp.y + d.y * fx.int(2),\n"
             "           pp.z + fx.int(3)))\n"
             "mobs.live[aim_up:index()].hp = 500\n", "a target on a balcony")) return 1;
    recompute();

    // THE ENABLING HALF: dnp.hitscan can now travel at a slope.
    //
    // Asked of the binding directly and with no ticks in between, so nothing
    // has moved and the only difference between the two calls is the fourth
    // argument.
    if (!run("local p = dnp.players()[1]\n"
             "aim_slope = mobs.slope_to(p, aim_up)\n"
             "local flat = dnp.hitscan(p, fx.int(8), 0, fx.int(0))\n"
             "local sloped = dnp.hitscan(p, fx.int(8), 0, aim_slope)\n"
             "flat_found = (flat ~= nil and flat:index() == aim_up:index()) and 1 or 0\n"
             "sloped_found = (sloped ~= nil and sloped:index() == aim_up:index()) and 1 or 0\n",
             "casting at a balcony")) return 1;
    std::printf("  a body three units up: a flat ray finds it %d times, a sloped one %d\n",
                (int)num("flat_found"), (int)num("sloped_found"));
    // CONTROL: the flat ray -- every ray this engine cast before today -- goes
    // straight underneath it. Without this the line below passes against a
    // binding that ignores its new argument entirely.
    CHECK(num("flat_found") == 0);
    CHECK(num("sloped_found") == 1);

    // THE SEARCH finds it, and reports a slope that climbs.
    if (!run("local p = dnp.players()[1]\n"
             "local t, ang, sl = mobs.autoaim(p, fx.int(8))\n"
             "up_found = (t ~= nil and t:index() == aim_up:index()) and 1 or 0\n"
             "up_climbs = (sl ~= nil and sl > fx.int(0)) and 1 or 0\n",
             "searching")) return 1;
    CHECK(num("up_found") == 1);
    CHECK(num("up_climbs") == 1);

    // AND THE CORE'S AUTO-AIM AGREES (docs/WEAPON_PREDICTION.md): the search a
    // player's predicted shot is corrected with, on the client and the server
    // alike, is World::autoaim, and it has to land on the same heading and the
    // same slope as this file's to the last bit -- or the two machines aim
    // differently and the parity above is about everything but the aim.
    if (!run("local p = dnp.players()[1]\n"
             "local t, ang, sl = mobs.autoaim(p, fx.int(8))\n"
             "local cang, csl = dnp.weapon_autoaim(p, fx.int(8), mobs.autoaim_h)\n"
             "aim_same = (t ~= nil and cang == ang and csl ~= nil and csl.raw == sl.raw) and 1 or 0\n"
             "-- CONTROL: the cone shut, the core finds nothing either.\n"
             "aim_shut = (dnp.weapon_autoaim(p, fx.int(8), 0) == nil) and 1 or 0\n"
             "aim_dbg = tostring(ang) .. ' ' .. tostring(sl and sl.raw) .. ' / ' .. tostring(cang) .. ' ' .. tostring(csl and csl.raw)\n",
             "asking the core")) return 1;
    std::printf("  auto-aim, mobs.lua and the core: %s\n", str("aim_dbg").c_str());
    CHECK(num("aim_same") == 1);
    CHECK(num("aim_shut") == 1);

    // AND THE WHOLE PLAYER PATH, which is this call: weapons.lua's own
    // `hitscan` action is one line handing the mob runtime a synthetic owner,
    // so driving it here drives exactly the code a trigger pull runs.
    if (!run("local p = dnp.players()[1]\n"
             "local shot = { range = fx.int(8), damage = 10, count = 1 }\n"
             "aim_hp0 = mobs.live[aim_up:index()].hp\n"
             "mobs.acts.hitscan(mobs.synthetic_owner(p, p:facing()), shot)\n"
             "aim_hp1 = mobs.live[aim_up:index()].hp\n"
             "-- CONTROL: the same shot with the cone shut. Nothing else changes.\n"
             "mobs.autoaim_h = 0\n"
             "mobs.acts.hitscan(mobs.synthetic_owner(p, p:facing()), shot)\n"
             "aim_hp2 = mobs.live[aim_up:index()].hp\n"
             "mobs.autoaim_h = 5.625\n", "shooting at a balcony")) return 1;
    std::printf("  a player firing at it: %d -> %d hit points, and with the cone shut %d\n",
                (int)num("aim_hp0"), (int)num("aim_hp1"), (int)num("aim_hp2"));
    CHECK(num("aim_hp1") < num("aim_hp0"));
    CHECK(num("aim_hp2") == num("aim_hp1"));

    // A WEAPON'S OWN CHOICE (its `autoaim`), on the Lua path: off is the flat
    // shot that passes underneath; the cone a weapon names replaces the game's.
    if (!run("local p = dnp.players()[1]\n"
             "local shot = { range = fx.int(8), damage = 10, count = 1 }\n"
             "local function fire(mode, cone)\n"
             "  local m = mobs.synthetic_owner(p, p:facing())\n"
             "  m.aim_mode, m.aim_cone = mode, cone\n"
             "  local was = mobs.live[aim_up:index()].hp\n"
             "  mobs.acts.hitscan(m, shot)\n"
             "  return was - mobs.live[aim_up:index()].hp\n"
             "end\n"
             "own_off = fire(2)\n"
             "own_on = fire(1)\n"
             "mobs.autoaim_h = 0\n"
             "own_cone = fire(0, 10)\n"
             "own_shut = fire(0)\n"
             "mobs.autoaim_h = 5.625\n"
             "own_op_off = (mobs.weapon_runtime(p, 'autoaim', 2) == nil) and 1 or 0\n"
             "own_op_on = (mobs.weapon_runtime(p, 'autoaim', 1) ~= nil) and 1 or 0\n",
             "a weapon's own aim assist")) return 1;
    std::printf("  the weapon's choice: off %d, on %d, its own cone with the game's shut %d, shut %d\n",
                (int)num("own_off"), (int)num("own_on"), (int)num("own_cone"), (int)num("own_shut"));
    CHECK(num("own_off") == 0);
    CHECK(num("own_on") > 0);            // CONTROL: the same shot, assisted, lands
    CHECK(num("own_cone") > 0);
    CHECK(num("own_shut") == 0);         // CONTROL: without the weapon's cone, nothing
    CHECK(num("own_op_off") == 1);
    CHECK(num("own_op_on") == 1);
    // The compiler: a cone past a half turn is refused, naming the field.
    if (!run("local ok, why = dnp.weapons_compile({ gun = { label = 'Gun', order = 1, autoaim = false, autoaim_cone = 500, initial = 'ready',\n"
             "  phases = { ready = { } } } }, {})\n"
             "own_refused = (ok == nil and tostring(why):find('autoaim_cone')) and 1 or 0\n"
             "local ok2, why2 = dnp.weapons_compile({ gun = { label = 'Gun', order = 1, autoaim = false, autoaim_cone = 15, initial = 'ready',\n"
             "  phases = { ready = { } } } }, {})\n"
             "own_taken = ok2 ~= nil and 1 or 0\n"
             "own_why = tostring(why) .. ' / ' .. tostring(why2)\n", "compiling a weapon's aim")) return 1;
    std::printf("  compiling: %s\n", str("own_why").c_str());
    CHECK(num("own_refused") == 1);
    CHECK(num("own_taken") == 1);        // CONTROL: the same weapon, a legal cone

    // THE HORIZONTAL HALF, and that it is SLIGHT.
    //
    // A candidate four degrees off the nose at five units, and one fifteen
    // degrees off at THREE -- nearer, so a search that only asked "what is
    // closest" would take it. The cone has to be what refuses it, and the
    // second half of the check removes the legal candidate and requires the
    // answer to become nothing rather than the illegal one.
    if (!run("local p = dnp.players()[1]\n"
             "for _, e in ipairs(dnp.entities()) do\n"
             "  if e:index() ~= p:index() then e:despawn() end\n"
             "end\n"
             "mobs.reset()\n", "clearing the stage again")) return 1;
    server_tick(0);
    if (!run("local p = dnp.players()[1]\n"
             "local pp = p:pos()\n"
             "local function place(deg, dist)\n"
             "  local a = (p:facing() + math.floor(deg * 4294967296 / 360)) % 4294967296\n"
             "  local d = dnp.dir(a)\n"
             "  return mobs.spawn('zombieman',\n"
             "    vec3.new(pp.x + d.x * dist, pp.y + d.y * dist, pp.z))\n"
             "end\n"
             "near_cone = place(4, fx.int(5))\n"
             "off_cone  = place(15, fx.int(3))\n", "two candidates")) return 1;
    recompute();
    if (!run("local p = dnp.players()[1]\n"
             "local t = mobs.autoaim(p, fx.int(10))\n"
             "cone_pick = (t == nil) and 0 or (t:index() == near_cone:index() and 1 or 2)\n"
             "near_cone:despawn()\n"
             "local t2 = mobs.autoaim(p, fx.int(10))\n"
             "cone_alone = (t2 == nil) and 0 or 1\n", "asking the cone")) return 1;
    std::printf("  of two bodies, 4 deg at five units and 15 deg at three, "
                "the cone picks %s\n",
                num("cone_pick") == 1 ? "the nearer-to-the-nose one"
                                      : (num("cone_pick") == 2 ? "the WRONG one" : "neither"));
    CHECK(num("cone_pick") == 1);
    // CONTROL: fifteen degrees is outside it. A cone wide enough to take
    // anything would have answered 1 here, and the assist would be an aimbot.
    CHECK(num("cone_alone") == 0);

    // ------------------------------------------------------- and a PROJECTILE
    //
    // A missile carries the same slope and adds it to its height once per step.
    // Measured on the flying prop itself rather than on a victim, because a
    // victim tall enough to be hit by both is a victim that proves neither.
    const auto rocket_rise = [&](const char* slope_expr) {
        const std::string src =
            std::string("local p = dnp.players()[1]\n"
                        "for _, pr in ipairs(dnp.props()) do pr:despawn() end\n"
                        "mobs.launch_from(p, 'rocket', p:facing(), fx.frac(30,48), ")
            + slope_expr + ")\n"
            "rocket_z0 = nil\n"
            "for _, pr in ipairs(dnp.props()) do\n"
            "  if pr:name() == 'rocket' then rocket_z0 = pr:pos().z end\n"
            "end\n";
        if (!run(src.c_str(), "launching")) return -999.0;
        for (int i = 0; i < 5; ++i) server_tick(0);
        if (!run("rocket_rise = -999\n"
                 "for _, pr in ipairs(dnp.props()) do\n"
                 "  if pr:name() == 'rocket' and rocket_z0 ~= nil then\n"
                 "    rocket_rise = ((pr:pos().z - rocket_z0) * fx.int(256)).int\n"
                 "  end\n"
                 "end\n", "reading the rocket")) return -999.0;
        return num("rocket_rise");
    };
    const double flat_rise = rocket_rise("nil");
    const double up_rise = rocket_rise("fx.int(1)");
    std::printf("  a rocket over five ticks rises %.0f/256 flat and %.0f/256 with a slope of one\n",
                flat_rise, up_rise);
    // CONTROL: no slope, no climb -- the flight every missile in this engine
    // had before today, unchanged.
    CHECK(flat_rise == 0);
    // AND IT CLIMBS. A slope of one is forty-five degrees, so over five ticks
    // it gains exactly what it travelled: a rocket that stored the slope and
    // never spent it would read 0 here and pass every other check in the file.
    CHECK(up_rise > 0);

    // ================================================================
    // A BLAST SHOVES, by mass (mob_vocab.lua `mass`, Doom's thrust): one rocket
    // blast between a zombieman (100) and a demon (400), the same distance
    // either side. The zombie is thrown four times as hard -- and, measured
    // over the next half second, it really went further.
    {
        const auto shove = [&](bool knockback) {
            const std::string src = std::string(R"LUA(
              local p = dnp.players()[1]
              for _, e in ipairs(dnp.entities()) do if e:index() ~= p:index() then e:despawn() end end
              for _, pr in ipairs(dnp.props()) do pr:despawn() end
              mobs.reset()
              local pp = p:pos()
              local c = vec3.new(pp.x + fx.int(10), pp.y, pp.z)
              shove_z = mobs.spawn('zombieman', vec3.new(c.x, c.y + fx.int(1), c.z))
              shove_d = mobs.spawn('demon',     vec3.new(c.x, c.y - fx.int(1), c.z))
              shove_z0, shove_d0 = shove_z:pos(), shove_d:pos()
              local m = mobs.synthetic_owner(p, p:facing())
              m.impact = c
              mobs.acts.damage_area(m, { damage = 1, radius = fx.int(3), falloff = true,
                                          hurts_owner = false, )LUA") +
                (knockback ? "knockback = fx.int(12), lift = fx.int(6)" : "knockback = fx.int(0)") + R"LUA( })
              m.impact = nil
            )LUA";
            if (!run(src.c_str(), "a blast between two bodies")) return false;
            commands.apply(world, [](u32) {});
            if (!run("shove_up = shove_z:vel().z.raw / 4294967296\n"
                     "shove_vz = dnp.length(shove_z:vel()); shove_vd = dnp.length(shove_d:vel())",
                     "their velocities")) return false;
            for (int i = 0; i < 30; ++i) server_tick(0);
            return run("shove_mz = dnp.length(vec3.new(shove_z:pos().x - shove_z0.x, shove_z:pos().y - shove_z0.y, fx.int(0)))\n"
                       "shove_md = dnp.length(vec3.new(shove_d:pos().x - shove_d0.x, shove_d:pos().y - shove_d0.y, fx.int(0)))\n"
                       "shove_vz, shove_vd = shove_vz.raw / 4294967296, shove_vd.raw / 4294967296\n"
                       "shove_mz, shove_md = shove_mz.raw / 4294967296, shove_md.raw / 4294967296", "how far they went");
        };
        if (!shove(true)) return 1;
        const double vz = num("shove_vz"), vd = num("shove_vd"), mz = num("shove_mz"), md = num("shove_md");
        const double up = num("shove_up");
        if (!shove(false)) return 1;
        const double mz0 = num("shove_mz"), up0 = num("shove_up");
        std::printf("  and up: the zombieman at %.2f (no knockback or lift: %.2f)\n", up, up0);
        CHECK(up > 0.5);                          // a blast lifts a monster too
        CHECK(up0 < 0.01);                        // CONTROL: without lift it does not rise
        std::printf("  a blast: zombieman thrown at %.2f, demon at %.2f; after half a second %.2f and %.2f "
                    "units (with no knockback: %.2f)\n", vz, vd, mz, md, mz0);
        CHECK(vz > 0.5 && vd > 0.1);
        CHECK(vz > vd * 3.5 && vz < vd * 4.5);   // mass 100 against 400
        CHECK(mz > md * 2);                       // and it went further
        CHECK(mz > mz0 + 0.3);                    // CONTROL: the shove is what moved it
    }

    // ================================================================
    // A ROCKET JUMP: the real rocket, fired at the floor at your own feet.
    // It bursts ON the floor (it used to sink through it until it met a wall),
    // and its blast throws its owner up and never hurts it (`shoves_owner`).
    // CONTROL: the same rocket fired level flies away, and nobody rises.
    {
        const auto jump = [&](const char* slope) {
            const std::string src = std::string(R"LUA(
              local p = dnp.players()[1]
              for _, e in ipairs(dnp.entities()) do if e:index() ~= p:index() then e:despawn() end end
              for _, pr in ipairs(dnp.props()) do pr:despawn() end
              mobs.reset()
              p:set_hp(100)
              rj_z0 = p:pos().z
              rj_top = rj_z0
              rj_owner = mobs.launch_from(p, 'rocket', p:facing(), fx.frac(1, 2), )LUA") + slope + R"LUA()
            )LUA";
            if (!run(src.c_str(), "a rocket at the floor")) return false;
            for (int i = 0; i < 40; ++i) {
                server_tick(0);
                if (!run("local p = dnp.players()[1]; if p:pos().z > rj_top then rj_top = p:pos().z end",
                         "how high")) return false;
            }
            return run("local p = dnp.players()[1]\n"
                       "rj_rose = (rj_top - rj_z0).raw / 4294967296\n"
                       "rj_hp = p:hp()", "the jump");
        };
        if (!jump("fx.int(-2)")) return 1;
        const double rose = num("rj_rose"), hp = num("rj_hp");
        if (!jump("fx.int(0)")) return 1;
        const double rose0 = num("rj_rose");
        std::printf("  a rocket at your feet: rose %.2f units, hp %d (fired level: rose %.2f)\n",
                    rose, (int)hp, rose0);
        CHECK(rose > 1.0);            // thrown up
        CHECK(hp == 100);             // and not hurt
        CHECK(rose0 < 0.05);          // CONTROL: level, it flew away and nothing happened
    }

    // ================================================================
    // DROPS (mob_vocab.lua `drops`): what a monster leaves where it dies.
    {
        if (!run(R"LUA(
          local p = dnp.players()[1]
          for _, e in ipairs(dnp.entities()) do if e:index() ~= p:index() then e:despawn() end end
          for _, pr in ipairs(dnp.props()) do pr:despawn() end
          mobs.reset()
          local pp = p:pos()
          local z = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(6), pp.y, pp.z))
          drop_at = z:pos()
          mobs.hurt(z, 1000, nil)
          drop_clips, drop_others, drop_far = 0, 0, 0
          for _, pr in ipairs(dnp.props()) do
            if pr:name() == 'ammo_clip' then
              drop_clips = drop_clips + 1
              if dnp.length(vec3.new(pr:pos().x - drop_at.x, pr:pos().y - drop_at.y, fx.int(0))) > fx.int(1) then
                drop_far = drop_far + 1
              end
            else drop_others = drop_others + 1 end
          end
        )LUA", "a zombieman dies")) return 1;
        std::printf("  a zombieman killed left %d clip(s), %d other prop(s)\n",
                    (int)num("drop_clips"), (int)num("drop_others"));
        CHECK(num("drop_clips") == 1);
        CHECK(num("drop_far") == 0);      // at the body
        CHECK(num("drop_others") == 0);   // CONTROL: and nothing else

        // THE WEIGHTED CHOICE, by the numbers: a medikit one time in four.
        // Counted by standing in for dnp.spawn_prop, so 800 rolls do not fill
        // the prop table.
        if (!run(R"LUA(
          local p = dnp.players()[1]
          local real = dnp.spawn_prop
          local seen = {}
          dnp.spawn_prop = function(name, at) seen[#seen + 1] = { name = name, at = at } return nil end
          local rec = { e = p, def = { name = 'test', drops = {
            { one_of = { { item = 'medikit', weight = 1 }, { weight = 3 } } } } } }
          for _ = 1, 800 do mobs.drop_loot(rec) end
          drop_medikits = #seen
          -- CONTROL: the weight is what decides -- three to one the other way.
          seen = {}
          rec.def.drops = { { one_of = { { item = 'medikit', weight = 3 }, { weight = 1 } } } }
          for _ = 1, 800 do mobs.drop_loot(rec) end
          drop_medikits_heavy = #seen
          -- COUNT: three of a thing are three places, round the body.
          seen = {}
          rec.def.drops = { { item = 'ammo_clip', count = 3 } }
          mobs.drop_loot(rec)
          drop_three = #seen
          local spots = {}
          for _, s in ipairs(seen) do spots[tostring(s.at.x.raw) .. ',' .. tostring(s.at.y.raw)] = true end
          drop_spots = 0
          for _ in pairs(spots) do drop_spots = drop_spots + 1 end
          dnp.spawn_prop = real
        )LUA", "rolling the choice")) return 1;
        std::printf("  a medikit at 1:3 dropped %d times in 800, at 3:1 %d; three clips in %d spots\n",
                    (int)num("drop_medikits"), (int)num("drop_medikits_heavy"), (int)num("drop_spots"));
        CHECK(num("drop_medikits") > 140 && num("drop_medikits") < 260);
        CHECK(num("drop_medikits_heavy") > 540 && num("drop_medikits_heavy") < 660);
        CHECK(num("drop_three") == 3 && num("drop_spots") == 3);

        // REFUSED AT LOAD, each with a complaint, and the bad entry not kept.
        if (!run(R"LUA(
          local before = #mobs.problems
          local function check(drops)
            local d = { drops = drops }
            mobs.check_drops(d, 'mob test')
            return d.drops
          end
          drop_bad = 0
          local cases = {
            { { } },                                          -- neither item nor one_of
            { { item = 'ammo_clip', one_of = { { weight = 1 } } } }, -- both
            { { item = 'no_such_thing' } },                   -- not in the catalogue
            { { item = 'ammo_clip', cuont = 2 } },            -- a typo
            { { one_of = { { item = 'medikit', weight = 0 } } } }, -- weight 0
            { { one_of = { } } },                             -- an empty choice
          }
          for _, c in ipairs(cases) do if check(c) == nil then drop_bad = drop_bad + 1 end end
          drop_complaints = #mobs.problems - before
          -- CONTROL: a good table is kept whole, with no complaint.
          local before_good = #mobs.problems
          local good = check({ { item = 'ammo_clip' }, { one_of = { { item = 'medikit', weight = 2 }, { weight = 1 } } } })
          drop_good = (good ~= nil and #good == 2 and #mobs.problems == before_good) and 1 or 0
        )LUA", "refusing bad drops")) return 1;
        std::printf("  %d of 6 bad drop tables refused, with %d complaints\n",
                    (int)num("drop_bad"), (int)num("drop_complaints"));
        CHECK(num("drop_bad") == 6);
        CHECK(num("drop_complaints") >= 6);
        CHECK(num("drop_good") == 1);
    }

    // ================================================================
    // PARITY: THE TWO MACHINES, TICK BY TICK (docs/WEAPON_PREDICTION.md).
    //
    // Every weapon this file has loaded -- Doom's, the charge, the combo, the
    // alt-fire and the airborne ones -- driven through BOTH the Lua machine and
    // the core's from the same start with the same buttons, the same switches,
    // the same running dry and the same refills, and the two traces compared
    // line for line: the weapon in hand, its phase, its vars, every pool, and
    // each action the world was asked to carry out, by which weapon, phase and
    // step. The mobdiff argument: two implementations of one rule are only one
    // rule while something checks they agree.
    if (!run(R"LUA(
      local p = dnp.players()[1]
      local idx = p:index()
      parity_names = {}
      for name in pairs(weapons.defs) do parity_names[#parity_names + 1] = name end
      table.sort(parity_names)
      -- Which sequence entry an action came from, by the table's identity.
      parity_ident = {}
      for wname, def in pairs(weapons.defs) do
        for pname, ph in pairs(def.phases) do
          for i, entry in ipairs(ph.sequence or {}) do
            parity_ident[entry] = wname .. "/" .. pname .. "/" .. i
          end
        end
      end
      parity_events = {}
      for _, name in ipairs({ "hitscan", "launch", "damage_area", "telegraph", "vfx", "sound" }) do
        local real = weapons.acts[name]
        weapons.acts[name] = function(c, a)
          parity_events[#parity_events + 1] = parity_ident[c.entry or a] or ("?" .. name)
          return real(c, a)
        end
      end
      function parity_begin(force_lua)
        weapons.force_lua = force_lua
        weapons.core_stale()
        parity_core = weapons.core_index() ~= nil
        local inv = weapons.get(idx)
        inv.have = {}
        for _, n in ipairs(parity_names) do inv.have[n] = true end
        for pool, a in pairs(weapons.ammo) do inv.ammo[pool] = a.max end
        inv.current = parity_names[1]
        weapons.rearm_player(idx)
        parity_trace = {}
      end
      function parity_tick(t, now)
        local inv = weapons.get(idx)
        -- A switch every 97 ticks, round the whole list; every pool down to 2
        -- at 1500 so weapons run dry and drop to what still fires; refilled
        -- at 2600.
        if t % 97 == 0 then
          inv.current = parity_names[math.floor(t / 97) % #parity_names + 1]
        end
        if t == 1500 then for pool in pairs(weapons.ammo) do inv.ammo[pool] = 2 end end
        if t == 2600 then for pool, a in pairs(weapons.ammo) do inv.ammo[pool] = a.max end end
        parity_events = {}
        weapons.tick_player(p, idx, now)
        local parts = { tostring(inv.current), tostring(weapons.phase_of(idx)) }
        local vars = weapons.vars_of(idx) or {}
        local vk = {}
        for k in pairs(vars) do vk[#vk + 1] = k end
        table.sort(vk)
        for _, k in ipairs(vk) do parts[#parts + 1] = k .. "=" .. tostring(vars[k]) end
        local pk = {}
        for k in pairs(weapons.ammo) do pk[#pk + 1] = k end
        table.sort(pk)
        for _, k in ipairs(pk) do parts[#parts + 1] = k .. ":" .. tostring(inv.ammo[k]) end
        parts[#parts + 1] = "|"
        for _, ev in ipairs(parity_events) do parts[#parts + 1] = ev end
        parity_trace[#parity_trace + 1] = table.concat(parts, " ")
      end
    )LUA", "installing the parity harness")) return 1;
    const int kParityTicks = 3600;
    std::vector<std::string> traces[2];
    bool on_core[2] = {false, false};
    for (int path = 0; path < 2; ++path) {
        const bool force_lua = path == 0;
        if (!run(force_lua ? "parity_begin(true)" : "parity_begin(false)", "starting a parity run")) return 1;
        lua_getglobal(L, "parity_core");
        on_core[path] = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        // The same buttons both times: runs of 1..40 ticks of nothing,
        // primary, secondary or both, from a fixed seed.
        u32 seed = 0x5eedu, left = 0;
        u16 mask = 0;
        for (int t = 0; t < kParityTicks; ++t) {
            if (left == 0) {
                seed = seed * 1664525u + 1013904223u;
                left = 1 + (seed >> 8) % 40;
                const u32 pick = (seed >> 20) % 8;
                mask = pick < 3 ? 0
                     : pick < 6 ? proto::button::ATTACK_PRIMARY
                     : pick < 7 ? proto::button::ATTACK_SECONDARY
                     : static_cast<u16>(proto::button::ATTACK_PRIMARY | proto::button::ATTACK_SECONDARY);
            }
            --left;
            set_buttons(mask);
            const std::string step = "parity_tick(" + std::to_string(t) + ", " +
                                     std::to_string(500000 + t) + ")";
            vm.begin_tick();   // a tick's budget, as the server gives one
            if (!run(step.c_str(), "a parity tick")) return 1;
        }
        set_buttons(0);
        lua_getglobal(L, "parity_trace");
        for (int i = 1; i <= kParityTicks; ++i) {
            lua_rawgeti(L, -1, i);
            traces[path].emplace_back(lua_tostring(L, -1) ? lua_tostring(L, -1) : "");
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    CHECK(!on_core[0] && on_core[1]);
    int first_diff = -1, diffs = 0;
    for (int t = 0; t < kParityTicks; ++t) {
        if (traces[0][t] != traces[1][t]) { ++diffs; if (first_diff < 0) first_diff = t; }
    }
    // What the run covered, so a parity that held over nothing is visible.
    int fired = 0, phases_seen = 0;
    {
        std::vector<std::string> seen;
        for (const std::string& line : traces[1]) {
            const size_t bar = line.find('|');
            if (bar != std::string::npos && bar + 1 < line.size()) ++fired;
            const size_t sp = line.find(' ');
            const std::string wp = line.substr(0, line.find(' ', sp + 1));
            if (std::find(seen.begin(), seen.end(), wp) == seen.end()) seen.push_back(wp);
        }
        phases_seen = static_cast<int>(seen.size());
    }
    std::printf("  parity: %d ticks, %d with actions, %d weapon/phase pairs seen, %d differing\n",
                kParityTicks, fired, phases_seen, diffs);
    if (first_diff >= 0) {
        std::printf("    first at tick %d\n      lua:  %s\n      core: %s\n", first_diff,
                    traces[0][first_diff].c_str(), traces[1][first_diff].c_str());
    }
    CHECK(diffs == 0);
    if (const char* dump = std::getenv("DNP_PARITY_DUMP")) {
        if (FILE* f = std::fopen(dump, "w")) {
            for (int t = 0; t < kParityTicks; ++t) std::fprintf(f, "%d %s\n", t, traces[1][t].c_str());
            std::fclose(f);
        }
    }
    // CONTROL: the run reached many phases and fired often, or agreement is
    // agreement about idling.
    CHECK(fired > 50);
    CHECK(phases_seen > 20);
    // And the machine switched weapons ON ITS OWN -- running dry dropped to
    // one that still fires -- between the switches this run made.
    int auto_switches = 0;
    for (int t = 1; t < kParityTicks; ++t) {
        if (t % 97 == 0) continue;
        const std::string& a = traces[1][t - 1];
        const std::string& b = traces[1][t];
        if (a.substr(0, a.find(' ')) != b.substr(0, b.find(' '))) ++auto_switches;
    }
    std::printf("  parity: %d switches the machine made itself\n", auto_switches);
    CHECK(auto_switches > 0);
    // CONTROL: the comparison can fail. The same trace with one action
    // dropped must differ, or `diffs == 0` is a comparison of nothing.
    {
        std::vector<std::string> broken = traces[1];
        for (std::string& line : broken) {
            const size_t bar = line.find('|');
            if (bar != std::string::npos && bar + 1 < line.size()) { line.resize(bar + 1); break; }
        }
        CHECK(broken != traces[0]);
    }
    if (!run("weapons.force_lua = false\nweapons.core_stale()", "restoring the path")) return 1;

    // ------------------------------------- graphs manage a player's weapons
    std::printf("-- graph weapon nodes (docs/GRAPHS.md \"Weapons\") --\n");
    {
        for (const char* s : { "maps/scripts/graph_vocab.lua", "maps/scripts/graphs.lua" }) {
            if (!vm.run_file(s, s)) { std::printf("  FAIL loading %s: %s\n", s, vm.last_error()); return 1; }
        }
        CHECK(run("gw_ok, gw_err = graphs.load({ graph = 1, name = 'arms', vars = { fired = 0, changed = 0, dry = 0, held = '', shells = -1, has_ssg = true },\n"
                  "  nodes = {\n"
                  "    { id = 'g', type = 'on_call', name = 'arm' },\n"
                  "    { id = 'tk', type = 'take_weapon', weapon = 'supershotgun' },\n"
                  "    { id = 'give', type = 'give_weapon', weapon = 'shotgun', select = true },\n"
                  "    { id = 'ammo', type = 'set_ammo', ammo = 'shells', amount = 2 },\n"
                  "    { id = 'r', type = 'on_call', name = 'read' },\n"
                  "    { id = 'hw', type = 'held_weapon' }, { id = 'sh', type = 'set_var', var = 'held' },\n"
                  "    { id = 'ao', type = 'ammo_of' }, { id = 'sa', type = 'set_var', var = 'shells' },\n"
                  "    { id = 'hs', type = 'has_weapon', weapon = 'supershotgun' }, { id = 'ss', type = 'set_var', var = 'has_ssg' },\n"
                  "    { id = 'f', type = 'when_fired', weapon = 'shotgun' }, { id = 'af', type = 'add_var', var = 'fired', value = 1 },\n"
                  "    { id = 'c', type = 'when_weapon_changed' }, { id = 'ac', type = 'add_var', var = 'changed', value = 1 },\n"
                  "    { id = 'o', type = 'when_out_of_ammo' }, { id = 'ao2', type = 'add_var', var = 'dry', value = 1 } },\n"
                  "  links = { { 'g.then', 'tk.in' }, { 'tk.then', 'give.in' }, { 'give.then', 'ammo.in' },\n"
                  "            { 'r.then', 'sh.in' }, { 'hw.value', 'sh.value' }, { 'sh.then', 'sa.in' }, { 'ao.value', 'sa.value' },\n"
                  "            { 'sa.then', 'ss.in' }, { 'hs.value', 'ss.value' },\n"
                  "            { 'f.then', 'af.in' }, { 'c.then', 'ac.in' }, { 'o.then', 'ao2.in' } } }, 'arms')",
                  "loading the weapon graph"));
        CHECK(run("gw_tostr = tostring(gw_err and table.concat(gw_err, '; ') or '')", "load result"));
        if (!truthy("gw_ok")) std::printf("  load problems: %s\n", str("gw_tostr").c_str());
        CHECK(truthy("gw_ok"));
        set_buttons(0);
        for (int i = 0; i < 3; ++i) server_tick(0);
        CHECK(run("graphs.call('arm', dnp.players()[1], nil)", "arming"));
        for (int i = 0; i < 3; ++i) server_tick(0);
        CHECK(run("graphs.call('read', dnp.players()[1], nil)\n"
                  "gw_held = graphs.var('arms', 'held'); gw_sh = graphs.var('arms', 'shells'); gw_ssg = graphs.var('arms', 'has_ssg')",
                  "reading"));
        std::printf("  held %s, shells %g, has the super shotgun: %s\n", str("gw_held").c_str(), num("gw_sh"),
                    truthy("gw_ssg") ? "yes" : "no");
        CHECK(str("gw_held") == "shotgun" && num("gw_sh") == 2 && !truthy("gw_ssg"));
        CHECK(run("gw_changed = graphs.var('arms', 'changed')", "changed") && num("gw_changed") >= 1);
        // Fire both shells: two shots told, then the weapon runs dry.
        for (int shot = 0; shot < 2; ++shot) {
            for (int i = 0; i < 4; ++i) server_tick(0x0080);
            for (int i = 0; i < 70; ++i) server_tick(0);
        }
        CHECK(run("gw_fired = graphs.var('arms', 'fired'); gw_dry = graphs.var('arms', 'dry')", "fired"));
        std::printf("  fired %g shots, ran dry %g time(s)\n", num("gw_fired"), num("gw_dry"));
        CHECK(num("gw_fired") == 2 && num("gw_dry") == 1);
    }

    // ================================================================
    // A SWING IS AN ATTACK LIKE ANY OTHER (docs/SWINGS.md): a `swing` step with
    // its own numbers -- for a weapon, on both machines, and for a monster --
    // starting at its tick, in its shape, costing its damage.
    std::printf("-- swing steps (docs/SWINGS.md) --\n");
    {
        CHECK(run(R"LUA(
          local before = weapons.load_errors()
          weapons.load {
            weapons = {
              -- Reaches three units, at tick 2 of `slash`; costs 9.
              stepblade = {
                label = "Step Blade", order = 40, initial = "ready",
                phases = {
                  ready = { transitions = { { when = "button_pressed", to = "slash" } } },
                  slash = { ticks = 14, next = "ready", sequence = {
                    { at = 2, action = "swing", damage = 9, reach = "3", ticks = 4,
                      half_arc = "0.25", sweep_from = "0", sweep_to = "0" } } },
                },
              },
              -- The same, reaching one unit.
              shortblade = {
                label = "Short Blade", order = 41, initial = "ready",
                phases = {
                  ready = { transitions = { { when = "button_pressed", to = "slash" } } },
                  slash = { ticks = 14, next = "ready", sequence = {
                    { at = 2, action = "swing", damage = 9, reach = "1", ticks = 4,
                      half_arc = "0.25", sweep_from = "0", sweep_to = "0" } } },
                },
              },
            },
          }
          step_errors = weapons.load_errors() - before
          -- A swing that costs nothing is refused: no damage of its own, and no
          -- `swing` on the weapon or the phase to take one from...
          before = weapons.load_errors()
          weapons.check_weapon("free", { label = "Free", order = 50, initial = "a",
            phases = { a = { sequence = { { at = 0, action = "swing", reach = "2" } } } } })
          free_errors = weapons.load_errors() - before
          -- ...and is not, with one. (CONTROL: the check can pass.)
          before = weapons.load_errors()
          weapons.check_weapon("paid", { label = "Paid", order = 51, initial = "a",
            phases = { a = { sequence = { { at = 0, action = "swing", reach = "2", damage = 1 } } } } })
          paid_errors = weapons.load_errors() - before
        )LUA", "loading the swing-step weapons"));
        std::printf("  swing-step weapons loaded with %d complaints; a free swing %d, a paid one %d\n",
                    (int)num("step_errors"), (int)num("free_errors"), (int)num("paid_errors"));
        CHECK(num("step_errors") == 0);
        CHECK(num("free_errors") == 1);
        CHECK(num("paid_errors") == 0);

        // The vocabulary's `swing` arguments are held to its swing fields: one
        // taken away is a complaint. (CONTROL: none before.)
        CHECK(run("sw_before = weapons.check_vocabulary()\n"
                  "local args = nil\n"
                  "for _, a in ipairs(weapon_vocab.actions) do if a.name == 'swing' then args = a.args end end\n"
                  "sw_removed = table.remove(args)\n"
                  "sw_after = weapons.check_vocabulary()\n"
                  "args[#args + 1] = sw_removed\n", "the swing arguments' parity"));
        CHECK(num("sw_before") == 0);
        CHECK(num("sw_after") >= 1);

        struct Blade { bool core = false; int start = -1, starts = 0, lost = 0, swung_step = -2; };
        const auto blade = [&](const char* weapon, bool force_lua) {
            Blade b;
            const std::string setup =
                std::string("weapons.force_lua = ") + (force_lua ? "true" : "false") + "\n"
                "weapons.core_stale()\n"
                "step_core = weapons.core_index() ~= nil\n"
                "local p = dnp.players()[1]\n"
                "for _, e in ipairs(dnp.entities()) do\n"
                "  if e:index() ~= p:index() then e:despawn() end\n"
                "end\n"
                "mobs.reset()\n"
                "p:teleport(vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
                "local inv = weapons.get(p:index())\n"
                "inv.have['" + std::string(weapon) + "'] = true\n"
                "inv.current = '" + std::string(weapon) + "'\n"
                "weapons.rearm_player(p:index())\n"
                // Two and a half units in front: inside three, outside one.
                "blade_victim = mobs.spawn('zombieman', vec3.new(fx.frac(5,2), fx.int(0), fx.int(0)))\n"
                "blade_victim:set_solid(false)\n"
                "mobs.acts.sleep(mobs.live[blade_victim:index()])\n"
                "mobs.live[blade_victim:index()].hp = 200\n";
            if (!run(setup.c_str(), "arming a blade")) return b;
            lua_getglobal(L, "step_core");
            b.core = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            set_aim(0);
            for (int i = 0; i < 8; ++i) server_tick(0);
            u8 was = 0;
            for (int i = 0; i < 40; ++i) {
                // Pressed and HELD three ticks: the button runs the phases and
                // never swings itself, so a held press is still one swing.
                server_tick(i < 3 ? proto::button::ATTACK_PRIMARY : 0);
                const u8 left = world.agent(player).mv.attack_left;
                if (left > was) { ++b.starts; if (b.start < 0) b.start = i; }
                was = left;
                if (i == 3 && run("blade_ws = dnp.weapon_state(dnp.players()[1])\n"
                                  "blade_step = blade_ws.swung_step\n"
                                  "if blade_ws.swung_weapon < 0 then blade_step = -1 end", "reading the state")) {
                    b.swung_step = static_cast<int>(num("blade_step"));
                }
            }
            if (run("local r = mobs.live[blade_victim:index()]\n"
                    "blade_hp = r and r.hp or -1", "reading the victim")) {
                b.lost = 200 - static_cast<int>(num("blade_hp"));
            }
            return b;
        };
        for (int path = 0; path < 2; ++path) {
            const bool force_lua = path == 0;
            const Blade far = blade("stepblade", force_lua);
            const Blade near = blade("shortblade", force_lua);
            std::printf("  %s machine: a three-unit step swings from tick %d (%d swing(s)) and costs %d;"
                        " a one-unit one from tick %d, costing %d\n",
                        far.core ? "the core's" : "the Lua", far.start, far.starts, far.lost,
                        near.start, near.lost);
            CHECK(far.core == !force_lua);           // the table compiled, swing steps and all
            CHECK(far.start == 2);                   // the step's tick, not the press's
            CHECK(far.starts == 1);                  // the button did not swing as well
            CHECK(far.lost == 9);                    // its own reach, and its own damage
            CHECK(near.start == 2);                  // CONTROL: it swung, at the same tick...
            CHECK(near.lost == 0);                   // ...and missed: the reach was the step's
            if (far.core) CHECK(far.swung_step == 0);   // the core names the step that swung
        }
        CHECK(run("weapons.force_lua = false\nweapons.core_stale()", "back to the core"));

        // A MONSTER: two swings from one body, each in its own shape and for its
        // own damage, and a swing with no arguments back in the declared one.
        CHECK(run("local p = dnp.players()[1]\n"
                  "for _, e in ipairs(dnp.entities()) do\n"
                  "  if e:index() ~= p:index() then e:despawn() end\n"
                  "end\n"
                  "mobs.reset()\n"
                  "p:teleport(vec3.new(fx.int(30), fx.int(30), fx.int(0)))\n"
                  "sd = mobs.spawn('demon', vec3.new(fx.int(0), fx.int(0), fx.int(0)))\n"
                  "sd:set_input(fx.int(0), fx.int(0), 0, 0)\n"
                  "mobs.acts.sleep(mobs.live[sd:index()])\n"
                  "sd_reach0 = sd:base_param('attack_reach').raw\n"
                  "sd_v = mobs.spawn('zombieman', vec3.new(fx.frac(5,2), fx.int(0), fx.int(0)))\n"
                  "sd_v:set_solid(false)\n"
                  "mobs.acts.sleep(mobs.live[sd_v:index()])\n"
                  "mobs.live[sd_v:index()].hp = 200\n", "a demon and a victim out of its reach"));
        recompute();
        for (int i = 0; i < 4; ++i) server_tick(0);
        const auto demon_swing = [&](const char* args) {
            const std::string go =
                std::string("sd_hp0 = mobs.live[sd_v:index()].hp\n"
                            "mobs.acts.swing(mobs.live[sd:index()], ") + args + ")\n";
            if (!run(go.c_str(), "a demon's swing")) return -1;
            for (int i = 0; i < 40; ++i) server_tick(0);
            if (!run("sd_hp1 = mobs.live[sd_v:index()].hp\n"
                     "sd_reach = sd:base_param('attack_reach').raw\n", "reading")) return -1;
            return static_cast<int>(num("sd_hp0") - num("sd_hp1"));
        };
        const int declared = demon_swing("nil");
        const int long_jab = demon_swing("{ reach = fx.int(3), damage = 7, sweep_from = fx.int(0), sweep_to = fx.int(0) }");
        const double long_reach = num("sd_reach");
        const int back_again = demon_swing("nil");
        std::printf("  a demon out of reach: its own bite costs %d, a three-unit jab costs %d, "
                    "its bite again %d (reach %.2f -> %.2f -> %.2f)\n",
                    declared, long_jab, back_again, num("sd_reach0") / 4294967296.0,
                    long_reach / 4294967296.0, num("sd_reach") / 4294967296.0);
        CHECK(declared == 0);                        // CONTROL: the declared reach misses
        CHECK(long_jab == 7);                        // the step's reach, and the step's damage
        CHECK(long_reach == 3.0 * 4294967296.0);     // written into the body for that swing
        CHECK(back_again == 0);                      // and put back for the next
        CHECK(num("sd_reach") == num("sd_reach0"));
    }

    // ================================================================
    // A MISSILE'S TRAIL: the particle kind its `trail` names is on its prop from
    // launch, through the flight, and gone when it bursts -- while the burst
    // still plays -- so every client follows it from one field.
    std::printf("-- a missile's trail --\n");
    {
        CHECK(run("local p = dnp.players()[1]\n"
                  "for _, e in ipairs(dnp.entities()) do\n"
                  "  if e:index() ~= p:index() then e:despawn() end\n"
                  "end\n"
                  "mobs.reset()\n"
                  "mobs.add_missiles({\n"
                  "  smoky = { prop = 'rocket', speed = fx.int(8), life = 200, trail = 13,\n"
                  "            anim_burst = 1, burst_ticks = 15,\n"
                  "            on_hit = { { action = 'damage', amount = 1 } } },\n"
                  "  plain = { prop = 'rocket', speed = fx.int(8), life = 200 },\n"
                  "})\n"
                  "local pp = p:pos()\n"
                  "tr_target = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(4), pp.y, pp.z))\n"
                  "mobs.acts.sleep(mobs.live[tr_target:index()])\n"
                  "mobs.launch_from(p, 'smoky', 0)\n"
                  "mobs.launch_from(p, 'plain', 2147483648)\n"
                  "function tr_read()\n"
                  "  tr_smoky, tr_plain, tr_bursting = -1, -1, false\n"
                  "  for _, s in pairs(mobs.flying) do\n"
                  "    if s.def.trail ~= nil then tr_smoky = s.p:trail(); tr_bursting = s.burst_at ~= nil\n"
                  "    else tr_plain = s.p:trail() end\n"
                  "  end\n"
                  "end\n", "launching a trailed missile and a plain one"));
        server_tick(0);
        CHECK(run("tr_read()", "reading the trails"));
        std::printf("  in flight: the trailed missile's prop says %d, the plain one's %d\n",
                    (int)num("tr_smoky"), (int)num("tr_plain"));
        CHECK(num("tr_smoky") == 13);
        CHECK(num("tr_plain") == 0);                 // CONTROL: a missile with no trail has none
        int bursting_trail = -2;
        for (int i = 0; i < 60 && bursting_trail == -2; ++i) {
            server_tick(0);
            if (run("tr_read()", "reading") && truthy("tr_bursting")) bursting_trail = static_cast<int>(num("tr_smoky"));
        }
        std::printf("  bursting on the zombieman, still there, its prop says %d\n", bursting_trail);
        CHECK(bursting_trail == 0);                  // nothing more comes out of a burst
    }

    // ================================================================
    // mobs.hurt WITH AN ENTITY for the attacker, as a module's on_attack hands
    // one over: it is not a record, and infighting indexed its `e` and threw.
    std::printf("-- hurt with an attacking entity; melee asks for a clear reach --\n");
    {
        CHECK(run("local p = dnp.players()[1]\n"
                  "local pp = p:pos()\n"
                  "hv = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(6), pp.y, pp.z))\n"
                  "ha = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(8), pp.y, pp.z))\n"
                  "mobs.acts.sleep(mobs.live[hv:index()])\n"
                  "mobs.live[hv:index()].hp = 100\n"
                  "hurt_ok, hurt_err = pcall(mobs.hurt, hv, 5, ha)\n"
                  "hurt_err = tostring(hurt_err)\n"
                  "hurt_hp = mobs.live[hv:index()].hp\n"
                  "hurt_target = mobs.live[hv:index()].target\n"
                  "hurt_retarget = (hurt_target ~= nil and hurt_target:index() == ha:index()) and 1 or 0\n"
                  "-- A player's entity: not a mob, so the owner record a shot uses.\n"
                  "hurt_ok2 = pcall(mobs.hurt, hv, 5, p)\n", "hurting with entities"));
        std::printf("  hurt by a zombieman's entity: %s (%s), hp %d, retargeted %d\n",
                    truthy("hurt_ok") ? "ok" : "raised", str("hurt_err").c_str(),
                    (int)num("hurt_hp"), (int)num("hurt_retarget"));
        CHECK(truthy("hurt_ok"));
        CHECK(num("hurt_hp") == 95);
        // Imps infight: the one that hit it is its target now -- the line that threw.
        CHECK(num("hurt_retarget") == 1);
        CHECK(truthy("hurt_ok2"));

        // MELEE asks the map for a clear reach (dnp.can_reach, physics::reach_clear
        // and its own test). Told there is a wall, it does not land; told there is
        // none, the same blow does -- the CONTROL that it reaches at all.
        CHECK(run("local p = dnp.players()[1]\n"
                  "local pp = p:pos()\n"
                  "mh = mobs.spawn('zombieman', vec3.new(pp.x + fx.int(1), pp.y, pp.z))\n"
                  "local m = mobs.live[mh:index()]\n"
                  "mobs.acts.sleep(m)\n"
                  "m.target = p\n"
                  "local real = dnp.can_reach\n"
                  "local hp0 = p:hp()\n"
                  "dnp.can_reach = function() return false end\n"
                  "mobs.acts.melee(m, { damage = 7, range = fx.int(2) })\n"
                  "melee_walled = hp0 - p:hp()\n"
                  "dnp.can_reach = real\n"
                  "local hp1 = p:hp()\n"
                  "melee_clear_real = real(mh, p) and 1 or 0\n"
                  "mobs.acts.melee(m, { damage = 7, range = fx.int(2) })\n"
                  "melee_open = hp1 - p:hp()\n", "a melee told of a wall"));
        std::printf("  melee behind a wall: %d lost; in the open: %d lost (the real reach says %d)\n",
                    (int)num("melee_walled"), (int)num("melee_open"), (int)num("melee_clear_real"));
        CHECK(num("melee_walled") == 0);
        CHECK(num("melee_clear_real") == 1);
        CHECK(num("melee_open") == 7);
    }

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
