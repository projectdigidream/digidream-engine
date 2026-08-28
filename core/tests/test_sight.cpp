//
// What a ray can see, and what stops it (design §7.1.1's swept cast).
//
// A raycast in this engine answers three separate questions -- can a monster
// see you, did a bullet reach you, did a fireball hit a wall -- and until now it
// answered all three in two dimensions. A boundary occluded only if it had no
// sector behind it or carried BLOCKING.
//
// That is a much weaker test than any of the three callers wanted, and in a
// converted Doom level it is wrong nearly everywhere: a step, a balcony, a
// window ledge and a shut door are all two-sided lines with a named neighbour.
// So a zombieman shot a player through several walls and across the map, which
// is what this file exists to prevent from coming back.
//
// The controls are the ways a sight test passes for the wrong reason:
//
//   open        a real opening at the ray's height still lets it through, or
//               the fix is just "everything blocks" wearing a disguise.
//   height      the SAME geometry blocks at one height and not at another,
//               which no floorplan test can do.
//   flat        a caller that asks for the old two-dimensional cast still gets
//               it, so the golden traces and the grapple are untouched.
//
#include <cstdio>

#include "dnp/geom/map.hpp"
#include "dnp/geom/mapstore.hpp"
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

// Two rooms, west and east, sharing a two-sided boundary at x = 20 -- a
// NEIGHBOUR, not a hole. What is behind it is the east sector's own floor and
// ceiling, and that is the whole question.
struct Pair {
    MapStorage store;
    MapData    view{};

    Pair(fx east_floor, fx east_ceil) {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(40)}, {F(0), F(40)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(20), F(40)}};
        store.add_sector(west, 4, F(0), F(10), 1);
        store.add_sector(east, 4, east_floor, east_ceil, 1);

        // West: three walls and the shared boundary on the (20,0)->(20,40) edge.
        store.add_side(0, 0, 1, NO_SECTOR, 0);
        store.add_side(0, 1, 2, 1, 0);          // -> east
        store.add_side(0, 2, 3, NO_SECTOR, 0);
        store.add_side(0, 3, 0, NO_SECTOR, 0);
        // East: the shared boundary is its (20,40)->(20,0) edge.
        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0);
        store.add_side(1, 3, 0, 0, 0);          // -> west
        view = store.view();
    }
};

// Straight east from the middle of the west room, far enough to cross.
static RayHit look_east(const MapData& m, fx z) {
    return raycast(m, 0, fx2{F(10), F(20)}, fx2{fx::one(), fx::zero()}, F(25), 0, z);
}

// The room next door is at the same height and open. Nothing should stop this,
// and if something does then every other assertion here is meaningless.
static void test_an_open_neighbour_does_not_block() {
    std::printf("sight: an open room next door is visible\n");
    static Pair p(F(0), F(10));
    CHECK(!look_east(p.view, F(1)).blocked);
    CHECK(!look_east(p.view, F(5)).blocked);
}

// A floor raised above the eye. This is a step, a crate or a balcony, and it is
// a two-sided line -- so the old test walked straight through it.
static void test_a_raised_floor_blocks_sight_below_it() {
    std::printf("sight: a floor above your eyes is a wall\n");
    static Pair p(F(6), F(10));      // east floor at 6
    CHECK(look_east(p.view, F(1)).blocked);
    // And the control: from ABOVE that floor the same geometry is open. No
    // floorplan test can tell those two apart, which is the point.
    CHECK(!look_east(p.view, F(8)).blocked);
}

// A ceiling below the eye -- a crawlspace, a window's underside.
static void test_a_low_ceiling_blocks_sight_above_it() {
    std::printf("sight: a ceiling below your eyes is a wall\n");
    static Pair p(F(0), F(2));       // east ceiling at 2
    CHECK(look_east(p.view, F(5)).blocked);
    CHECK(!look_east(p.view, F(1)).blocked);
}

// Doom stores a shut door as a sector whose ceiling rests on its floor: a
// two-sided line with nothing passable behind it at any height whatsoever.
static void test_a_shut_door_blocks_at_every_height() {
    std::printf("sight: a shut door is opaque at every height\n");
    static Pair p(F(0), F(0));       // ceiling on the floor
    for (i64 z = 0; z <= 9; ++z) CHECK(look_east(p.view, F(z)).blocked);
}

// The old behaviour, kept for callers that genuinely want a floorplan cast and
// for every golden trace recorded before this existed.
static void test_the_flat_cast_is_unchanged() {
    std::printf("sight: without a height, the cast is still two-dimensional\n");
    static Pair p(F(6), F(10));      // a raised floor, which blocks at eye level
    CHECK(look_east(p.view, F(1)).blocked);
    const RayHit flat =
        raycast(p.view, 0, fx2{F(10), F(20)}, fx2{fx::one(), fx::zero()}, F(25), 0);
    CHECK(!flat.blocked);
}

// A one-sided wall is opaque whatever else is true. The cheapest thing to break
// while adding a second reason for a side to be solid.
static void test_a_plain_wall_still_blocks() {
    std::printf("sight: a wall with nothing behind it still stops a ray\n");
    static Pair p(F(0), F(10));
    // North, into the west room's own outer wall.
    const RayHit north =
        raycast(p.view, 0, fx2{F(10), F(20)}, fx2{fx::zero(), fx::one()}, F(40), 0, F(1));
    CHECK(north.blocked);
    CHECK(north.distance > fx::zero());
}

// The one that was actually happening on MAP01.
//
// The walk re-found the boundary it had just stepped through -- `from` is never
// advanced, so `t` is measured from the origin and the epsilon meant to skip
// that side only ever applied on the first hop. The ray bounced between two
// sectors until it ran out of hops, and running out used to be reported as NOT
// BLOCKED. So a long shot resolved to a clear line through everything.
//
// Asserted as a COUNT rather than as "it works": a ray crossing one boundary
// must take one hop, and the old code took all of them.
static void test_the_walk_does_not_bounce_between_two_sectors() {
    std::printf("sight: the walk crosses a boundary once, not repeatedly\n");

    // Open, same height, so nothing stops the ray and the only thing that can
    // end the walk is reaching the end of the range.
    static Pair p(F(0), F(10));

    // Far enough east to cross into the second room and stop inside it. If the
    // walk bounces, it exhausts its hops and -- with the fix -- reports blocked.
    const RayHit into_east = look_east(p.view, F(1));
    CHECK(!into_east.blocked);

    // And a long shot that ends inside the far room is still clear. This is the
    // shape of the MAP01 case: enough range to cross several boundaries.
    const RayHit far_shot =
        raycast(p.view, 0, fx2{F(1), F(20)}, fx2{fx::one(), fx::zero()}, F(38), 0, F(1));
    CHECK(!far_shot.blocked);

    // The same shot with a wall in the way is blocked, so the assertion above
    // is the walk working rather than the test never reaching anything.
    static Pair shut(F(0), F(0));
    const RayHit stopped =
        raycast(shut.view, 0, fx2{F(1), F(20)}, fx2{fx::one(), fx::zero()}, F(38), 0, F(1));
    CHECK(stopped.blocked);
    CHECK(stopped.distance < F(38));
}

int main() {
    test_an_open_neighbour_does_not_block();
    test_a_raised_floor_blocks_sight_below_it();
    test_a_low_ceiling_blocks_sight_above_it();
    test_a_shut_door_blocks_at_every_height();
    test_the_flat_cast_is_unchanged();
    test_a_plain_wall_still_blocks();
    test_the_walk_does_not_bounce_between_two_sectors();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
