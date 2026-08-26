//
// Geometry predicates and the four collision rules of design §7.1.
//
// Each rule gets its own scenario with a hand-checkable expected outcome,
// because "the agent ended up somewhere plausible" is not a test. The step
// rules in particular are easy to write in a way that looks right and silently
// does nothing.
//
#include <cstdio>

#include "dnp/geom/map.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/physics/collide.hpp"

using namespace dnp;
using namespace dnp::geom;
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

static fx F(i64 n) { return fx::from_int(n); }
static fx Ff(i64 n, i64 d) { return fx::from_frac(n, d); }

// ---------------------------------------------------------------- geometry

static void test_point_in_sector() {
    std::printf("geom: point-in-sector is exact on boundaries and vertices\n");

    // Unit square, counter-clockwise.
    static const fx2 pts[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    static const Sector secs[] = {{0, 4, 0, 0, F(0), F(8), 0}};
    MapData map{pts, 4, secs, 1, nullptr, 0};

    CHECK(point_in_sector(map, 0, fx2{F(5), F(5)}));
    CHECK(!point_in_sector(map, 0, fx2{F(15), F(5)}));
    CHECK(!point_in_sector(map, 0, fx2{F(-1), F(5)}));
    CHECK(!point_in_sector(map, 0, fx2{F(5), F(15)}));

    // A point exactly level with two vertices must not be double-counted. The
    // half-open vertical rule is what makes this deterministic rather than a
    // coin flip that could differ between client and server.
    CHECK(point_in_sector(map, 0, fx2{F(5), F(0)}) != point_in_sector(map, 0, fx2{F(5), F(-1)}));
    CHECK(!point_in_sector(map, 0, fx2{F(-5), F(0)}));
    CHECK(!point_in_sector(map, 0, fx2{F(15), F(10)}));

    // Concave polygon: an L shape. The notch must read as outside.
    static const fx2 lpts[] = {
        {F(0), F(0)}, {F(10), F(0)}, {F(10), F(4)}, {F(4), F(4)}, {F(4), F(10)}, {F(0), F(10)}
    };
    static const Sector lsecs[] = {{0, 6, 0, 0, F(0), F(8), 0}};
    MapData lmap{lpts, 6, lsecs, 1, nullptr, 0};

    CHECK(point_in_sector(lmap, 0, fx2{F(2), F(2)}));
    CHECK(point_in_sector(lmap, 0, fx2{F(8), F(2)}));
    CHECK(point_in_sector(lmap, 0, fx2{F(2), F(8)}));
    CHECK(!point_in_sector(lmap, 0, fx2{F(8), F(8)}));   // the notch
}

static void test_segment_math() {
    std::printf("geom: closest-point and distance-to-segment\n");
    const fx2 a{F(0), F(0)};
    const fx2 b{F(10), F(0)};

    CHECK(closest_point_on_segment(a, b, fx2{F(5), F(3)}) == (fx2{F(5), F(0)}));
    CHECK(closest_point_on_segment(a, b, fx2{F(-5), F(3)}) == a);   // clamps to the ends
    CHECK(closest_point_on_segment(a, b, fx2{F(50), F(3)}) == b);
    CHECK(closest_point_on_segment(a, a, fx2{F(5), F(5)}) == a);    // degenerate, no divide by zero

    const u128 d_sq = distance_sq_to_segment(a, b, fx2{F(5), F(3)});
    CHECK(fx::from_raw(static_cast<i64>(isqrt128(d_sq))) == F(3));

    // Scale invariance of the projection: a point's foot on a segment should
    // not depend on how long the segment is beyond it.
    Rng rng(11);
    for (int i = 0; i < 5000; ++i) {
        const fx px = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 32);
        const fx2 p{px, F(7)};
        const fx2 c1 = closest_point_on_segment(fx2{F(-1000), F(0)}, fx2{F(1000), F(0)}, p);
        CHECK(c1.y == F(0));
    }
}

// -------------------------------------------------------------- collision

// Two sectors side by side sharing a side, so floor heights can be varied to
// exercise the step rules.
struct TwoRoom {
    fx2 pts[6];
    Sector secs[2];
    Side sides[8];
    MapData map;

    TwoRoom(fx floor_a, fx floor_b, fx ceil = F(20)) {
        // Left room x[0,10], right room x[10,20], both y[0,10].
        pts[0] = {F(0), F(0)};
        pts[1] = {F(10), F(0)};
        pts[2] = {F(10), F(10)};
        pts[3] = {F(0), F(10)};
        pts[4] = {F(20), F(0)};
        pts[5] = {F(20), F(10)};

        secs[0] = {0, 4, 0, 4, floor_a, ceil, 0};
        secs[1] = {0, 0, 4, 4, floor_b, ceil, 0};

        // Left room's sides; the x=10 edge faces the right room.
        sides[0] = {0, 1, NO_SECTOR, 0};
        sides[1] = {1, 2, 1, 0};          // shared
        sides[2] = {2, 3, NO_SECTOR, 0};
        sides[3] = {3, 0, NO_SECTOR, 0};
        // Right room's sides; the x=10 edge faces the left room.
        sides[4] = {1, 4, NO_SECTOR, 0};
        sides[5] = {4, 5, NO_SECTOR, 0};
        sides[6] = {5, 2, NO_SECTOR, 0};
        sides[7] = {2, 1, 0, 0};          // shared

        map = MapData{pts, 6, secs, 2, sides, 8};
    }
};

// The right sector's polygon needs its own point range; build it explicitly.
static void build_two_room(fx2* pts, Sector* secs, Side* sides, MapData& map,
                           fx floor_a, fx floor_b, fx ceil) {
    pts[0] = {F(0), F(0)};   pts[1] = {F(10), F(0)};
    pts[2] = {F(10), F(10)}; pts[3] = {F(0), F(10)};
    pts[4] = {F(10), F(0)};  pts[5] = {F(20), F(0)};
    pts[6] = {F(20), F(10)}; pts[7] = {F(10), F(10)};

    secs[0] = {0, 4, 0, 4, floor_a, ceil, 0};
    secs[1] = {4, 4, 4, 4, floor_b, ceil, 0};

    sides[0] = {0, 1, NO_SECTOR, 0};
    sides[1] = {1, 2, 1, 0};
    sides[2] = {2, 3, NO_SECTOR, 0};
    sides[3] = {3, 0, NO_SECTOR, 0};

    sides[4] = {4, 5, NO_SECTOR, 0};
    sides[5] = {5, 6, NO_SECTOR, 0};
    sides[6] = {6, 7, NO_SECTOR, 0};
    sides[7] = {7, 4, 0, 0};

    map = MapData{pts, 8, secs, 2, sides, 8};
}

static void test_wall_slide() {
    std::printf("physics: wall sliding preserves tangential velocity\n");
    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    // Both rooms at the same height but the shared side made BLOCKING, so the
    // divider behaves as a solid wall.
    build_two_room(pts, secs, sides, map, F(0), F(0), F(20));
    sides[1].flags = side_flag::BLOCKING;
    sides[7].flags = side_flag::BLOCKING;

    MoveInput in;
    in.pos = fx3{F(5), F(5), F(0)};
    in.vel = fx3{F(10), F(4), F(0)};     // into the wall, and along it
    in.sector = 0;
    in.grounded = true;

    // Run until the wall is first touched and capture the state THERE. Once
    // the agent settles it rests at exactly contact distance, which is not
    // penetration, so hit_wall is correctly false again on later ticks --
    // asserting on the final tick would be testing the resting case by
    // accident.
    MoveResult r;
    MoveResult at_contact;
    bool ever_hit = false;
    for (int i = 0; i < 30; ++i) {
        if (i) { in.pos = r.pos; in.vel = r.vel; in.sector = r.sector; in.grounded = r.grounded; }
        r = move(map, in, sim::DT);
        if (r.hit_wall && !ever_hit) { at_contact = r; ever_hit = true; }
        CHECK(r.pos.x < F(10));   // must never tunnel, on any tick
    }

    CHECK(ever_hit);
    // The into-wall component is gone...
    CHECK(at_contact.vel.x < Ff(1, 100));
    // ...and the along-wall component survives untouched. Naive resolution
    // zeroes both and the agent stops dead against the wall.
    CHECK(at_contact.vel.y == F(4));
}

static void test_slide_has_no_drift() {
    // Regression: the push-out normal was originally derived from
    // `normalize(p - closest)`, which carries the accumulated rounding of the
    // POSITION. Against an axis-aligned wall that produced a normal a hair
    // off-axis, and the projection leaked ~4e-8 of velocity between the two
    // axes on every tick of contact -- invisible per tick, a steady drift over
    // a long slide. Taking the wall's own normal for interior contacts makes it
    // exact.
    std::printf("physics: sliding along a wall leaks nothing between axes\n");

    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    build_two_room(pts, secs, sides, map, F(0), F(0), F(20));
    sides[1].flags = side_flag::BLOCKING;
    sides[7].flags = side_flag::BLOCKING;

    MoveInput in;
    in.pos = fx3{F(9), F(1), F(0)};
    in.vel = fx3{F(10), F(4), F(0)};
    in.sector = 0;
    in.grounded = true;

    MoveResult r;
    fx settled_y_vel = fx::zero();
    fx settled_x = fx::zero();
    bool settled = false;

    for (int i = 0; i < 100; ++i) {
        if (i) { in.pos = r.pos; in.sector = r.sector; in.grounded = r.grounded; }
        // Re-apply the drive each tick, as a controller would.
        in.vel = fx3{F(10), i && settled ? settled_y_vel : F(4), fx::zero()};
        r = move(map, in, sim::DT);

        if (r.hit_wall && !settled) {
            settled = true;
            settled_y_vel = r.vel.y;
            settled_x = r.pos.x;
        } else if (settled) {
            // Pinned against the wall: x must not creep, and the tangential
            // velocity must be bit-identical every tick, not merely close.
            CHECK(r.pos.x == settled_x);
            CHECK(r.vel.y == settled_y_vel);
            CHECK(r.vel.x == fx::zero());
        }
        if (r.pos.y > F(9)) break;
    }
    CHECK(settled);
    CHECK(settled_y_vel == F(4));   // tangential component survives EXACTLY
}

static void test_step_up() {
    std::printf("physics: step up over small rises, blocked by large ones\n");

    // A rise of exactly the step height must be climbed.
    {
        fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
        build_two_room(pts, secs, sides, map, F(0), sim::STEP_HEIGHT, F(20));

        MoveInput in;
        in.pos = fx3{F(8), F(5), F(0)};
        in.vel = fx3{F(6), F(0), F(0)};
        in.sector = 0;
        in.grounded = true;

        MoveResult r;
        for (int i = 0; i < 40; ++i) {
            if (i) { in.pos = r.pos; in.vel = r.vel; in.sector = r.sector; in.grounded = r.grounded; }
            in.vel.x = F(6);   // keep driving forward
            r = move(map, in, sim::DT);
        }
        CHECK(r.pos.x > F(10));                       // crossed into the higher room
        CHECK(r.pos.z == sim::STEP_HEIGHT);           // and is standing on its floor
        CHECK(r.grounded);
    }

    // One ulp above the threshold must behave as a wall. This is the boundary
    // that decides whether a slope is walkable, so it is tested exactly rather
    // than approximately.
    {
        fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
        build_two_room(pts, secs, sides, map, F(0),
                       fx::from_raw(sim::STEP_HEIGHT.raw + 1), F(20));

        MoveInput in;
        in.pos = fx3{F(8), F(5), F(0)};
        in.vel = fx3{F(6), F(0), F(0)};
        in.sector = 0;
        in.grounded = true;

        MoveResult r;
        for (int i = 0; i < 40; ++i) {
            if (i) { in.pos = r.pos; in.vel = r.vel; in.sector = r.sector; in.grounded = r.grounded; }
            in.vel.x = F(6);
            r = move(map, in, sim::DT);
        }
        CHECK(r.pos.x < F(10));      // stopped at the riser
        CHECK(r.hit_wall);
        CHECK(r.pos.z == F(0));
    }
}

static void test_step_down() {
    std::printf("physics: step down snaps to ground, no airborne frames\n");

    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    // Right room is one step LOWER.
    build_two_room(pts, secs, sides, map, F(0), -sim::STEP_HEIGHT, F(20));

    MoveInput in;
    in.pos = fx3{F(8), F(5), F(0)};
    in.vel = fx3{F(6), F(0), F(0)};
    in.sector = 0;
    in.grounded = true;

    bool ever_airborne = false;
    MoveResult r;
    for (int i = 0; i < 40; ++i) {
        if (i) { in.pos = r.pos; in.vel = r.vel; in.sector = r.sector; in.grounded = r.grounded; }
        in.vel.x = F(6);
        in.vel.z = fx::zero();      // no gravity applied: only snapping can move z
        r = move(map, in, sim::DT);
        if (r.pos.x > F(10) && !r.grounded) ever_airborne = true;
    }

    CHECK(r.pos.x > F(10));
    CHECK(r.pos.z == -sim::STEP_HEIGHT);
    CHECK(r.grounded);
    // The whole point of the rule: descending a step must not produce a single
    // airborne frame, or movement feels floaty and `grounded`-gated abilities
    // flicker.
    CHECK(!ever_airborne);
}

static void test_step_symmetry() {
    std::printf("physics: the same constant governs up and down\n");
    // A rise that can be climbed must also be descendable, by construction --
    // there is only one threshold, so this can never drift out of sync.
    CHECK(sim::STEP_HEIGHT > fx::zero());

    for (int sign = -1; sign <= 1; sign += 2) {
        fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
        const fx dz = sign > 0 ? sim::STEP_HEIGHT : -sim::STEP_HEIGHT;
        build_two_room(pts, secs, sides, map, F(0), dz, F(20));

        MoveInput in;
        in.pos = fx3{F(8), F(5), F(0)};
        in.sector = 0;
        in.grounded = true;
        MoveResult r;
        for (int i = 0; i < 40; ++i) {
            if (i) { in.pos = r.pos; in.sector = r.sector; in.grounded = r.grounded; }
            in.vel = fx3{F(6), fx::zero(), fx::zero()};
            r = move(map, in, sim::DT);
        }
        CHECK(r.pos.x > F(10));
        CHECK(r.pos.z == dz);
    }
}

static void test_ccd() {
    std::printf("physics: CCD prevents tunnelling at high speed\n");

    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    build_two_room(pts, secs, sides, map, F(0), F(0), F(20));
    sides[1].flags = side_flag::BLOCKING;
    sides[7].flags = side_flag::BLOCKING;

    // Fast enough to cross the whole left room in a single tick.
    MoveInput in;
    in.pos = fx3{F(1), F(5), F(0)};
    in.vel = fx3{F(600), F(0), F(0)};
    in.sector = 0;
    in.grounded = true;

    const MoveResult r = move(map, in, sim::DT);
    CHECK(r.swept);
    CHECK(r.pos.x < F(10));      // did not pass through the wall
    CHECK(r.hit_wall);

    // Below the threshold the cheaper discrete path is used.
    MoveInput slow = in;
    slow.vel = fx3{F(1), F(0), F(0)};
    CHECK(!move(map, slow, sim::DT).swept);
}

static void test_ceiling_and_containment() {
    std::printf("physics: ceilings block, and the agent never leaves the map\n");

    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    build_two_room(pts, secs, sides, map, F(0), F(0), F(3));

    // ceil 3, body height 2, so the highest the feet can go is z == 1.
    // vel.z must be large enough to actually reach it inside one tick.
    MoveInput in;
    in.pos = fx3{F(5), F(5), F(0)};
    in.vel = fx3{fx::zero(), fx::zero(), F(120)};
    in.height = F(2);
    in.sector = 0;

    const MoveResult r = move(map, in, sim::DT);
    CHECK(r.hit_ceiling);
    CHECK(r.pos.z == F(1));
    CHECK(r.pos.z + in.height <= F(3));
    CHECK(r.vel.z == fx::zero());

    // Driving hard at an outer wall for a long time must never put the agent
    // outside the map. Falling out of the world is not a recoverable state, so
    // this is asserted every tick rather than only at the end.
    MoveInput out;
    out.pos = fx3{F(5), F(5), F(0)};
    out.sector = 0;
    out.grounded = true;
    MoveResult m;
    for (int i = 0; i < 200; ++i) {
        if (i) { out.pos = m.pos; out.sector = m.sector; out.grounded = m.grounded; }
        out.vel = fx3{F(-40), F(-40), fx::zero()};
        m = move(map, out, sim::DT);
        CHECK(m.sector != NO_SECTOR);
        CHECK(point_in_sector(map, static_cast<u32>(m.sector), fx2{m.pos.x, m.pos.y}));
    }
}

// ------------------------------------------------------------------ ledges

// One square platform, 0..10 on both axes, floating at z = 4. `flags` decides
// whether its edges are walls or open.
static MapData make_platform(u8 flags, Sector* sec, Side* sides, const fx2* pts) {
    for (int i = 0; i < 4; ++i) {
        sides[i] = Side{static_cast<u16>(i), static_cast<u16>((i + 1) % 4), NO_SECTOR, flags};
    }
    *sec = Sector{0, 4, 0, 4, F(4), F(40), 0, 0, 0};
    MapData m;
    m.points = pts; m.point_count = 4;
    m.sectors = sec; m.sector_count = 1;
    m.sides = sides; m.side_count = 4;
    return m;
}

static void test_walking_off_a_ledge() {
    std::printf("ledge: an open edge can be walked off; a wall cannot\n");

    static const fx2 pts[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    Sector sec_open{};
    Side sides_open[4]{};
    const MapData open_map = make_platform(side_flag::LEDGE, &sec_open, sides_open, pts);

    Sector sec_wall{};
    Side sides_wall[4]{};
    const MapData wall_map = make_platform(0, &sec_wall, sides_wall, pts);

    // The same agent, the same walk east, on the two maps.
    const fx dt = Ff(1, 60);
    const auto walk = [&](const MapData& m, int ticks) {
        MoveInput in;
        in.pos = fx3{F(5), F(5), F(4)};
        in.vel = fx3{F(8), fx::zero(), fx::zero()};
        in.sector = 0;
        in.grounded = true;
        MoveResult r;
        for (int t = 0; t < ticks; ++t) {
            r = move(m, in, dt);
            in.pos = r.pos;
            in.vel = r.vel;
            in.sector = r.sector;
            in.grounded = r.grounded;
            // Gravity, so leaving the platform actually falls.
            in.vel.z -= F(16) * dt;
        }
        return r;
    };

    const MoveResult walled = walk(wall_map, 120);
    CHECK(walled.sector == 0);
    CHECK(walled.grounded);
    // Stopped at the wall, radius short of it.
    CHECK(walled.pos.x <= F(10));
    CHECK(walled.pos.x >= F(9));
    CHECK(walled.pos.z == F(4));

    const MoveResult fell = walk(open_map, 120);
    CHECK(fell.sector == NO_SECTOR);
    CHECK(!fell.grounded);
    CHECK(fell.pos.x > F(10));      // past where the wall would have been
    CHECK(fell.pos.z < F(4));       // and below the platform

    // Horizontal speed survives the fall. An entity that stopped dead the
    // instant it left the map would make a missed jump look like hitting
    // something invisible.
    CHECK(fell.vel.x == F(8));

    // Falling is not a floor: it keeps going, tick after tick.
    MoveInput deep;
    deep.pos = fell.pos;
    deep.vel = fell.vel;
    deep.sector = fell.sector;
    const MoveResult a = move(open_map, deep, dt);
    deep.pos = a.pos;
    deep.vel = a.vel;
    deep.vel.z -= F(16) * dt;
    deep.sector = a.sector;
    const MoveResult b = move(open_map, deep, dt);
    CHECK(b.pos.z < a.pos.z);
    CHECK(!b.grounded);
    CHECK(b.sector == NO_SECTOR);
}

static void test_falling_past_a_platform() {
    std::printf("ledge: a falling entity passes under a platform, never onto it\n");

    // Two platforms in a row, both open-edged, both floating at z = 4:
    //   A: x 0..10   B: x 20..30      (a 10-unit gap, y 0..10 for both)
    static const fx2 pts[] = {
        {F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)},
        {F(20), F(0)}, {F(30), F(0)}, {F(30), F(10)}, {F(20), F(10)}
    };
    Sector secs[2] = {
        Sector{0, 4, 0, 4, F(4), F(40), 0, 0, 0},
        Sector{4, 4, 4, 4, F(4), F(40), 0, 0, 0}
    };
    Side sides[8]{};
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < 4; ++i) {
            sides[s * 4 + i] = Side{static_cast<u16>(s * 4 + i),
                                    static_cast<u16>(s * 4 + (i + 1) % 4),
                                    NO_SECTOR, side_flag::LEDGE};
        }
    }
    MapData m;
    m.points = pts; m.point_count = 8;
    m.sectors = secs; m.sector_count = 2;
    m.sides = sides; m.side_count = 8;

    // A missed jump: leaves A heading east, not fast enough to clear the gap.
    const fx dt = Ff(1, 60);
    MoveInput in;
    in.pos = fx3{F(9), F(5), F(4)};
    in.vel = fx3{F(12), fx::zero(), F(4)};
    in.sector = 0;
    in.grounded = false;

    bool entered_void = false;
    bool ever_grounded_after_void = false;
    // Sampled WHILE it is beneath platform B, not at the end of the run: at
    // 12 units per second it crosses the whole 10-unit slab in under a second
    // and is long past it by tick 240. The first version of this asserted the
    // final position was under B and failed for exactly that reason.
    int ticks_under_b = 0;
    fx lowest = in.pos.z;
    for (int t = 0; t < 240; ++t) {
        const MoveResult r = move(m, in, dt);
        if (r.sector == NO_SECTOR) entered_void = true;
        if (entered_void && r.grounded) ever_grounded_after_void = true;
        if (r.pos.x > F(20) && r.pos.x < F(30) && r.pos.z < F(4)) ++ticks_under_b;
        lowest = r.pos.z < lowest ? r.pos.z : lowest;
        in.pos = r.pos;
        in.vel = r.vel;
        in.sector = r.sector;
        in.grounded = r.grounded;
        in.vel.z -= F(16) * dt;
    }

    CHECK(entered_void);
    // It really did pass through the space beneath B rather than missing it.
    CHECK(ticks_under_b > 10);
    // Never snapped up onto it, and still falling at the end.
    CHECK(!ever_grounded_after_void);
    CHECK(in.sector == NO_SECTOR);
    CHECK(lowest < F(-40));

    // The control: from ABOVE, the same platform still catches it. Otherwise
    // "never lands" would be satisfied by a floor that had stopped working.
    MoveInput from_above;
    from_above.pos = fx3{F(25), F(5), F(30)};
    from_above.vel = fx3{fx::zero(), fx::zero(), fx::zero()};
    from_above.sector = NO_SECTOR;      // arriving out of the void
    bool landed = false;
    for (int t = 0; t < 240 && !landed; ++t) {
        const MoveResult r = move(m, from_above, dt);
        landed = r.grounded;
        from_above.pos = r.pos;
        from_above.vel = r.vel;
        from_above.sector = r.sector;
        from_above.grounded = r.grounded;
        from_above.vel.z -= F(16) * dt;
    }
    CHECK(landed);
    CHECK(from_above.pos.z == F(4));
    CHECK(from_above.sector == 1);
}

static void test_ledge_does_not_leak_into_closed_maps() {
    std::printf("ledge: a sector with no open edge still cannot be left\n");

    static const fx2 pts[] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)}};
    Sector sec{};
    Side sides[4]{};
    const MapData m = make_platform(0, &sec, sides, pts);

    CHECK(!sector_has_ledge(m, 0));
    CHECK(!sector_has_ledge(m, -1));      // defined answer, not a crash
    CHECK(!sector_has_ledge(m, 99));

    // Fired at the wall far faster than one tick of travel, so the swept path
    // is what has to stop it rather than resting contact.
    MoveInput in;
    in.pos = fx3{F(5), F(5), F(4)};
    in.vel = fx3{F(600), fx::zero(), fx::zero()};
    in.sector = 0;
    const MoveResult r = move(m, in, Ff(1, 60));
    CHECK(r.sector == 0);
    CHECK(r.pos.x < F(10));

    // And with the edges opened, the SAME shot leaves.
    Sector osec{};
    Side osides[4]{};
    const MapData om = make_platform(side_flag::LEDGE, &osec, osides, pts);
    CHECK(sector_has_ledge(om, 0));
    const MoveResult ro = move(om, in, Ff(1, 60));
    CHECK(ro.sector == NO_SECTOR);
    CHECK(ro.pos.x > F(10));
}

static void test_determinism_of_move() {
    std::printf("physics: move() is a pure function of its inputs\n");
    fx2 pts[8]; Sector secs[2]; Side sides[8]; MapData map;
    build_two_room(pts, secs, sides, map, F(0), Ff(1, 4), F(20));

    Rng rng(2024);
    for (int trial = 0; trial < 2000; ++trial) {
        MoveInput in;
        in.pos = fx3{rng.next_fx(F(1), F(19)), rng.next_fx(F(1), F(9)), rng.next_fx(F(0), F(5))};
        in.vel = fx3{rng.next_fx(F(-20), F(20)), rng.next_fx(F(-20), F(20)), rng.next_fx(F(-20), F(20))};
        in.sector = 0;
        in.grounded = (rng.next_u32() & 1) != 0;

        const MoveResult a = move(map, in, sim::DT);
        const MoveResult b = move(map, in, sim::DT);
        CHECK(a.pos == b.pos);
        CHECK(a.vel == b.vel);
        CHECK(a.sector == b.sector);
        CHECK(a.grounded == b.grounded);
    }
}

int main() {
    std::printf("== core/physics test suite ==\n");
    test_point_in_sector();
    test_segment_math();
    test_wall_slide();
    test_slide_has_no_drift();
    test_step_up();
    test_step_down();
    test_step_symmetry();
    test_ccd();
    test_ceiling_and_containment();
    test_walking_off_a_ledge();
    test_falling_past_a_platform();
    test_ledge_does_not_leak_into_closed_maps();
    test_determinism_of_move();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
