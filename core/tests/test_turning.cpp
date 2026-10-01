//
// Turning is a rate, not a snap (§7.1).
//
// An entity that faces a new direction the instant its input changes has no
// weight and cannot be outmanoeuvred, and every aimed thing it does -- a swing,
// a reach for a switch -- lands wherever the stick was rather than where the
// body was pointing.
//
// The properties worth pinning are the ones that are easy to get almost right:
// it must MOVE (not snap), it must ARRIVE (not orbit), it must take the SHORT
// way round including across the wrap, and it must not overshoot.
//
#include <cstdio>

#include "dnp/sim/world.hpp"

using namespace dnp;
using namespace dnp::geom;
using namespace dnp::sim;

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

// One flat room, so nothing but turning is under test.
struct Room {
    MapStorage store;
    MapData    view{};
    Room() {
        const fx2 p[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
        store.add_sector(p, 4, F(0), F(40), 1);
        for (u16 i = 0; i < 4; ++i) {
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        }
    }
    void attach(World& w) { view = store.view(); w.set_map(&view); w.set_map_storage(&store); }
};

// Turns, as a double, for reporting only.
static double turns_of(brad a) { return static_cast<double>(a.raw) / 4294967296.0; }

// Aim at `target` for `ticks` and report where the facing ended up.
static brad aim_for(World& w, u32 a, brad target, u32 ticks) {
    for (u32 t = 0; t < ticks; ++t) {
        proto::InputFrame f;
        f.aim = target;
        w.set_input(a, f);
        w.step();
    }
    return w.agent(a).facing;
}

static void test_turning_takes_time_and_then_stops() {
    std::printf("turning: a body turns at a rate, arrives, and does not overshoot\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    const brad target = brad(0x40000000u);   // a quarter turn from 0

    // ONE tick is not enough. At 4 turns/s a tick is 1/15 of a turn, so a
    // quarter turn takes four of them.
    const brad after_one = aim_for(w, a, target, 1);
    CHECK(after_one.raw != target.raw);
    CHECK(after_one.raw != 0);               // but it did move

    // Given time it arrives exactly, and STAYS there -- an implementation that
    // stepped by a fixed amount without clamping would orbit the target
    // forever, which looks like a jitter nobody can place.
    const brad arrived = aim_for(w, a, target, 60);
    CHECK(arrived.raw == target.raw);
    const brad still = aim_for(w, a, target, 60);
    CHECK(still.raw == target.raw);

    std::printf("   after 1 tick: %.4f turns; after 60: %.4f\n",
                turns_of(after_one), turns_of(arrived));
}

// The short way round, including across the wrap where a naive comparison of
// two unsigned angles goes the long way.
static void test_turning_takes_the_short_way() {
    std::printf("turning: the short way round, including across zero\n");

    static Room r;
    static World w;

    // From just below a full turn to just above zero: 1/16 of a turn forwards,
    // or 15/16 backwards. A body that went backwards would spin most of the way
    // round to reach a direction a sixteenth away.
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    w.agent_mut(a).facing = brad(0xF8000000u);          // 31/32 of a turn
    const brad fwd = aim_for(w, a, brad(0x04000000u), 1);   // 1/32 of a turn
    // One tick at 4 turns/s covers 1/15 of a turn, which is more than the 1/16
    // gap -- so it should ARRIVE, having gone forwards through zero.
    CHECK(fwd.raw == 0x04000000u);

    // And the same in the other direction.
    w.reset();
    r.attach(w);
    const u32 b = w.spawn(fx3{F(20), F(20), F(0)});
    w.agent_mut(b).facing = brad(0x04000000u);
    const brad back = aim_for(w, b, brad(0xF8000000u), 1);
    CHECK(back.raw == 0xF8000000u);

    // CONTROL: a HALF turn is the same distance either way and must take many
    // ticks. If this arrived in one, the step would not be bounded at all.
    w.reset();
    r.attach(w);
    const u32 c = w.spawn(fx3{F(20), F(20), F(0)});
    const brad half = aim_for(w, c, brad(0x80000000u), 1);
    CHECK(half.raw != 0x80000000u);
}

static void test_turn_speed_is_a_parameter() {
    std::printf("turning: the rate is a parameter, not a constant\n");

    auto ticks_to_face = [](fx speed, brad target) {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
        w.params_mut(a).turn_speed = speed;
        for (u32 t = 0; t < 600; ++t) {
            proto::InputFrame f;
            f.aim = target;
            w.set_input(a, f);
            w.step();
            if (w.agent(a).facing.raw == target.raw) return static_cast<i32>(t) + 1;
        }
        return -1;
    };

    const brad quarter = brad(0x40000000u);
    const i32 slow = ticks_to_face(fx::from_frac(1, 2), quarter);   // half a turn per second
    const i32 fast = ticks_to_face(F(4), quarter);
    std::printf("   0.5 turns/s took %d ticks; 4 turns/s took %d\n", slow, fast);
    CHECK(slow > 0 && fast > 0);
    CHECK(slow > fast);

    // A rate large enough to cover a whole turn in one tick IS a snap, which is
    // how a script asks for the old behaviour rather than by a separate flag.
    CHECK(ticks_to_face(F(120), quarter) == 1);
}

// FIRST-PERSON MOVEMENT (MoveParams::move_relative): the push is turned by the
// body's facing -- y forward, x a sidestep to the right -- and the vertical aim
// is taken from the input and clamped. Each claim is against the same body
// with the parameter off, so a test that passed because nothing moved fails.
static void test_move_relative_and_pitch() {
    std::printf("turning: first-person movement walks where the body faces\n");
    // Walk `ticks` with a push, facing `face` (an instant turn), from the middle.
    const auto walk = [](bool relative, brad face, i16 mx, i16 my, i16 pitch = 0) {
        // Static, both: a MapStorage is too big for the stack since the caps rose.
        static Room room;
        static World w;
        w.reset();
        room.attach(w);
        const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
        w.params_mut(a).turn_speed = F(1000);   // a snap: facing = aim
        w.params_mut(a).move_relative = relative;
        for (int t = 0; t < 40; ++t) {
            proto::InputFrame f;
            f.aim = face;
            f.move_x = mx;
            f.move_y = my;
            f.pitch = pitch;
            w.set_input(a, f);
            w.step();
        }
        struct R { double dx, dy; i16 pitch; };
        const AgentState& s = w.agent(a);
        return R{static_cast<double>((s.pos.x - F(20)).raw) / 4294967296.0,
                 static_cast<double>((s.pos.y - F(20)).raw) / 4294967296.0, s.pitch};
    };
    const brad north{0x40000000u};   // a quarter turn: +y
    // Facing north, forward goes north.
    const auto fwd = walk(true, north, 0, 32767);
    std::printf("  facing north, forward: moved (%.2f, %.2f)\n", fwd.dx, fwd.dy);
    CHECK(fwd.dy > 2.0 && fwd.dx > -0.01 && fwd.dx < 0.01);
    // CONTROL: the same push with the parameter off is a world-space push north
    // too (y is north on the stick) -- so turn to EAST and it must differ.
    const brad east{0};
    const auto fwd_e = walk(true, east, 0, 32767);
    CHECK(fwd_e.dx > 2.0 && fwd_e.dy > -0.01 && fwd_e.dy < 0.01);
    const auto world_e = walk(false, east, 0, 32767);
    CHECK(world_e.dy > 2.0);   // off: the stick's y is the world's y, whatever the facing
    // Facing north, right is east: clockwise of the facing.
    const auto right = walk(true, north, 32767, 0);
    std::printf("  facing north, right: moved (%.2f, %.2f)\n", right.dx, right.dy);
    CHECK(right.dx > 2.0 && right.dy > -0.01 && right.dy < 0.01);
    // PITCH: what the input says, clamped to MAX_PITCH, and zero by default.
    CHECK(walk(true, north, 0, 0, 5000).pitch == 5000);
    CHECK(walk(true, north, 0, 0, 32000).pitch == proto::MAX_PITCH);
    CHECK(walk(true, north, 0, 0, -32000).pitch == -proto::MAX_PITCH);
    CHECK(walk(false, north, 0, 0).pitch == 0);
}

int main() {
    test_turning_takes_time_and_then_stops();
    test_turning_takes_the_short_way();
    test_turn_speed_is_a_parameter();
    test_move_relative_and_pitch();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
