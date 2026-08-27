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

int main() {
    test_turning_takes_time_and_then_stops();
    test_turning_takes_the_short_way();
    test_turn_speed_is_a_parameter();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
