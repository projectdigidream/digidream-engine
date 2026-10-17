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
#include "dnp/sim/world.hpp"

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

// Two rooms that are NOT connected: each declares the shared line as its own
// wall, so it is solid from both sides and both rooms are standable.
//
// Distinct from Pair above, where the two sectors are neighbours and the
// question is what is behind the boundary. Here there is nothing behind it --
// which is the ordinary case for a wall in a real level, and the one a swing
// has to respect.
struct Split {
    MapStorage store;
    MapData    view{};

    Split() {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(40)}, {F(0), F(40)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(20), F(40)}};
        store.add_sector(west, 4, F(0), F(10), 1);
        store.add_sector(east, 4, F(0), F(10), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        view = store.view();
    }
};

// A swing does not reach through a wall either.
//
// The distance and angle tests describe a SHAPE and say nothing about the
// level, so without this a player on the far side of a door kills what is
// behind it. Here rather than in test_props because it is the same question
// line of sight asks, answered by the same raycast: what stops a bullet stops
// a sword.
static void test_a_swing_does_not_reach_through_a_wall() {
    std::printf("sight: a swing is stopped by a wall like everything else\n");

    static Split r;
    static sim::World w;
    w.reset();
    w.set_map(&r.view);
    w.set_map_storage(&r.store);

    // Either side of the shared wall at x = 20, well inside a generous reach.
    const u32 swinger = w.spawn(fx3{F(19), F(20), F(0)});
    const u32 victim  = w.spawn(fx3{F(21), F(20), F(0)});
    w.params_mut(swinger).attack_reach = F(6);

    proto::InputFrame f;
    f.buttons = proto::button::ATTACK_PRIMARY;
    f.aim = brad(0u);                 // east, straight at them
    w.set_input(swinger, f);
    w.step();

    bool hit = false;
    for (u32 i = 0; i < w.attacks().count(); ++i) {
        if (w.attacks().at(i).target == victim) hit = true;
    }
    CHECK(!hit);

    // The control: the same pair on the SAME side of the wall, the same
    // distance apart, is hit. Without it "nothing was hit" is satisfied by a
    // swing that never worked.
    static sim::World w2;
    w2.reset();
    w2.set_map(&r.view);
    w2.set_map_storage(&r.store);
    const u32 s2 = w2.spawn(fx3{F(10), F(20), F(0)});
    const u32 v2 = w2.spawn(fx3{F(12), F(20), F(0)});
    w2.params_mut(s2).attack_reach = F(6);
    w2.set_input(s2, f);
    w2.step();
    bool hit2 = false;
    for (u32 i = 0; i < w2.attacks().count(); ++i) {
        if (w2.attacks().at(i).target == v2) hit2 = true;
    }
    CHECK(hit2);
}

// --- a blow at arm's length: walls and doors stop it, ledges do not -----------
//
// A monster's `melee` is a distance test, and a distance says nothing about the
// level: a claw reached through a wall, and through a shut door, to a player
// standing on the far side (physics::reach_clear). Three rooms in a row, the
// middle one a door half a unit thick, open or shut.
struct Doorway {
    MapStorage store;
    MapData    view{};
    Doorway(fx door_ceil, fx east_floor) {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(40)}, {F(0), F(40)}};
        const fx2 door[] = {{F(20), F(0)}, {fx::from_frac(41, 2), F(0)}, {fx::from_frac(41, 2), F(40)}, {F(20), F(40)}};
        const fx2 east[] = {{fx::from_frac(41, 2), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {fx::from_frac(41, 2), F(40)}};
        store.add_sector(west, 4, F(0), F(10), 1);
        store.add_sector(door, 4, F(0), door_ceil, 2);
        store.add_sector(east, 4, east_floor, F(10), 3);
        store.add_side(0, 0, 1, NO_SECTOR, 0); store.add_side(0, 1, 2, 1, 0);
        store.add_side(0, 2, 3, NO_SECTOR, 0); store.add_side(0, 3, 0, NO_SECTOR, 0);
        store.add_side(1, 0, 1, NO_SECTOR, 0); store.add_side(1, 1, 2, 2, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0); store.add_side(1, 3, 0, 0, 0);
        store.add_side(2, 0, 1, NO_SECTOR, 0); store.add_side(2, 1, 2, NO_SECTOR, 0);
        store.add_side(2, 2, 3, NO_SECTOR, 0); store.add_side(2, 3, 0, 1, 0);
        view = store.view();
    }
};

static void test_a_blow_stops_at_walls_and_shut_doors() {
    std::printf("reach: a blow is stopped by a wall and a shut door, not by a ledge\n");
    // From just west of the door to just east of it, at a striker's middle.
    const fx2 from{fx::from_frac(39, 2), F(20)};
    const fx2 to{fx::from_frac(43, 2), F(20)};
    const fx mid = fx::from_frac(1, 2);
    static Doorway open(F(8), F(0));
    static Doorway shut(F(0), F(0));
    static Doorway ledge(F(8), F(1));      // the far room a step up
    CHECK(reach_clear(open.view, 0, from, to, mid));     // CONTROL: through an open door
    CHECK(!reach_clear(shut.view, 0, from, to, mid));    // a shut door
    CHECK(reach_clear(ledge.view, 0, from, to, mid));    // a step up is not a wall
    static Split wall;
    CHECK(!reach_clear(wall.view, 0, fx2{F(19), F(20)}, fx2{F(21), F(20)}, mid));   // a plain wall
    // A wall BEHIND the target is in nobody's way.
    CHECK(reach_clear(wall.view, 0, fx2{F(17), F(20)}, fx2{F(19), F(20)}, mid));
    // And the flat cast the swing uses goes straight through the shut door,
    // which is why this is its own question.
    const RayHit flat = raycast(shut.view, 0, from, fx2{fx::one(), fx::zero()}, F(2), 0);
    CHECK(!flat.blocked);
}

// --- a missile under a platform -------------------------------------------------
//
// A deck over a path: two footprints, the deck's inside the path's, its floor
// above the path's ceiling. Where a sloped missile's flight ends, the sector it
// is in decides which floor or ceiling stops it -- and asked by footprint, the
// deck's (the smaller) answered for a missile flying under it, whose floor was
// over its head: it burst where it was, on nothing.
struct Deck {
    MapStorage store;
    MapData    view{};
    Deck() {
        const fx2 path[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
        const fx2 deck[] = {{F(15), F(0)}, {F(25), F(0)}, {F(25), F(40)}, {F(15), F(40)}};
        store.add_sector(path, 4, F(0), F(3), 1);
        store.add_sector(deck, 4, F(4), F(12), 2);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        view = store.view();
    }
};

static void test_a_missile_under_a_platform_flies_on() {
    std::printf("sweep: a missile under a platform is not stopped by the platform's floor\n");
    static Deck d;
    static sim::World w;
    w.reset();
    w.set_map(&d.view);
    w.set_map_storage(&d.store);
    const fx r = fx::from_frac(1, 4);
    // Under the deck, coming down gently: from x 10 to 20, height 1 to 0.9.
    const sim::World::SweepHit under = w.sweep_entities(fx3{F(10), F(20), F(1)}, fx2{fx::one(), fx::zero()},
                                                        F(10), r, -1, -1, -1, -fx::from_frac(1, 100));
    std::printf("  under the deck: %s at %lld\n", under.hit ? "STOPPED" : "flies on",
                static_cast<long long>(under.distance.raw >> 32));
    CHECK(!under.hit);
    // CONTROL: one above the deck, coming down onto it, is stopped by its floor,
    // five units on (from height 5 at a fall of 0.2 a unit, to the deck at 4).
    const sim::World::SweepHit over = w.sweep_entities(fx3{F(16), F(20), F(5)}, fx2{fx::one(), fx::zero()},
                                                       F(6), r, -1, -1, -1, -fx::from_frac(1, 5));
    CHECK(over.hit && over.plane == -1);
    CHECK(fx_abs(over.distance - F(5)) < fx::from_frac(1, 100));
}

// --- looking DOWN a step ------------------------------------------------------
//
// The bug this was written for: "when the player is standing on a step lower
// than the enemy, the enemy seems to be stuck on that step".
//
// A flat cast travels at the LOOKER's eye height for its whole length, so a
// monster standing above its target asks whether there is a clear horizontal
// line at its own eye -- which is not the line to the target. Here the room
// next door is a low one: its ceiling is below the monster's eye but well above
// the player standing in it. The horizontal ray runs into that ceiling; the
// real line of sight, sloping down, is clear the whole way.
//
// The monster therefore decided it could not see somebody standing in plain
// view a little below, dropped its target, immediately reacquired it, and stood
// on the step doing that.
static void test_a_sloped_ray_reaches_a_target_below_a_low_ceiling() {
    std::printf("sight: looking down into a lower room\n");
    // East room: floor at 0, ceiling at 5. The looker's eye is at 6 -- it is
    // standing on something -- and the target's eye is at 2, inside that room.
    //
    // The numbers are chosen so the two casts genuinely disagree, which took a
    // wrong fixture to notice: the boundary is 10 of the ray's 25 units away,
    // so a ray sloping 6 -> 2 is at 4.4 where it crosses. A ceiling of 4 blocks
    // BOTH casts -- correctly, the line really does clip it -- and the test
    // then proved nothing. At 5 the sloped line passes under and the flat one
    // at 6 does not.
    static Pair p(F(0), F(5));

    const fx2 from{F(10), F(20)};
    const fx2 dir{fx::one(), fx::zero()};

    // THE CONTROL, and it is the old behaviour: flat at the looker's eye, the
    // low ceiling is a wall. This must fail, or the sloped cast below is not
    // being asked anything.
    CHECK(raycast(p.view, 0, from, dir, F(25), 0, F(6)).blocked);

    // The same geometry, the same two points, with the ray told where it is
    // going: clear.
    const RayHit sloped = raycast(p.view, 0, from, dir, F(25), 0, F(6), F(2));
    CHECK(!sloped.blocked);

    // AND IT STILL BLOCKS WHAT IT SHOULD. A target below the far room's floor
    // is behind a wall however the ray slopes, so a sloped cast is not simply a
    // more permissive one.
    static Pair high(F(8), F(12));   // east floor at 8, above both eyes
    CHECK(raycast(high.view, 0, from, dir, F(25), 0, F(6), F(2)).blocked);
}

// A sloped cast with no far height is the flat one, exactly. Every existing
// caller passes six arguments and must be untouched by this.
static void test_omitting_the_far_height_is_the_flat_cast() {
    std::printf("sight: a ray with no far height is flat\n");
    static Pair p(F(0), F(4));
    const fx2 from{F(10), F(20)};
    const fx2 dir{fx::one(), fx::zero()};
    const RayHit a = raycast(p.view, 0, from, dir, F(25), 0, F(6));
    const RayHit b = raycast(p.view, 0, from, dir, F(25), 0, F(6), RAY_NO_HEIGHT);
    CHECK(a.blocked == b.blocked);
    CHECK(a.blocked);
}

// A BODY IS A COLUMN, and that is what "monsters lose the player when he is
// higher or lower" turned out to be.
//
// Sight was one line drawn between two eyes. Stand a few units back from a
// ledge and the line from a monster below to your WAIST runs into the ledge
// while the line to your HEAD clears it -- which is exactly the moment a player
// walks up onto a balcony and every monster in the room forgets about him.
//
// The numbers are worked out rather than found: the east floor is at 6, the
// monster's eye at 1 in the west room at x = 10, and the boundary is at x = 20.
// A body standing at x = 23 with its feet on that floor puts the boundary
// 10/13 of the way along, so the middle line arrives there at 5.62 -- under the
// floor -- and the head line at 6.38, over it. One is visible and one is not,
// and the body is the same body.
static void test_a_head_over_a_ledge_is_visible() {
    std::printf("sight: a head over a ledge is visible when the waist is not\n");
    static Pair p(F(6), F(10));
    const fx3 eye{F(10), F(20), F(1)};
    const fx2 them{F(23), F(20)};

    // CONTROL, and it is the bug: the single line this replaced, between the
    // two middles, says no. Without this the claim below could be met by a
    // sight test that had simply stopped checking heights.
    const fx mid = F(7);
    const fx2 d{them.x - eye.x, them.y - eye.y};
    CHECK(raycast(p.view, 0, fx2{eye.x, eye.y}, normalize(d), length(d), 0,
                  eye.z, mid).blocked);

    // The column: feet on the floor at 6, head at 8.
    CHECK(can_see(p.view, 0, eye, them, F(6), F(8)));
}

// The same claim upside down, because a fix that only looks up is half a fix --
// and Doom's own levels put as many monsters on balconies as in pits.
//
// A crawlspace east: floor 0, ceiling 2. The monster's eye is at 5 in the west
// room and the body stands at x = 25 with its head at 2. The boundary is 10/15
// of the way, so the middle line is at 2.33 -- over the ceiling -- and the line
// to the FEET is at 1.67, under it.
static void test_feet_under_a_ceiling_are_visible() {
    std::printf("sight: feet below a ceiling are visible when the waist is not\n");
    static Pair p(F(0), F(2));
    const fx3 eye{F(10), F(20), F(5)};
    const fx2 them{F(25), F(20)};

    const fx2 d{them.x - eye.x, them.y - eye.y};
    // CONTROL: the middle line is stopped by the ceiling.
    CHECK(raycast(p.view, 0, fx2{eye.x, eye.y}, normalize(d), length(d), 0,
                  eye.z, F(1)).blocked);

    CHECK(can_see(p.view, 0, eye, them, F(0), F(2)));
}

// AND IT IS NOT "YES" WEARING A DISGUISE. Two bodies that genuinely cannot see
// each other still cannot, or every assertion above is satisfied by a function
// that returns true.
static void test_can_see_still_says_no() {
    std::printf("sight: and a body that is really hidden is still hidden\n");

    // A shut door -- opaque at every height, so no aim along the column helps
    // and the retry must not be attempted at all.
    static Pair shut(F(0), F(0));
    CHECK(!can_see(shut.view, 0, fx3{F(10), F(20), F(1)}, fx2{F(30), F(20)},
                   F(0), F(2)));

    // A floor high enough that the whole column is under it: the same geometry
    // as the head-over-a-ledge case, with the body standing far enough back
    // that even its head is below the line. A retry that answered yes anyway
    // would pass the two tests above for the wrong reason.
    static Pair p(F(6), F(10));
    CHECK(!can_see(p.view, 0, fx3{F(10), F(20), F(1)}, fx2{F(35), F(20)},
                   F(6), F(8)));

    // And a plain wall with nothing behind it, northward out of the west room.
    CHECK(!can_see(p.view, 0, fx3{F(10), F(20), F(1)}, fx2{F(10), F(60)},
                   F(0), F(2)));
}

int main() {
    test_a_missile_under_a_platform_flies_on();
    test_a_blow_stops_at_walls_and_shut_doors();
    test_an_open_neighbour_does_not_block();
    test_a_raised_floor_blocks_sight_below_it();
    test_a_low_ceiling_blocks_sight_above_it();
    test_a_shut_door_blocks_at_every_height();
    test_the_flat_cast_is_unchanged();
    test_a_plain_wall_still_blocks();
    test_the_walk_does_not_bounce_between_two_sectors();
    test_a_swing_does_not_reach_through_a_wall();
    test_a_sloped_ray_reaches_a_target_below_a_low_ceiling();
    test_omitting_the_far_height_is_the_flat_cast();
    test_a_head_over_a_ledge_is_visible();
    test_feet_under_a_ceiling_are_visible();
    test_can_see_still_says_no();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
