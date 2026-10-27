// Movement states defined by data (docs/MOVEMENT.md): the parser and what it
// refuses, a crouch-slide taken in and out by its transitions, a liquid state
// holding a body up, a flag the world honours -- each with the control that
// shows the state, not the default, did it.
#include <cstdio>
#include <cstring>
#include <utility>

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
        "movestate a group floor\ntransition default -> a when depth waist\n",  // not built yet
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
    test_it_is_deterministic();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
