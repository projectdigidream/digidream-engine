//
// Reaching for a side, and crossing one (§3.2.2).
//
// The engine's whole job here is to answer two questions -- "which side did
// this entity reach for" and "which trip-wire did it just cross" -- and to be
// wrong about neither. So every positive case ships with a control that must
// NOT fire, because a detector that reported the nearest side whenever the
// button was held would pass every positive test in this file:
//
//   reach     facing AWAY from the same side at the same distance finds
//             nothing, and standing too far from it finds nothing.
//   edge      HOLDING the button reports once, not once per tick -- a door
//             worked sixty times a second is not a door.
//   crossing  the SAME movement across a side without the TRIGGER flag
//             reports nothing, and moving parallel to a trip-wire without
//             crossing it reports nothing.
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

// Two rooms, 20 wide each, sharing the boundary at x = 20. The shared edge is
// side 1 of sector 0 and side 7 of sector 1.
struct Rooms {
    MapStorage store;
    MapData    view{};

    explicit Rooms(u8 shared_flags = 0) {
        const fx2 a[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 b[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        store.add_sector(a, 4, F(0), F(40), 1);
        store.add_sector(b, 4, F(0), F(40), 2);

        store.add_side(0, 0, 1, NO_SECTOR, 0);
        store.add_side(0, 1, 2, 1, shared_flags);   // side 1: the shared edge, x = 20
        store.add_side(0, 2, 3, NO_SECTOR, 0);
        store.add_side(0, 3, 0, NO_SECTOR, 0);

        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0);
        store.add_side(1, 3, 0, 0, shared_flags);   // side 7: the same edge, other owner
    }

    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// brad is a full turn in 2^32, so east is 0 and west is half of that.
static constexpr brad EAST = brad(0u);
static constexpr brad WEST = brad(0x80000000u);
static constexpr brad NORTH = brad(0x40000000u);

static proto::InputFrame frame(bool interact, brad aim, i16 mx = 0, i16 my = 0) {
    proto::InputFrame f;
    f.buttons = interact ? proto::button::INTERACT : 0;
    f.aim = aim;
    f.move_x = mx;
    f.move_y = my;
    return f;
}

static int count_of(const World& w, TouchKind kind) {
    int n = 0;
    for (u32 i = 0; i < w.interactions().count(); ++i) {
        if (w.interactions().at(i).kind == kind) ++n;
    }
    return n;
}

static i32 side_of(const World& w, TouchKind kind) {
    for (u32 i = 0; i < w.interactions().count(); ++i) {
        if (w.interactions().at(i).kind == kind) {
            return static_cast<i32>(w.interactions().at(i).side);
        }
    }
    return -1;
}

// --------------------------------------------------------------------- reach

static void test_reach_finds_the_side_in_front() {
    std::printf("interact: pressing use names the side the entity faces, and only that one\n");

    static Rooms r;
    static World w;
    w.reset();
    r.attach(w);

    // 1.0 from the shared edge at x = 20, which is inside INTERACT_REACH (1.5).
    const u32 a = w.spawn(fx3{F(19), F(10), F(0)});

    w.set_input(a, frame(true, EAST));
    w.step();
    CHECK(count_of(w, TouchKind::Use) == 1);
    CHECK(side_of(w, TouchKind::Use) == 1);          // sector 0's shared edge

    // CONTROL 1: the same position, the same distance, facing the other way.
    // If this fires, the test above proved only that a side was nearby.
    w.reset();
    r.attach(w);
    const u32 b = w.spawn(fx3{F(19), F(10), F(0)});
    w.set_input(b, frame(true, WEST));
    w.step();
    CHECK(count_of(w, TouchKind::Use) == 0);

    // CONTROL 2: facing the right way, out of reach. 2.0 away against a reach
    // of 1.5 -- computed, not guessed: INTERACT_REACH is 3/2.
    w.reset();
    r.attach(w);
    const u32 c = w.spawn(fx3{F(18), F(10), F(0)});
    w.set_input(c, frame(true, EAST));
    w.step();
    CHECK(count_of(w, TouchKind::Use) == 0);
}

static void test_use_is_edge_triggered() {
    std::printf("interact: holding use reports once, not once per tick\n");

    static Rooms r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(19), F(10), F(0)});

    int reports = 0;
    for (u32 t = 0; t < 30; ++t) {
        w.set_input(a, frame(true, EAST));      // held down the whole time
        w.step();
        reports += count_of(w, TouchKind::Use);
    }
    CHECK(reports == 1);

    // Releasing and pressing again is a second use, or a door could never be
    // shut by the same key that opened it.
    w.set_input(a, frame(false, EAST));
    w.step();
    CHECK(count_of(w, TouchKind::Use) == 0);
    w.set_input(a, frame(true, EAST));
    w.step();
    CHECK(count_of(w, TouchKind::Use) == 1);
}

// ------------------------------------------------------------------ crossing

// Walk east across the x = 20 boundary and report how many crossings the engine
// saw. Shared by the positive case and its control so the ONLY difference
// between them is the flag on the side.
static int walk_across(Rooms& r) {
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(17), F(10), F(0)});

    int crossings = 0;
    i32 last_side = -1;
    for (u32 t = 0; t < 240; ++t) {
        proto::InputFrame f = frame(false, EAST);
        f.move_x = 32767;                       // full east
        w.set_input(a, f);
        w.step();
        const int n = count_of(w, TouchKind::Cross);
        if (n > 0) last_side = side_of(w, TouchKind::Cross);
        crossings += n;
    }
    // The side reported must be one of the two that share the edge, and it must
    // be the one the entity LEFT -- reporting the arrival side too would fire a
    // one-shot trigger twice from a single step across it.
    if (crossings > 0) CHECK(last_side == 1);
    return crossings;
}

static void test_crossing_a_trigger_reports_once() {
    std::printf("interact: crossing a trip-wire reports once; the same walk across a plain side reports nothing\n");

    static Rooms tripwire(side_flag::TRIGGER);
    const int with_flag = walk_across(tripwire);
    CHECK(with_flag == 1);

    // CONTROL: identical geometry, identical walk, no TRIGGER flag. This is the
    // one that proves the detector reads the map rather than noticing that the
    // sector changed.
    static Rooms plain(0);
    CHECK(walk_across(plain) == 0);
}

static void test_moving_parallel_never_crosses() {
    std::printf("interact: walking alongside a trip-wire never trips it\n");

    static Rooms r(side_flag::TRIGGER);
    static World w;
    w.reset();
    r.attach(w);
    // 1 unit west of the boundary, walking north along it for four seconds.
    const u32 a = w.spawn(fx3{F(19), F(2), F(0)});

    int crossings = 0;
    for (u32 t = 0; t < 240; ++t) {
        proto::InputFrame f = frame(false, NORTH);
        f.move_y = 32767;
        w.set_input(a, f);
        w.step();
        crossings += count_of(w, TouchKind::Cross);
    }
    CHECK(crossings == 0);
}

// A trip-wire and a use are different events, and a script that treats one as
// the other opens doors by walking past them. The kinds must not collapse.
static void test_kinds_do_not_collapse() {
    std::printf("interact: a crossing is never reported as a use\n");

    static Rooms r(side_flag::TRIGGER);
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(17), F(10), F(0)});

    int uses = 0;
    for (u32 t = 0; t < 240; ++t) {
        proto::InputFrame f = frame(false, EAST);   // never presses use
        f.move_x = 32767;
        w.set_input(a, f);
        w.step();
        uses += count_of(w, TouchKind::Use);
    }
    CHECK(uses == 0);
}

// A body only fits through an opening taller than itself.
//
// Obvious, and the reason it is asserted anyway: it is the entire justification
// for the map carrying its own body proportions. A Doom map converted at 1/48
// has 56-to-72-unit doorways -- 1.167 to 1.5 of our units -- and the engine's
// default 2.0 body is 96 Doom units. Without the map saying so, every converted
// map is a set of rooms the player cannot walk between, and the symptom ("I get
// stuck in doorways") points nowhere near the cause.
static void test_a_body_fits_only_where_it_fits() {
    std::printf("interact: a Doom-proportioned body clears a Doom doorway and the default body does not\n");

    // Sector 1's ceiling is a 72-unit Doom doorway at 1/48 scale, less Doom's
    // four units of headroom: 1.5 - 4/48 = 1.4167.
    const fx opening = fx::from_frac(3, 2) - fx::from_frac(4, 48);

    auto walks_through = [&](fx body_height) {
        static Rooms r;
        static World w;
        w.reset();
        r.store.sector_mut(1).ceil_z = opening;
        r.attach(w);

        const u32 a = w.spawn(fx3{F(17), F(10), F(0)});
        w.params_mut(a).height = body_height;
        w.params_mut(a).radius = fx::from_frac(1, 3);
        for (u32 t = 0; t < 240; ++t) {
            proto::InputFrame f = frame(false, EAST);
            f.move_x = 32767;
            w.set_input(a, f);
            w.step();
        }
        return w.agent(a).pos.x;
    };

    // Doom's 56-unit player: 1.1667, a quarter of a unit under the opening.
    const fx small = walks_through(fx::from_frac(7, 6));
    // The engine's default: 2.0, which is 96 Doom units.
    const fx big = walks_through(fx::from_int(2));

    std::printf("   1.167 body reached x=%.2f, 2.0 body reached x=%.2f\n",
                static_cast<double>(small.raw) / 4294967296.0,
                static_cast<double>(big.raw) / 4294967296.0);

    // The boundary is at x = 20, so "through" means past it.
    CHECK(small > F(20));
    CHECK(big < F(20));
}

int main() {
    test_reach_finds_the_side_in_front();
    test_a_body_fits_only_where_it_fits();
    test_use_is_edge_triggered();
    test_crossing_a_trigger_reports_once();
    test_moving_parallel_never_crosses();
    test_kinds_do_not_collapse();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
