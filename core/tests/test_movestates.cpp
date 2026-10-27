// Movement states defined by data (docs/MOVEMENT.md): the parser and what it
// refuses, a crouch-slide taken in and out by its transitions, a liquid state
// holding a body up, a flag the world honours -- each with the control that
// shows the state, not the default, did it.
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "dnp/geom/mapstore.hpp"
#include "dnp/physics/movedefs.hpp"
#include "dnp/sim/world.hpp"
#include "dnp/sim/testmap.hpp"

using namespace dnp;
using namespace dnp::sim;
using namespace dnp::physics;

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

static double f(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

static bool parse(MoveDefs& d, const char* text, char* err = nullptr) {
    static char sink[160];
    return parse_move_defs(d, text, static_cast<u32>(std::strlen(text)), err ? err : sink, 160);
}

static const char* SLIDE =
    "# a module's movement\n"
    "player jump_impulse 9          # not ours: skipped\n"
    "transition slide -> default when speed below 3\n"   // before its state: read in a second pass
    "movestate slide group floor speed 14 accel 1 friction 3 anim slide flags no_attack\n"
    "transition default -> slide when grounded pressed crouch speed above 6\n";   // walking is 8

static void test_the_parser() {
    std::printf("movestates: the text, and what it refuses\n");
    static MoveDefs d;
    CHECK(parse(d, SLIDE));
    CHECK(d.state_count == 2 && std::strcmp(d.states[1].name, "slide") == 0 && d.states[1].group == MoveGroup::Floor);
    CHECK((d.states[1].has & move_has::FRICTION) && d.states[1].friction == fx::from_int(3) &&
          !(d.states[1].has & move_has::GRAVITY) && (d.states[1].flags & move_flag::NO_ATTACK));
    CHECK(d.transition_count == 2 && d.transitions[0].from == 1 && d.transitions[0].to == 0 &&
          d.transitions[1].cond_count == 3 && d.transitions[1].cond[1].arg == move_button::CROUCH);
    // Each refusal names its line, and leaves `default` alone -- never half a set.
    const char* bad[] = {
        "movestate slide group floor speeed 3\n",               // a setting nobody has
        "movestate slide speed 3\n",                            // no group
        "movestate default group floor\n",                      // a reserved name
        "movestate a group floor\nmovestate a group air\n",     // twice
        "transition default -> nowhere when grounded\n",        // a state nobody declared
        "movestate a group floor\ntransition default -> a when\n",          // no condition
        "movestate a group floor\ntransition default -> a when depth knees\n",  // no such depth
        "movestate a group floor\ntransition default -> a when meter air empty\n",  // no such meter
        "meter air max 0\n",                                                        // nothing to fill
        "meter air max 10 start 11\n",                                              // more than its max
        "meter air max 10\nmeter air in nowhere rate 1\n",                         // no such state
        "meter air max 10\nmovestate a group floor\ntransition default -> a when grounded do meter air fill 3\n",
        "move dash add sideways 9 for 0.2\n",                                       // no such way
        "move dash set facing 9 for 0\n",                                           // a set with no time
        "move dash add facing 9 for 0.2 cost breath 5\n",                           // no such meter
        "movestate a group floor\ntransition default -> a when grounded do move blink\n",   // no such move
        "meter air max 10\nmeter air in any rate 1 when asked\n",                  // `asked` is a transition's
        "movestate a group floor\ntransition default -> a when touching floor\n",  // only walls
        "movestate a group floor\ntransition default -> a when pressed sneeze\n",
        "movestate a group floor\ntransition default -> a when grounded do dance\n",
    };
    int refused = 0;
    char err[160];
    for (const char* b : bad) {
        if (!parse(d, b, err) && d.state_count == 1 && std::strncmp(err, "line ", 5) == 0) ++refused;
        else std::printf("  not refused: %s", b);
    }
    std::printf("  %d of %d refused, by line; e.g. \"%s\"\n", refused, static_cast<int>(sizeof(bad) / sizeof(bad[0])), err);
    CHECK(refused == static_cast<int>(sizeof(bad) / sizeof(bad[0])));
}

// Run east on the test map's open floor for a second, then let the stick go
// and press CROUCH: the speed it still has after another second.
struct Run { double speed_1s; int state_1s; int state_end; bool went_back; };
static Run run_and_crouch(const char* defs_text) {
    static World w;
    static MoveDefs d;
    w.reset();
    w.set_map(&test_map());
    d = MoveDefs{};
    if (defs_text) parse(d, defs_text);
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(4), fx::from_int(10), fx::zero()});
    Run r{};
    for (u32 t = 0; t < 420; ++t) {
        proto::InputFrame in;
        if (t < 60) in.move_x = 32767;
        if (t >= 60 && t < 63) in.buttons = proto::button::CROUCH;
        w.set_input(a, in);
        w.step();
        if (t == 120) {
            r.speed_1s = f(length(fx2{w.agent(a).vel.x, w.agent(a).vel.y}));
            r.state_1s = w.agent(a).mv.move_state;
        }
        if (t > 120 && w.agent(a).mv.move_state == 0) r.went_back = true;
    }
    r.state_end = w.agent(a).mv.move_state;
    return r;
}

static void test_a_crouch_slide() {
    std::printf("movestates: a crouch-slide, in by its transition and out by another\n");
    const Run slide = run_and_crouch(SLIDE);
    const Run walk = run_and_crouch(nullptr);
    std::printf("  a second after the crouch: sliding %.2f units/s in state %d; walking %.2f; slide over: %d\n",
                slide.speed_1s, slide.state_1s, walk.speed_1s, slide.went_back);
    CHECK(slide.state_1s == 1 && slide.speed_1s > 4);    // still going, in `slide` (8, less 3 a second)
    CHECK(walk.speed_1s < 0.5);                          // CONTROL: without it, a walk stops
    CHECK(slide.went_back && slide.state_end == 0);      // below 3 units/s, back to `default`
}

static void test_a_liquid_state_and_a_flag() {
    std::printf("movestates: a liquid state holds a body up; a state's no_attack stops the swing\n");
    static World w;
    static MoveDefs d;
    parse(d,
          "movestate float group liquid speed 4 accel 8 friction 8 gravity 0 flags no_attack\n"
          "transition default -> float when pressed climb\n");
    auto fall = [&](bool with_state) {
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(with_state ? d : MoveDefs{});
        const u32 a = w.spawn(fx3{fx::from_int(10), fx::from_int(10), fx::from_int(3)});
        double z_after = 0;
        bool swung = false;
        for (u32 t = 0; t < 90; ++t) {
            proto::InputFrame in;
            if (t == 0) in.buttons = proto::button::CLIMB;
            if (t == 40) in.buttons = proto::button::ATTACK_PRIMARY;
            w.set_input(a, in);
            w.step();
            if (w.agent(a).mv.attack_left > 0) swung = true;
            z_after = f(w.agent(a).pos.z);
        }
        return std::make_pair(z_after, swung);
    };
    const auto floated = fall(true);
    const auto fell = fall(false);
    std::printf("  height after 1.5 s: floating %.2f, falling %.2f; swung: %d and %d\n",
                floated.first, fell.first, floated.second, fell.second);
    CHECK(floated.first > 2.9);                  // held up, gravity 0, no stick
    CHECK(fell.first < 0.1);                     // CONTROL: `default` falls to the floor
    CHECK(!floated.second && fell.second);       // no_attack in it; the swing in `default`
}

// A POOL: room 0 (x 0..20, floor 0) opens at x 20 onto room 1 (x 20..40,
// floor -3), and a liquid (x 10..40) stands to 0.75 over both -- a shallow end
// to wade and a deep one to swim, its edge low enough for a floating body (feet
// a waist, 1, under the surface) to step up onto. Sector 2 is the liquid: an effect sector, so
// nothing collides with it, with medium LIQUID; its ceiling is the surface.
struct Pool {
    geom::MapStorage store;
    geom::MapData    view{};
    Pool() {
        const fx F0 = fx::zero();
        const fx2 a[] = {{F0, F0}, {fx::from_int(20), F0}, {fx::from_int(20), fx::from_int(16)}, {F0, fx::from_int(16)}};
        const fx2 b[] = {{fx::from_int(20), F0}, {fx::from_int(40), F0}, {fx::from_int(40), fx::from_int(16)}, {fx::from_int(20), fx::from_int(16)}};
        const fx2 l[] = {{fx::from_int(10), F0}, {fx::from_int(40), F0}, {fx::from_int(40), fx::from_int(16)}, {fx::from_int(10), fx::from_int(16)}};
        store.add_sector(a, 4, F0, fx::from_int(20), 1);
        store.add_sector(b, 4, fx::from_int(-3), fx::from_int(20), 1);
        store.add_sector(l, 4, fx::from_int(-3), fx::from_frac(3, 4), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), i == 1 ? 1 : geom::NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), i == 3 ? 0 : geom::NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(2, i, static_cast<u16>((i + 1) % 4), geom::NO_SECTOR, 0);
        store.sector_mut(2).effect = true;
        store.sector_mut(2).medium = geom::sector_medium::LIQUID;
        store.rebuild_grid();
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

static const char* WATER =
    "movestate wade group floor speed 4 anim wade\n"
    "movestate swim group liquid speed 4 accel 20 friction 3 gravity 0.3 buoyancy 8 anim swim\n"
    "transition default -> wade when depth feet\n"
    "transition any -> swim when depth waist falling\n"
    "transition swim -> wade when grounded depth below waist\n"
    "transition swim -> wade when pressed jump depth below eyes do jump\n"
    "transition wade -> default when depth none\n";

static void test_wading_and_swimming() {
    std::printf("movestates: depth -- wading in the shallows, swimming in the deep, out with a jump\n");
    static Pool pool;
    static World w;
    static MoveDefs d;
    CHECK(parse(d, WATER) && d.uses_depth);
    struct Seen { int shallow = -1; double shallow_speed = 0; int deep = -1; double float_lo = 99, float_hi = -99;
                  double dive = 99; double top = -99; bool stayed = true; int out = -1; double out_x = 0; };
    auto swim = [&](bool liquid, bool jump_out) {
        pool.store.sector_mut(2).medium = liquid ? geom::sector_medium::LIQUID : geom::sector_medium::NONE;
        pool.store.rebuild_grid();
        w.reset();
        pool.attach(w);
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(2), fx::from_int(8), fx::zero()});
        Seen s;
        for (u32 t = 0; t < 900; ++t) {
            proto::InputFrame in;
            if (t < 240) in.move_x = 32767;                         // east: through the shallows, off the edge
            if (t >= 420 && t < 480) in.buttons = proto::button::CROUCH;  // down, a dive
            if (t >= 480 && t < 600) in.buttons = proto::button::JUMP;    // then up, held, from under
            if (t >= 600) in.move_x = -32767;                      // back west, to the edge
            if (jump_out && t >= 690 && t % 30 == 0) in.buttons = proto::button::JUMP;   // and out with a jump
            w.set_input(a, in);
            w.step();
            const auto& g = w.agent(a);
            const double x = f(g.pos.x), z = f(g.pos.z);
            if (t == 100) { s.shallow = g.mv.move_state; s.shallow_speed = f(length(fx2{g.vel.x, g.vel.y})); }
            if (t == 300) s.deep = g.mv.move_state;
            if (t >= 300 && t < 420) { s.float_lo = std::min(s.float_lo, z); s.float_hi = std::max(s.float_hi, z); }
            if (t == 479) s.dive = z;
            if (t >= 480 && t < 600) s.top = std::max(s.top, z);
            if (t >= 300 && t < 600 && g.mv.move_state != 2) s.stayed = false;
            if (t == 899) { s.out = g.mv.move_state; s.out_x = x; }
            (void)x;
        }
        pool.store.sector_mut(2).medium = geom::sector_medium::LIQUID;
        pool.store.rebuild_grid();
        return s;
    };
    const Seen wet = swim(true, true);
    const Seen walked = swim(true, false);
    const Seen dry = swim(false, true);
    std::printf("  shallows: state %d at %.2f units/s; deep: state %d, floating between %.2f and %.2f; "
                "dived to %.2f, up held: top %.2f, swam throughout %d; at the end: state %d at x %.2f\n",
                wet.shallow, wet.shallow_speed, wet.deep, wet.float_lo, wet.float_hi, wet.dive, wet.top, wet.stayed, wet.out, wet.out_x);
    std::printf("  CONTROL, no liquid: shallows state %d at %.2f; deep state %d at %.2f..%.2f\n",
                dry.shallow, dry.shallow_speed, dry.deep, dry.float_lo, dry.float_hi);
    CHECK(wet.shallow == 1 && wet.shallow_speed < 4.5);        // wading, at the wade's speed
    CHECK(dry.shallow == 0 && dry.shallow_speed > 7);          // CONTROL: no liquid, a walk
    CHECK(wet.deep == 2 && wet.float_lo > -2 && wet.float_hi < 0.75);   // swimming, held up near the surface
    CHECK(dry.deep == 0 && dry.float_hi < -2.9);               // CONTROL: no liquid, on the bottom
    CHECK(wet.dive < wet.float_lo - 0.5 && wet.top > wet.dive + 0.5);   // down and up again, under the water
    CHECK(wet.top < 0.75 && wet.stayed);                        // up stops at the surface: no flying out
    std::printf("  out with no jump, paddled to the edge: state %d at x %.2f\n", walked.out, walked.out_x);
    CHECK(wet.out == 0 && wet.out_x < 10);                     // out with a jump, and walked off dry
    CHECK(walked.out == 0 && walked.out_x < 10);               // or up the edge on foot: not left swimming on land
}

// COMING UP FROM A DIVE: movetest's swimmer (friction 1, buoyancy 8) let go
// on the bottom rises on buoyancy alone, and fast -- and the surface stops it.
// Momentum does not throw a swimmer clean out of the water to hang in the air
// still swimming; out is a transition's `do jump`, which still works.
static void test_coming_up_from_a_dive() {
    std::printf("movestates: up from a dive, the surface stops the swimmer; a jump still takes them out\n");
    static Pool pool;
    static World w;
    static MoveDefs d;
    CHECK(parse(d,
        "movestate wade group floor speed 4\n"
        "movestate swim group liquid speed 5 accel 20 friction 1 gravity 0.3 buoyancy 8\n"
        "transition any -> swim when depth waist falling\n"
        "transition swim -> wade when pressed jump depth below eyes do jump\n"
        "transition wade -> default when depth none\n"));
    w.reset();
    pool.attach(w);
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(30), fx::from_int(8), fx::from_frac(-1, 2)});   // in the deep end, falling in
    double top = -99, rise = 0;
    bool swam = true;
    int jumped_at = -1;
    double after_jump = -99;
    for (u32 t = 0; t < 600; ++t) {
        proto::InputFrame in;
        if (t < 90) in.buttons = proto::button::CROUCH;            // dived to the bottom, then let go
        if (t == 480) in.buttons = proto::button::JUMP;            // later, out with a jump
        w.set_input(a, in);
        w.step();
        const auto& g = w.agent(a);
        const double z = f(g.pos.z);
        if (t >= 90 && t < 480) {
            top = std::max(top, z);
            rise = std::max(rise, f(g.vel.z));
            if (g.mv.move_state != 2) swam = false;
        }
        if (t == 480) jumped_at = g.mv.move_state;
        if (t > 480) after_jump = std::max(after_jump, z);
    }
    std::printf("  rose at up to %.2f units/s; feet highest at %.2f (the surface is 0.75); swimming throughout %d; "
                "a jump: state %d, feet to %.2f\n", rise, top, swam, jumped_at, after_jump);
    CHECK(rise > 2);                                // CONTROL: it really came up fast
    CHECK(top < 0.75 && swam);                      // and stayed in the water, swimming
    CHECK(jumped_at == 1 && after_jump > 0.75);     // a jump still leaves it
}

// FLYING WITH INERTIA: a liquid-group state where there is no liquid (depth
// none) goes up when asked. "Up stops at the surface" is a swimmer's rule, for
// a body in the water; with none, there is no surface to stop at.
static void test_a_liquid_group_flies_dry() {
    std::printf("movestates: a liquid-group state out of any liquid rises on JUMP\n");
    static World w;
    static MoveDefs d;
    CHECK(parse(d,
        "movestate fly group liquid speed 4 accel 10 friction 2 gravity 0\n"
        "transition default -> fly when pressed climb\n"
        "transition fly -> default when depth feet\n"));      // depth used, so it is measured
    w.reset();
    w.set_map(&test_map());
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(10), fx::from_int(10), fx::from_int(1)});
    double z0 = 0, z1 = 0;
    for (u32 t = 0; t < 120; ++t) {
        proto::InputFrame in;
        if (t == 0) in.buttons = proto::button::CLIMB;
        if (t >= 10) in.buttons = proto::button::JUMP;
        w.set_input(a, in);
        w.step();
        if (t == 10) z0 = f(w.agent(a).pos.z);
        z1 = f(w.agent(a).pos.z);
    }
    std::printf("  flying, JUMP held: from %.2f to %.2f\n", z0, z1);
    CHECK(w.agent(a).mv.move_state == 1 && z1 > z0 + 2);
}

// BREATH: under the water to the eyes the air drains; above, it comes back;
// out of it the swimmer drowns -- a state with no control, sinking.
static const char* BREATH =
    "movestate swim group liquid speed 4 accel 20 friction 1 gravity 0.3 buoyancy 8\n"
    "movestate drown group none\n"
    "meter air max 10 rate 5\n"
    "meter air in swim rate -2 when depth eyes\n"
    "meter air in drown rate 0\n"
    "transition default -> swim when depth waist falling\n"
    "transition swim -> drown when meter air empty\n";

static void test_breath() {
    std::printf("meters: breath -- drains under the water, comes back above it, and runs out\n");
    static Pool pool;
    static World w;
    static MoveDefs d;
    auto dive = [&](const char* text) {
        CHECK(parse(d, text));
        w.reset();
        pool.attach(w);
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(30), fx::from_int(8), fx::zero()});   // over the deep end
        struct { double start, under, back, last; int state_end; } r{};
        r.start = f(w.agent(a).mv.meters[0]);
        for (u32 t = 0; t < 1500; ++t) {
            proto::InputFrame in;
            if ((t >= 60 && t < 150) || t >= 600) in.buttons = proto::button::CROUCH;   // down; up; down for good
            w.set_input(a, in);
            w.step();
            if (t == 149) r.under = f(w.agent(a).mv.meters[0]);
            if (t == 599) r.back = f(w.agent(a).mv.meters[0]);
        }
        r.last = f(w.agent(a).mv.meters[0]);
        r.state_end = w.agent(a).mv.move_state;
        return r;
    };
    const auto held = dive(BREATH);
    std::string no_rule = BREATH;   // the same, without the rule under the water
    no_rule.erase(no_rule.find("meter air in"), std::strlen("meter air in swim rate -2 when depth eyes\n"));
    const auto free = dive(no_rule.c_str());
    std::printf("  air: %.2f at the start, %.2f after 1.5 s down, %.2f after 7.5 s up, %.2f at the end in state %d\n",
                held.start, held.under, held.back, held.last, held.state_end);
    std::printf("  CONTROL, no rule for under the water: %.2f, %.2f, %.2f, state %d\n",
                free.under, free.back, free.last, free.state_end);
    CHECK(held.start == 10 && held.under < 7 && held.under > 0);     // down there 2 a second goes
    CHECK(held.back == 10);                                           // back, at +5, and no more than the max
    CHECK(held.last == 0 && held.state_end == 2);                     // ran out: drowning, and its own rate 0
    CHECK(free.under == 10 && free.last == 10 && free.state_end == 1);   // CONTROL: it never drains
}

// STAMINA: a sprint that costs 10 to start and drains 30 a second, back to
// walking when it is gone, while walking refills it.
static void test_stamina() {
    std::printf("meters: stamina -- a sprint that costs it, and stops when it runs out\n");
    static World w;
    static MoveDefs d;
    CHECK(parse(d,
                "movestate sprint group floor speed 14 accel 40\n"
                "meter stamina max 100 rate 20\n"
                "meter stamina in sprint rate -30\n"
                "transition default -> sprint when held dash meter stamina above 10 do meter stamina add -10\n"
                "transition sprint -> default when meter stamina empty\n"
                "transition sprint -> default when released dash\n"));
    w.reset();
    w.set_map(&test_map());
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(2), fx::from_int(10), fx::zero()});
    double first = 0, at_1s = 0, speed_1s = 0;
    int state_1s = -1, out_at = -1;
    for (u32 t = 0; t < 300; ++t) {
        proto::InputFrame in;
        in.move_x = t % 120 < 60 ? 32767 : -32767;   // back and forth, the test map is not wide
        in.buttons = proto::button::DASH;
        w.set_input(a, in);
        w.step();
        const auto& g = w.agent(a);
        if (t == 0) first = f(g.mv.meters[0]);
        if (t == 60) { at_1s = f(g.mv.meters[0]); state_1s = g.mv.move_state; speed_1s = f(length(fx2{g.vel.x, g.vel.y})); }
        if (out_at < 0 && t > 0 && g.mv.move_state == 0) out_at = static_cast<int>(t);
    }
    std::printf("  stamina %.2f after the start's cost, %.2f a second on (state %d at %.2f units/s); out of it at tick %d\n",
                first, at_1s, state_1s, speed_1s, out_at);
    CHECK(first < 90 && first > 89);                  // 100, less 10 to start, less a tick's drain
    CHECK(state_1s == 1 && at_1s < 61 && at_1s > 58); // sprinting, 30 a second gone
    CHECK(out_at > 170 && out_at < 190);              // 90 / 30 = 3 s, then back to walking
}

// LAYERED MOVES. A dash that holds the level at 18 for 0.2 s and costs 20
// stamina, on USE (DASH would also start the built-in dash); a pad, up; and a
// boost a transition starts as a slide begins.
struct Dash { double speed_in, speed_after, stamina; double peak; };
static Dash dash_with(const char* text) {
    static World w;
    static MoveDefs d;
    CHECK(parse(d, text));
    w.reset();
    w.set_map(&test_map());
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(4), fx::from_int(10), fx::zero()});
    Dash r{};
    for (u32 t = 0; t < 120; ++t) {
        proto::InputFrame in;
        in.move_x = t < 60 ? 32767 : 0;
        if (t == 30) in.buttons = proto::button::INTERACT;
        if (t == 80) in.buttons = proto::button::CLIMB;
        w.set_input(a, in);
        w.step();
        const auto& g = w.agent(a);
        if (t == 36) r.speed_in = f(length(fx2{g.vel.x, g.vel.y}));
        if (t == 50) { r.speed_after = f(length(fx2{g.vel.x, g.vel.y})); r.stamina = f(g.mv.meters[0]); }
        if (t > 80) r.peak = std::max(r.peak, f(g.pos.z));
    }
    return r;
}

static void test_layered_moves() {
    std::printf("moves: a dash that costs, a pad that lifts, a boost a transition starts\n");
    const char* MOVES =
        "meter stamina max 100 rate 0\n"
        "move dash set facing 18 for 0.2 cost stamina 20 when pressed use\n"
        "move pad set up 9 for 0.1 when grounded pressed climb\n";
    const Dash on = dash_with(MOVES);
    std::string poor = MOVES;
    poor.replace(poor.find("rate 0"), 6, "start 10");   // not enough for the dash's 20
    const Dash broke = dash_with(poor.c_str());
    std::printf("  dashing: %.2f units/s, %.2f after it, stamina %.0f; pad peak %.2f\n", on.speed_in, on.speed_after, on.stamina, on.peak);
    std::printf("  CONTROL, 10 stamina: %.2f units/s, stamina %.0f\n", broke.speed_in, broke.stamina);
    CHECK(on.speed_in > 17.9 && on.speed_in < 18.1);    // held at 18
    CHECK(on.speed_after < 8.1);                        // and over: walking pace again
    CHECK(on.stamina == 80);                            // paid for
    CHECK(broke.speed_in < 8.1 && broke.stamina == 10); // CONTROL: too dear, no dash, nothing paid
    CHECK(on.peak > 0.5);                               // the pad lifts: 9 up for 0.1 s, then a fall

    // A boost started by the transition into a slide: `add`, so the level
    // keeps the speed it had plus the boost while it runs, untouched by the
    // slide's friction.
    auto slide = [&](bool boost) {
        static World w;
        static MoveDefs d;
        std::string text = std::string("movestate slide group floor speed 14 accel 1 friction 3\n"
                                        "move boost add facing 6 for 0.3\n") +
                           (boost ? "transition default -> slide when grounded pressed crouch do move boost\n"
                                  : "transition default -> slide when grounded pressed crouch\n");
        CHECK(parse(d, text.c_str()));
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(4), fx::from_int(10), fx::zero()});
        double at = 0;
        for (u32 t = 0; t < 80; ++t) {
            proto::InputFrame in;
            in.move_x = t < 60 ? 32767 : 0;
            if (t == 60) in.buttons = proto::button::CROUCH;
            w.set_input(a, in);
            w.step();
            if (t == 75) at = f(length(fx2{w.agent(a).vel.x, w.agent(a).vel.y}));
        }
        return at;
    };
    const double boosted = slide(true), plain = slide(false);
    std::printf("  a quarter second into the slide: boosted %.2f units/s, CONTROL plain %.2f\n", boosted, plain);
    CHECK(boosted > 13.9 && boosted < 14.1);   // 8 + 6, held: no friction while it runs
    CHECK(plain < 8);                          // CONTROL: the slide's friction at work
}

// WHAT SCRIPTS DO, and `touching wall`. A script asks for `stunned`, which
// a transition saying `asked` takes; sets a meter (clamped); starts a move. And
// a body that jumps into the west wall clings to it.
static void test_scripts_and_walls() {
    std::printf("scripts and walls: asked, set_meter, start_move, touching wall\n");
    static World w;
    static MoveDefs d;
    CHECK(parse(d,
                "movestate stunned group none\n"
                "movestate cling group air speed 2 gravity 0.1\n"
                "movestate never group none\n"
                "meter air max 10 start 10\n"
                "move shove set back 10 for 0.1\n"
                "transition default -> stunned when asked\n"
                "transition stunned -> default when after 0.5\n"
                "transition default -> cling when airborne touching wall\n"));
    w.reset();
    w.set_map(&test_map());
    w.set_move_defs(d);
    const u32 a = w.spawn(fx3{fx::from_int(20), fx::from_int(10), fx::zero()});
    auto step = [&](u32 n, proto::InputFrame in = {}) { for (u32 t = 0; t < n; ++t) { w.set_input(a, in); w.step(); } };
    step(10);
    const int before = w.agent(a).mv.move_state;
    CHECK(w.ask_movement_state(a, 1));
    step(1);
    const int asked_in = w.agent(a).mv.move_state;
    const int asked_left = w.agent(a).mv.asked;
    step(40);
    const int back = w.agent(a).mv.move_state;
    // A state nobody's transition takes when asked: the ask waits, nothing moves.
    CHECK(w.ask_movement_state(a, 3));
    step(10);
    const int waited_state = w.agent(a).mv.move_state, waited = w.agent(a).mv.asked;
    std::printf("  asked for stunned: %d -> %d (ask left %d), back to %d after 0.5 s; asked for `never`: state %d, ask %d\n",
                before, asked_in, asked_left, back, waited_state, waited);
    CHECK(before == 0 && asked_in == 1 && asked_left == 0 && back == 0);
    CHECK(waited_state == 0 && waited == 4);
    CHECK(!w.ask_movement_state(a, 9));                 // no such state

    CHECK(w.set_meter(a, 0, fx::from_frac(7, 2)) && w.agent(a).mv.meters[0] == fx::from_frac(7, 2));
    CHECK(w.set_meter(a, 0, fx::from_int(99)) && w.agent(a).mv.meters[0] == fx::from_int(10));   // clamped
    CHECK(!w.set_meter(a, 1, fx::zero()));              // no such meter

    // Facing east (aim 0), so `back` is west.
    CHECK(w.start_move(a, 0));
    step(3);
    const double vx = f(w.agent(a).vel.x);
    std::printf("  start_move(shove): x velocity %.2f\n", vx);
    CHECK(vx < -9.9 && vx > -10.1);
    CHECK(!w.start_move(a, 1));                          // no such move

    // Into the west wall, jumping: cling. CONTROL: the same jump east, in the open.
    auto jump_at = [&](i16 way) {
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(d);
        const u32 b = w.spawn(fx3{fx::from_int(way < 0 ? 2 : 20), fx::from_int(10), fx::zero()});
        bool clung = false;
        for (u32 t = 0; t < 60; ++t) {
            proto::InputFrame in;
            in.move_x = way;
            if (t == 2) in.buttons = proto::button::JUMP;
            w.set_input(b, in);
            w.step();
            if (w.agent(b).mv.move_state == 2) clung = true;
        }
        return clung;
    };
    const bool wall = jump_at(-32767), open = jump_at(32767);
    std::printf("  jumping into the west wall: clung %d; CONTROL, east in the open: %d\n", wall, open);
    CHECK(wall && !open);
}

static void test_it_is_deterministic() {
    std::printf("movestates: the same inputs, the same states and hash\n");
    auto once = []() {
        static World w;
        static MoveDefs d;
        parse(d, SLIDE);
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(4), fx::from_int(10), fx::zero()});
        for (u32 t = 0; t < 200; ++t) {
            proto::InputFrame in;
            in.move_x = t < 60 ? 32767 : 0;
            if (t == 60) in.buttons = proto::button::CROUCH;
            w.set_input(a, in);
            w.step();
        }
        return w.agent_hash(a);
    };
    CHECK(once() == once());
}

// SWITCHING THEM OFF (docs/MOVEMENT.md): a state that starts `off` is never
// entered until a script turns it on; switched off while in it, the body is
// back in `default` the next tick; a move switched off does not start, and a
// running one stops. Each with its control: the same input, switched on.
static void test_switching_off() {
    std::printf("movestates: switched off, a state is not entered and a move does not start\n");
    static World w;
    static MoveDefs d;
    CHECK(parse(d,
        "movestate glide group air speed 8 accel 6 gravity 0.1 flags off\n"
        "transition default -> glide when falling held jump\n"
        "transition glide -> default when grounded\n"
        "move dash set facing 18 for 0.5 when pressed dash\n"
        "move boost add facing 4 for 0.25 off\n"));
    CHECK(d.states[1].flags & move_flag::OFF);
    CHECK(d.moves[1].off && !d.moves[0].off);
    // Jump and hold it: the glide, if it may be had.
    auto jump_and_hold = [&](bool glide_on) {
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(10), fx::from_int(10), fx::zero()});
        const u8 started_off = w.agent(a).mv.states_off;
        if (glide_on) w.set_movement_state_enabled(a, 1, true);
        int glided = 0;
        for (u32 t = 0; t < 60; ++t) {
            proto::InputFrame in;
            in.buttons = proto::button::JUMP;
            w.set_input(a, in);
            w.step();
            if (w.agent(a).mv.move_state == 1) ++glided;
        }
        return std::make_pair(started_off, glided);
    };
    const auto locked = jump_and_hold(false);
    const auto found = jump_and_hold(true);
    std::printf("  spawned with states_off %u; held jump: glided %d ticks locked, %d once on\n",
                locked.first, locked.second, found.second);
    CHECK(locked.first == 1 && locked.second == 0);    // `flags off`: never entered
    CHECK(found.second > 10);                          // CONTROL: switched on, the same jump glides
    // Switched off mid-glide: back in `default` on the next tick.
    {
        const u32 a = 0;
        CHECK(w.agent(a).mv.move_state == 1);
        CHECK(w.set_movement_state_enabled(a, 1, false));
        proto::InputFrame in;
        in.buttons = proto::button::JUMP;
        w.set_input(a, in);
        w.step();
        CHECK(w.agent(a).mv.move_state == 0);
        CHECK(!w.ask_movement_state(a, 1));            // nor may a script ask for it now
        CHECK(!w.set_movement_state_enabled(a, 0, false));   // `default` is not a switch
    }
    // A move: DASH starts `dash`; switched off it does not, and one running stops.
    {
        w.reset();
        w.set_map(&test_map());
        w.set_move_defs(d);
        const u32 a = w.spawn(fx3{fx::from_int(10), fx::from_int(10), fx::zero()});
        CHECK(w.agent(a).mv.moves_off == 2);           // `boost` starts off
        CHECK(!w.start_move(a, 1));                    // and nothing starts it
        CHECK(w.set_move_enabled(a, 1, true) && w.start_move(a, 1));   // CONTROL: switched on, it does
        auto press_dash = [&]() {
            proto::InputFrame in;
            in.buttons = proto::button::DASH;
            w.set_input(a, in);
            w.step();
            const bool running = w.agent(a).mv.moves[0].move == 1;
            w.set_input(a, proto::InputFrame{});
            w.step();
            return running;
        };
        const bool on = press_dash();
        w.set_move_enabled(a, 0, false);
        w.step();
        const bool stopped = w.agent(a).mv.moves[0].move == 0;
        for (int t = 0; t < 40; ++t) w.step();
        const bool off = press_dash();
        std::printf("  dash on: ran %d; switched off while running: stopped %d; pressed again off: ran %d\n", on, stopped, off);
        CHECK(on && stopped && !off);
    }
}

int main() {
    std::printf("== core/movestates test suite ==\n");
    test_the_parser();
    test_a_crouch_slide();
    test_a_liquid_state_and_a_flag();
    test_wading_and_swimming();
    test_coming_up_from_a_dive();
    test_a_liquid_group_flies_dry();
    test_breath();
    test_stamina();
    test_layered_moves();
    test_scripts_and_walls();
    test_switching_off();
    test_it_is_deterministic();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
