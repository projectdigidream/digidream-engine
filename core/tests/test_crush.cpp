//
// Crushing and horizontal sweep (§3.1, §3.1.3).
//
// Every positive test here ships with a control that must NOT fire, because
// "the engine reported a crush" is worthless on its own -- a detector that
// fired whenever a sector moved would pass every positive case in this file.
// The controls are what distinguish detecting a crush from noticing motion:
//
//   vertical    the SAME motion applied to the whole sector instead of its
//               ceiling alone never crushes, however far or fast it travels,
//               because the gap never closes.
//   horizontal  the SAME footprint sweeping through the SAME agent never
//               touches it when the agent could have stood there anyway.
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
static double D(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

// Two rooms side by side, sharing the boundary at x = 20. Sector 1's floor and
// ceiling are per-test, because whether a sector is solid to a body is exactly
// what these tests are about.
struct Stage {
    MapStorage store;
    MapData    view{};

    Stage(fx s1_floor, fx s1_ceil, fx split, fx east) {
        const fx2 a[] = {{F(0), F(0)}, {split, F(0)}, {split, F(20)}, {F(0), F(20)}};
        const fx2 b[] = {{split, F(0)}, {east, F(0)}, {east, F(20)}, {split, F(20)}};
        store.add_sector(a, 4, F(0), F(60), 1);
        store.add_sector(b, 4, s1_floor, s1_ceil, 2);

        store.add_side(0, 0, 1, NO_SECTOR, 0);
        store.add_side(0, 1, 2, 1, 0);            // east: onto sector 1
        store.add_side(0, 2, 3, NO_SECTOR, 0);
        store.add_side(0, 3, 0, NO_SECTOR, 0);

        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0);
        store.add_side(1, 3, 0, 0, 0);            // west: back into sector 0
    }

    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }

    fx floor_z(u32 s) const { return store.view().sector(s).floor_z; }
    fx ceil_z(u32 s) const { return store.view().sector(s).ceil_z; }
    // West edge of sector 1, which is the face that sweeps.
    fx west_of_1() const { return store.view().point(store.view().sector(1).first_point).x; }
};

// Does this tick's log contain an event of `kind` naming `entity`?
static const CrushEvent* find_event(const World& w, u32 entity, CrushKind kind) {
    for (u32 i = 0; i < w.crushes().count(); ++i) {
        const CrushEvent& e = w.crushes().at(i);
        if (e.entity == entity && e.kind == kind) return &e;
    }
    return nullptr;
}

// ------------------------------------------------------------------ vertical

// A ceiling coming down on a standing body.
//
// The assertion is a RELATIONSHIP, not a tick number: on every single tick, an
// event exists exactly when the gap is smaller than the body, and its depth is
// exactly the shortfall. Pinning the answer to "tick 581" instead would pass
// just as well if the threshold drifted and the schedule drifted with it.
static void test_ceiling_crushes_when_the_gap_closes() {
    std::printf("crush: a descending ceiling crushes exactly when the body no longer fits\n");

    static Stage st(F(0), F(60), F(20), F(40));
    static World w;
    w.reset();
    st.attach(w);

    const u32 victim = w.spawn(fx3{F(30), F(10), F(0)});      // inside sector 1
    const u32 bystander = w.spawn(fx3{F(10), F(10), F(0)});   // next door, safe
    const fx height = w.params(victim).height;

    w.motions().add(st.store, 1);
    // To -59, not -58: a ceiling that stops exactly at body height leaves a gap
    // of precisely 2.0 for a 2.0 body, which does NOT crush -- correctly, and
    // the first version of this test proved only that.
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(-59)},
                                 0, 590, LoopMode::Once, MotionTarget::Ceiling);

    int crushed_ticks = 0;
    int deeper_ticks = 0;
    bool ever_wrong = false;
    fx  last_depth = fx::zero();
    bool depth_never_shrank = true;

    for (u32 t = 0; t < 620; ++t) {
        w.step();

        const fx gap = st.ceil_z(1) - st.floor_z(1);
        const bool should = gap < height;
        const CrushEvent* e = find_event(w, victim, CrushKind::Vertical);

        if ((e != nullptr) != should) ever_wrong = true;
        if (e) {
            ++crushed_ticks;
            if (e->depth != height - gap) ever_wrong = true;
            if (e->sector != 1) ever_wrong = true;
            if (crushed_ticks > 1) {
                if (e->depth < last_depth) depth_never_shrank = false;
                if (e->depth > last_depth) ++deeper_ticks;
            }
            last_depth = e->depth;
        }
        // The bystander is in the room next door the whole time.
        if (find_event(w, bystander, CrushKind::Vertical)) ever_wrong = true;
    }

    CHECK(!ever_wrong);
    CHECK(crushed_ticks > 0);
    std::printf("  crushed on %d of 620 ticks, deepening on %d, final depth %.3f\n",
                crushed_ticks, deeper_ticks, D(last_depth));
    // Deepening while the ceiling travels, then holding once it stops (Once).
    CHECK(depth_never_shrank);
    CHECK(deeper_ticks > 0);
    CHECK(last_depth == w.params(victim).height - (st.ceil_z(1) - st.floor_z(1)));

    // The floor never moved: MotionTarget::Ceiling means one surface travels.
    // Without this the "crush" would just be the whole sector sinking, and the
    // gap would have been constant.
    CHECK(st.floor_z(1) == F(0));
    CHECK(st.ceil_z(1) == F(1));

    // And the body is still between the two surfaces rather than below its own
    // floor -- the mover clamps it, which is why detection reads the geometry.
    CHECK(w.agent(victim).pos.z >= st.floor_z(1));
}

// The control. Same sector, same distance, same schedule -- moved as a body.
static void test_a_whole_sector_moving_never_crushes() {
    std::printf("crush: CONTROL -- the same motion applied to the whole sector never crushes\n");

    static Stage st(F(0), F(60), F(20), F(40));
    static World w;
    w.reset();
    st.attach(w);

    const u32 victim = w.spawn(fx3{F(30), F(10), F(0)});

    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(-59)},
                                 0, 590, LoopMode::Once, MotionTarget::Both);

    u32 events = 0;
    for (u32 t = 0; t < 620; ++t) {
        w.step();
        events += w.crushes().count();
    }

    CHECK(events == 0);
    // It went exactly as far as the crusher did, and carried the rider with it.
    CHECK(st.floor_z(1) == F(-59));
    CHECK(st.ceil_z(1) == F(1));
    CHECK(st.ceil_z(1) - st.floor_z(1) == F(60));
    std::printf("  sector travelled to floor %.1f with %u events\n", D(st.floor_z(1)), events);
    CHECK(w.agent(victim).pos.z == F(-59));
}

// ---------------------------------------------------------------- horizontal

// A solid block sliding across a room.
static void test_a_sweeping_footprint_pushes_and_then_pins() {
    std::printf("sweep: a moving footprint pushes a bystander, then pins it against the wall\n");

    // Sector 1's floor is above step height, so a body cannot be where it is:
    // that, and not the fact that it moves, is what makes it solid.
    static Stage st(F(4), F(60), F(12), F(24));
    static World w;
    w.reset();
    st.attach(w);

    const u32 victim = w.spawn(fx3{F(6), F(10), F(0)});
    const fx radius = w.params(victim).radius;

    w.motions().add(st.store, 1);
    // 12 units west: far enough that the room runs out before the travel does.
    w.motions().start_parametric(1, fx3::zero(), fx3{F(-12), fx::zero(), fx::zero()},
                                 0, 240, LoopMode::Once, MotionTarget::Both);

    bool untouched_before_contact = true;
    bool tracked_the_face = true;
    int  pushed_ticks = 0;
    int  pinned_ticks = 0;
    fx   min_x = F(1000);

    for (u32 t = 0; t < 260; ++t) {
        w.step();
        const fx face = st.west_of_1();
        const fx x = w.agent(victim).pos.x;
        min_x = x < min_x ? x : min_x;

        if (face - radius > F(6)) {
            // The face has not reached the agent yet. A sweep that nudged
            // bystanders would show up here and nowhere else.
            if (x != F(6)) untouched_before_contact = false;
        } else if (face - radius > radius) {
            // Being carried ahead of the face, and resting exactly against it.
            // Exact, not approximate: the push is a normal times a distance
            // whose square root is exact for an axis-aligned wall.
            ++pushed_ticks;
            if (x != face - radius) tracked_the_face = false;
        }
        if (find_event(w, victim, CrushKind::Horizontal)) ++pinned_ticks;
    }

    CHECK(untouched_before_contact);
    CHECK(tracked_the_face);
    CHECK(pushed_ticks > 20);
    CHECK(pinned_ticks > 0);
    std::printf("  pushed for %d ticks to x=%.3f, then pinned for %d\n",
                pushed_ticks, D(min_x), pinned_ticks);

    // Pinned means held against the far wall, not pushed through it.
    CHECK(min_x >= radius);
}

// The control: the identical sweep through the identical agent, with a floor a
// body could stand on.
static void test_a_sweeping_footprint_a_body_fits_in_touches_nothing() {
    std::printf("sweep: CONTROL -- the same footprint sweeps through an agent that fits in it\n");

    static Stage st(F(0), F(60), F(12), F(24));
    static World w;
    w.reset();
    st.attach(w);

    const u32 victim = w.spawn(fx3{F(6), F(10), F(0)});

    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{F(-12), fx::zero(), fx::zero()},
                                 0, 240, LoopMode::Once, MotionTarget::Both);

    u32 events = 0;
    bool moved = false;
    for (u32 t = 0; t < 260; ++t) {
        w.step();
        events += w.crushes().count();
        if (w.agent(victim).pos.x != F(6)) moved = true;
    }

    CHECK(events == 0);
    CHECK(!moved);
    // It really did sweep over the agent -- otherwise this control proves only
    // that the sector stayed home.
    CHECK(st.west_of_1() == F(0));
    std::printf("  footprint swept from x=12 to x=%.1f, agent unmoved, %u events\n",
                D(st.west_of_1()), events);
}

// A rider on a horizontally moving platform is carried, not swept.
static void test_a_rider_is_carried_not_swept() {
    std::printf("sweep: an anchored rider is carried by its platform, never shoved by it\n");

    static Stage st(F(0), F(60), F(12), F(24));
    static World w;
    w.reset();
    st.attach(w);

    // Standing on sector 1 itself.
    const u32 rider = w.spawn(fx3{F(18), F(10), F(0)});

    w.motions().add(st.store, 1);
    w.motions().start_parametric(1, fx3::zero(), fx3{F(-6), fx::zero(), fx::zero()},
                                 0, 120, LoopMode::Once, MotionTarget::Both);

    // One tick to land and anchor.
    w.step();
    CHECK(w.agent(rider).mv.anchored);
    CHECK(w.agent(rider).mv.anchor == 1);

    const fx start_face = st.west_of_1();
    const fx start_x = w.agent(rider).pos.x;

    u32 events = 0;
    for (u32 t = 0; t < 200; ++t) {
        w.step();
        events += w.crushes().count();
    }

    CHECK(events == 0);
    // Carried by exactly what the platform travelled: the rider's offset from
    // the platform is unchanged after 200 ticks, with no drift.
    CHECK(w.agent(rider).pos.x - start_x == st.west_of_1() - start_face);
    std::printf("  platform moved %.2f, rider moved %.2f, %u events\n",
                D(st.west_of_1() - start_face), D(w.agent(rider).pos.x - start_x), events);
}

// ------------------------------------------------------------------ the log

static void test_the_log_is_bounded_and_says_so() {
    std::printf("crush: the log is bounded and reports what it dropped\n");

    CrushLog log;
    for (u32 i = 0; i < MAX_CRUSH_EVENTS + 7; ++i) {
        log.push(CrushEvent{i, 0, CrushKind::Vertical, fx::one()});
    }
    CHECK(log.count() == MAX_CRUSH_EVENTS);
    // Protocol R10's rule locally: an overflow is reported, never swallowed.
    CHECK(log.dropped() == 7);
    CHECK(log.at(0).entity == 0);
    CHECK(log.at(MAX_CRUSH_EVENTS - 1).entity == MAX_CRUSH_EVENTS - 1);

    log.clear();
    CHECK(log.count() == 0);
    CHECK(log.dropped() == 0);
}

static void test_the_log_holds_one_tick_only() {
    std::printf("crush: the log describes one tick, not a backlog\n");

    static Stage st(F(0), F(60), F(20), F(40));
    static World w;
    w.reset();
    st.attach(w);

    w.spawn(fx3{F(30), F(10), F(0)});
    w.motions().add(st.store, 1);
    // Straight to a crushing ceiling and hold there.
    w.motions().start_parametric(1, fx3{fx::zero(), fx::zero(), F(-59)},
                                 fx3{fx::zero(), fx::zero(), F(-59)},
                                 0, 1, LoopMode::Once, MotionTarget::Ceiling);

    w.step();
    CHECK(w.crushes().count() == 1);
    w.step();
    CHECK(w.crushes().count() == 1);   // still 1, not 2

    // Lift the ceiling back out of the way; the log empties on the next step
    // rather than keeping the entity marked crushed forever.
    w.motions().start_parametric(1, fx3::zero(), fx3::zero(), 0, 1, LoopMode::Once,
                                 MotionTarget::Ceiling);
    w.step();
    CHECK(w.crushes().count() == 0);
}

// The events are hashed, so a detection rule that resolved differently on
// another target fails the determinism gate rather than shipping.
static void test_events_reach_the_state_hash() {
    std::printf("crush: the events are part of the hashed world state\n");

    static Stage clean(F(0), F(60), F(20), F(40));
    static Stage crushing(F(0), F(60), F(20), F(40));
    static World a;
    static World b;

    a.reset(); clean.attach(a);
    b.reset(); crushing.attach(b);
    a.spawn(fx3{F(30), F(10), F(0)});
    b.spawn(fx3{F(30), F(10), F(0)});

    a.motions().add(clean.store, 1);
    b.motions().add(crushing.store, 1);
    // Identical everything except which surface moves. Held at the far end from
    // tick zero: a motion whose start_tick is the tick being evaluated is still
    // at `from`, so ramping over one tick would leave both worlds unmoved.
    const fx3 down{fx::zero(), fx::zero(), F(-59)};
    a.motions().start_parametric(1, down, down, 0, 1, LoopMode::Once, MotionTarget::Both);
    b.motions().start_parametric(1, down, down, 0, 1, LoopMode::Once, MotionTarget::Ceiling);

    a.step();
    b.step();
    CHECK(a.crushes().count() == 0);
    CHECK(b.crushes().count() == 1);
    CHECK(a.hash() != b.hash());
}

int main() {
    test_ceiling_crushes_when_the_gap_closes();
    test_a_whole_sector_moving_never_crushes();
    test_a_sweeping_footprint_pushes_and_then_pins();
    test_a_sweeping_footprint_a_body_fits_in_touches_nothing();
    test_a_rider_is_carried_not_swept();
    test_the_log_is_bounded_and_says_so();
    test_the_log_holds_one_tick_only();
    test_events_reach_the_state_hash();

    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
