// Movement states defined by data (docs/MOVEMENT.md): the parser and what it
// refuses, a crouch-slide taken in and out by its transitions, a liquid state
// holding a body up, a flag the world honours -- each with the control that
// shows the state, not the default, did it.
#include <cstdio>
#include <algorithm>
#include <cstring>
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
        "movestate a group floor\ntransition default -> a when meter air empty\n",  // not built yet
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

int main() {
    std::printf("== core/movestates test suite ==\n");
    test_the_parser();
    test_a_crouch_slide();
    test_a_liquid_state_and_a_flag();
    test_wading_and_swimming();
    test_it_is_deterministic();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
