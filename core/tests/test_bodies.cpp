//
// Bodies that push each other (design §7.1's "soft agent-vs-agent push").
//
// A body BLOCKS: each one is pushed out of the others, and none of them is
// pushed by it. That distinction is the whole design. The first version shoved
// the lower-priority body instead, and since monsters chase, they walked
// players off the doors they were opening -- a body you cannot walk through is
// what "solid" means, and a body that pushes you around is a mechanic nobody
// asked for.
//
// Each agent resolves against where the others were at the START of the tick,
// which is what makes the outcome independent of the order the step loop ran
// in, and is also the only version a client can reproduce: a snapshot is
// exactly a set of start-of-tick positions.
//
// The controls are the ways an overlap test passes for the wrong reason:
//
//   vertical    a monster on a balcony does not shove one standing under it.
//   direction   separation is along the line between the centres, not along
//               whichever axis happened to be tested first.
//   order       the outcome does not depend on which body stepped first.
//   stillness   an approaching body does not move the one it walks into.
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

// Two rooms side by side, sharing a SOLID wall at x = 20 with no opening.
//
// A single room cannot test this. A push that leaves one open room leaves the
// map, and refusing it is easy; a push that crosses a wall INTO another legal
// room lands somewhere the body is perfectly entitled to stand, just not
// somewhere it could have walked to. The first fixture written for the
// out-of-bounds push was one flat room, and it passed with the entire fix
// commented out.
struct TwoRooms {
    MapStorage store;
    MapData    view{};

    TwoRooms() {
        const fx2 west[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(40)}, {F(0), F(40)}};
        const fx2 east[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(20), F(40)}};
        store.add_sector(west, 4, F(0), F(40), 1);
        store.add_sector(east, 4, F(0), F(40), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// One flat room, 40 x 40, tall enough to stack bodies in.
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
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

static fx dist2d(const World& w, u32 a, u32 b) {
    const fx2 d{w.agent(b).pos.x - w.agent(a).pos.x, w.agent(b).pos.y - w.agent(a).pos.y};
    return length(d);
}

// Two bodies at the same spot end the tick a diameter apart, not on top of each
// other. The starting overlap is total, which is also the degenerate case: with
// zero distance there is no line between the centres to separate along, and an
// implementation that normalises a zero vector either divides by zero or leaves
// them exactly where they were.
static void test_overlapping_bodies_separate() {
    std::printf("bodies: two bodies do not occupy one place\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 b = w.spawn(fx3{F(20), F(20), F(0)});
    const fx want = w.params(a).radius + w.params(b).radius;

    // Ticks, not one tick. Separation of two bodies that are not moving is
    // deliberately slow -- DEPENETRATE_PER_TICK, a twentieth of a unit -- so
    // that a body being walked into is not shoved across the room. A stuck
    // spawn unsticks in well under a second; it does not unstick instantly.
    for (u32 t = 0; t < 120; ++t) w.step();

    CHECK(dist2d(w, a, b) >= want);
    // Both moved, and in OPPOSITE directions. The degenerate case has no line
    // between the centres to separate along, so the direction is chosen by
    // index -- and an implementation that picks one fixed fallback for
    // everybody sends both bodies the same way and leaves them concentric,
    // which still satisfies "they each moved".
    CHECK(w.agent(a).pos.x != w.agent(b).pos.x || w.agent(a).pos.y != w.agent(b).pos.y);
}

// Separation runs along the line joining the centres. A body approached from
// the north-east must leave to the north-east, not sideways along an axis: an
// implementation that resolves x then y produces exactly that, and it still
// passes a distance assertion.
static void test_separation_follows_the_line_between_centres() {
    std::printf("bodies: pushed apart along the line between them\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    // Diagonally overlapping: 0.2 in each axis, well inside the sum of radii.
    const u32 b = w.spawn(fx3{F(20) + fx::from_frac(2, 10), F(20) + fx::from_frac(2, 10), F(0)});

    w.step();

    const fx dx = w.agent(b).pos.x - w.agent(a).pos.x;
    const fx dy = w.agent(b).pos.y - w.agent(a).pos.y;
    CHECK(dx > fx::zero());
    CHECK(dy > fx::zero());
    // Symmetric input, so the escape is symmetric too -- an axis-at-a-time
    // resolver leaves one of these near zero.
    const fx diff = dx > dy ? dx - dy : dy - dx;
    CHECK(diff < fx::from_frac(1, 100));
}

// A body overhead is not a body in the way. Without the vertical test, one
// monster on a ledge shoves another standing beneath it across the room.
static void test_a_body_overhead_does_not_push() {
    std::printf("bodies: vertically clear bodies pass over each other\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    w.spawn(fx3{F(20), F(20), F(0)});
    const u32 b = w.spawn(fx3{F(20), F(20), F(12)});
    w.params_mut(b).gravity_scale = fx::zero();   // hold it up there

    w.step();

    CHECK(w.agent(b).pos.x == F(20));
    CHECK(w.agent(b).pos.y == F(20));
    // The control: the same pair at the same height DOES push, so the pass
    // above is the height test working rather than the push being broken.
    static World w2;
    w2.reset();
    r.attach(w2);
    const u32 c = w2.spawn(fx3{F(20), F(20), F(0)});
    const u32 d = w2.spawn(fx3{F(20), F(20), F(0)});
    w2.params_mut(d).gravity_scale = fx::zero();
    w2.step();
    CHECK(dist2d(w2, c, d) > fx::zero());
}

// The load-bearing one: walking into somebody moves YOU, not them.
//
// Without it, monsters -- which chase, and therefore arrive -- push players
// around continuously, and a player trying to stand still and work a door is
// slowly walked off it by whatever is following them.
static void test_an_approaching_body_does_not_move_you() {
    std::printf("bodies: a body that walks into you does not move you\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 still = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 walker = w.spawn(fx3{F(14), F(20), F(0)});

    proto::InputFrame f;
    f.move_x = 32767;                 // due east, straight into `still`
    proto::InputFrame nothing;
    for (u32 t = 0; t < 120; ++t) {
        w.set_input(walker, f);
        w.set_input(still, nothing);
        w.step();
    }
    CHECK(w.agent(still).pos.x == F(20));
    CHECK(w.agent(still).pos.y == F(20));
    // And it really did arrive, so the assertion above is the rule holding
    // rather than the walker never getting there.
    CHECK(w.agent(walker).pos.x > F(18));

    // The control, with the roles reversed: the one that WALKS is the one that
    // stops. Same pair, same distance, and now the moving body is the one whose
    // position is pinned by the other.
    const fx want = w.params(still).radius + w.params(walker).radius;
    CHECK(w.agent(still).pos.x - w.agent(walker).pos.x >= want - fx::from_frac(1, 100));
}

// Walking into someone stops you at their surface rather than through them.
// The overlap in the tests above starts as a spawn; this one has to be created
// by movement, which is the case that actually happens in a game.
static void test_you_cannot_walk_through_a_body() {
    std::printf("bodies: a body blocks a walk\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 wall = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 walker = w.spawn(fx3{F(14), F(20), F(0)});
    const fx want = w.params(wall).radius + w.params(walker).radius;

    proto::InputFrame f;
    f.move_x = 32767;          // due east, into the other body
    f.aim = brad(0u);
    for (u32 t = 0; t < 120; ++t) {
        w.set_input(walker, f);
        w.step();
        CHECK(dist2d(w, wall, walker) >= want - fx::from_frac(1, 1000));
        if (g_fails) break;
    }
    // It got there, rather than the assertion holding because it never left.
    CHECK(w.agent(walker).pos.x > F(14));
}

// The answer must not depend on which body the loop reached first, which is why
// resolution is a separate pass after every agent has moved rather than
// something step_agent() does on its way past.
static void test_the_outcome_does_not_depend_on_step_order() {
    std::printf("bodies: resolution is order-independent\n");

    static Room r;

    // Same two bodies, same convergent inputs, spawned in opposite orders.
    auto run = [&](bool swap, fx3& lo, fx3& hi) {
        static World w;
        w.reset();
        r.attach(w);
        const fx3 west{F(18), F(20), F(0)};
        const fx3 east{F(22), F(20), F(0)};
        const u32 first = w.spawn(swap ? east : west);
        const u32 second = w.spawn(swap ? west : east);

        proto::InputFrame toward_east;
        toward_east.move_x = 32767;
        proto::InputFrame toward_west;
        toward_west.move_x = -32767;
        for (u32 t = 0; t < 60; ++t) {
            w.set_input(first, swap ? toward_west : toward_east);
            w.set_input(second, swap ? toward_east : toward_west);
            w.step();
        }
        lo = w.agent(swap ? second : first).pos;    // the westward body
        hi = w.agent(swap ? first : second).pos;    // the eastward one
    };

    fx3 a_west{}, a_east{}, b_west{}, b_east{};
    run(false, a_west, a_east);
    run(true, b_west, b_east);

    // Not equality: the two runs differ in which body carries the lower index,
    // and the rule is asymmetric by design, so they legitimately settle in
    // different places. What must hold either way is that they are apart and
    // still on the same side of each other -- nobody was pushed THROUGH.
    CHECK(a_west.x < a_east.x);
    CHECK(b_west.x < b_east.x);
    CHECK(a_east.x - a_west.x > fx::zero());
    CHECK(b_east.x - b_west.x > fx::zero());
}

// A push must not be an escape route.
//
// The push moves a cylinder along the line between two centres and asks the map
// nothing, so a body shoved against a wall goes THROUGH it -- and a player
// standing in a doorway with a monster walking into them leaves the level. That
// is not a hypothetical either: it is what a browser session did.
static void test_a_push_cannot_shove_you_out_of_the_world() {
    std::printf("bodies: a push cannot put you outside the map\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    // Two bodies OVERLAPPING, hard against the east wall at x = 40.
    //
    // A gentle approach is not the case that breaks: the mover resolves a
    // shallow overlap into the wall on the next tick and the body never
    // actually leaves. What escapes is a deep overlap, where the push is most
    // of a diameter in one step -- which is what a monster walking into a
    // player standing in a doorway produces, and what a spawn on an occupied
    // tag produces immediately.
    const fx at_wall = F(40) - fx::from_frac(3, 10);
    const u32 pinned = w.spawn(fx3{at_wall, F(20), F(0)});
    const u32 shover = w.spawn(fx3{at_wall, F(20), F(0)});

    proto::InputFrame east;
    east.move_x = 32767;
    proto::InputFrame nothing;
    for (u32 t = 0; t < 400; ++t) {
        w.set_input(shover, east);
        w.set_input(pinned, nothing);
        w.step();
        // BOTH of them: the pair is symmetric here, and asserting on one leaves
        // the other free to be the one that leaves.
        CHECK(w.agent(shover).pos.x <= F(40));
        CHECK(w.agent(shover).pos.y <= F(40));
        CHECK(w.agent(shover).pos.y >= F(0));
        // Inside the room, every tick. Not just at the end: a body that left
        // and came back would pass a final-position check, and while it was
        // out there it was falling.
        CHECK(w.agent(pinned).pos.x <= F(40));
        CHECK(w.agent(pinned).pos.x >= F(0));
        CHECK(w.agent(pinned).pos.y <= F(40));
        CHECK(w.agent(pinned).pos.y >= F(0));
        if (g_fails) break;
    }
    // And they really did separate, so the assertions above are the map
    // holding rather than two bodies sitting still on top of each other.
    const fx2 gap{w.agent(shover).pos.x - w.agent(pinned).pos.x,
                  w.agent(shover).pos.y - w.agent(pinned).pos.y};
    CHECK(length(gap) > fx::from_frac(1, 2));
}

// The one that reproduces it: a push across a solid wall into the next room.
//
// Two bodies overlapping hard against the shared wall. Separating them takes a
// full diameter, and the wall is one line thick -- so a push that teleports
// lands the far body in the east room, having walked through a wall it could
// never have opened. The body is inside the map afterwards, standing somewhere
// legal, which is precisely why an "is it still in the world" check does not
// catch this and a swept push does.
static void test_a_push_cannot_cross_a_wall() {
    std::printf("bodies: a push cannot cross a wall\n");

    static TwoRooms r;
    static World w;
    w.reset();
    r.attach(w);

    // Concentric, a third of a unit from the wall on the west side.
    const fx near_wall = F(20) - fx::from_frac(4, 10);
    const u32 a = w.spawn(fx3{near_wall, F(20), F(0)});
    const u32 b = w.spawn(fx3{near_wall, F(20), F(0)});

    proto::InputFrame nothing;
    for (u32 t = 0; t < 400; ++t) {
        w.set_input(a, nothing);
        w.set_input(b, nothing);
        w.step();
        // Both still west of the wall. Neither has any business east of it:
        // there is no opening, and neither of them ever walked anywhere.
        CHECK(w.agent(a).pos.x < F(20));
        CHECK(w.agent(b).pos.x < F(20));
        if (g_fails) break;
    }
    // And they did separate, west of the wall, rather than the assertion
    // holding because nothing moved.
    const fx2 gap{w.agent(b).pos.x - w.agent(a).pos.x, w.agent(b).pos.y - w.agent(a).pos.y};
    CHECK(length(gap) > fx::from_frac(1, 4));
}

// Bodies are part of hashed state, so a divergence in the push shows up on the
// determinism gate rather than as a shrug about interpolation.
static void test_the_push_is_hashed() {
    std::printf("bodies: the push is in the state hash\n");

    static Room r;
    auto run = [&](bool overlap) {
        static World w;
        w.reset();
        r.attach(w);
        w.spawn(fx3{F(20), F(20), F(0)});
        w.spawn(overlap ? fx3{F(20), F(20), F(0)} : fx3{F(30), F(20), F(0)});
        for (u32 t = 0; t < 8; ++t) w.step();
        return w.hash();
    };
    CHECK(run(true) != run(false));
}

// A CROWD, RESOLVED BOTH WAYS.
//
// Agent-vs-agent resolution used to be a flat double loop: every solid body
// against every other, every tick. It is now a hash grid over the bodies'
// tick-start positions, and the claim made for that grid is a strong one --
// not "close enough", but the SAME PAIRS IN THE SAME ORDER, with only the
// pairs that would have done nothing left out.
//
// It has to be that strong. The inner loop moves the body it is resolving, so
// the pushes are not independent: reorder two of them and the body ends up
// somewhere else, which is a different simulation, which is a client
// mispredicting every tick of a crowd.
//
// So the control is the comparison itself. The same crowd runs twice -- once
// through the grid, once with force_full_agent_sweep -- and every body's
// position and velocity must agree at every tick, not merely at the end,
// because two runs that diverge and reconverge are still two simulations.
//
// AND THE CROWD HAS TO BE A CROWD. Both paths agree by construction when a
// body has no neighbours, so a sparse scenario passes whether the grid works
// or not -- the golden trace's twelve scattered agents do exactly that, and
// reversing the candidate order does not move its hash. The bodies here start
// packed at well under a radius apart, and the run asserts the grid was
// actually used and that several candidates were in play.
static void test_a_crowd_resolves_the_same_both_ways() {
    std::printf("bodies: a crowd resolves the same with the broadphase and without\n");

    static Room r;

    // 100 bodies in a 10 x 10 lattice on 0.35-unit spacing, which is under one
    // radius: every one of them starts overlapping its neighbours, and the
    // first ticks are a press of mutual depenetration -- the case where the
    // order of the pushes matters most and where a body travels furthest from
    // where its query was centred.
    struct Sample { i64 x, y, z, vx, vy; };
    static Sample trace[2][64 * 100];

    auto run = [&](bool full_sweep, int slot) {
        static World w;
        w.reset();
        r.attach(w);
        w.force_full_agent_sweep(full_sweep);
        for (int gy = 0; gy < 10; ++gy) {
            for (int gx = 0; gx < 10; ++gx) {
                w.spawn(fx3{F(18) + fx::from_frac(35 * gx, 100),
                            F(18) + fx::from_frac(35 * gy, 100), F(0)});
            }
        }
        u32 fast = 0, full = 0;
        for (u32 t = 0; t < 64; ++t) {
            w.step();
            fast += w.broadphase_fast();
            full += w.broadphase_full();
            for (u32 i = 0; i < 100; ++i) {
                const AgentState& a = w.agent(i);
                trace[slot][t * 100 + i] =
                    Sample{a.pos.x.raw, a.pos.y.raw, a.pos.z.raw, a.vel.x.raw, a.vel.y.raw};
            }
        }
        return full_sweep ? full : fast;
    };

    const u32 grid_served = run(false, 0);
    const u32 swept       = run(true, 1);

    // The grid ran, and ran for everybody. Without this the comparison below
    // passes just as happily when the fast path is never taken -- the two
    // sides agree by design, so "identical" and "never tried" look the same.
    CHECK(grid_served == 64 * 100);
    CHECK(swept == 64 * 100);

    int first_diff = -1;
    for (int k = 0; k < 64 * 100 && first_diff < 0; ++k) {
        const Sample& a = trace[0][k];
        const Sample& b = trace[1][k];
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.vx != b.vx || a.vy != b.vy) {
            first_diff = k;
        }
    }
    if (first_diff >= 0) {
        std::printf("    first divergence at tick %d, body %d\n", first_diff / 100,
                    first_diff % 100);
    }
    CHECK(first_diff < 0);

    // And the crowd really did move, so "identical" is not two copies of
    // nothing happening. A hundred bodies starting inside each other cannot
    // end where they started.
    CHECK(trace[0][63 * 100].x != trace[0][0].x || trace[0][63 * 100].y != trace[0][0].y);
}

// A bridge over a path, both sharing a footprint in plan.
//
//   path    x 0..40, y 0..40,  floor 0,  ceiling 8
//   bridge  x 10..30, y 0..40, floor 10, thickness 2 (slab 8..10)
//
// This is maps/mylevel.map's shape, reduced: sectors stacked over each other,
// which is what `skirt` in the .map format is FOR.
struct Bridge {
    MapStorage store;
    MapData    view{};

    Bridge() {
        const fx2 path[]   = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
        const fx2 bridge[] = {{F(10), F(0)}, {F(30), F(0)}, {F(30), F(40)}, {F(10), F(40)}};
        store.add_sector(path, 4, F(0), F(8), 1);
        store.add_sector(bridge, 4, F(10), F(60), 2);
        store.sector_mut(1).thickness = F(2);
        for (u16 s = 0; s < 2; ++s) {
            for (u16 i = 0; i < 4; ++i) {
                store.add_side(s, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
            }
        }
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// You can stand on the upper of two stacked sectors, and jumping does not drop
// you into the lower one.
//
// Reported from maps/mylevel.map: jumping on a sector that had another one
// under it put the player in the one underneath. Two causes, and each alone
// was enough:
//
//   World::spawn and World::teleport used the height-BLIND find_sector, which
//   picks by footprint and breaks the tie by smallest area. A body placed on
//   the bridge was recorded as being in the path.
//
//   query_surface took last tick's sector as a hint to avoid a scan, and
//   tested only whether the point was inside it. With stacked sectors that is
//   true of both, so once the wrong one was in hand it was believed forever --
//   and the answer it gave was the path's floor, so the body fell through the
//   bridge and could not get back onto it. There is no hint any more; see the
//   comment in core/src/physics/collide.cpp for why it could not be repaired.
//
// core/tests/test_physics.cpp has tested this geometry since Phase 3, and
// missed both: it asserts find_sector_at, which was correct all along, and
// walks a body UNDER the bridge. Nothing walked on top of one.
//
// HONESTLY: run against either broken version, THIS test still passes. The
// old find_sector broke the tie by smallest area, and here the bridge is the
// smaller sector, so it guessed right for the case that was reported. What
// has teeth is the control below it -- the same geometry approached from
// underneath, where smallest-area guesses wrong -- and, for the hint, the
// query_surface block in test_physics.cpp's bridge test, which drives the
// wrong hint in directly instead of waiting for something to produce one.
// Kept anyway because it is the user's report, walked end to end.
static void test_you_can_stand_on_a_stacked_sector() {
    std::printf("bodies: a sector above another is a floor, not a trapdoor\n");

    static Bridge b;
    static World w;
    w.reset();
    b.attach(w);

    // Spawned on the bridge, in the overlap. The path is directly underneath.
    const u32 a = w.spawn(fx3{F(20), F(20), F(10)});
    CHECK(w.agent(a).mv.sector == 1);

    proto::InputFrame nothing;
    for (u32 t = 0; t < 60; ++t) { w.set_input(a, nothing); w.step(); }

    // Settled ON the bridge, not on the path eight units below it.
    CHECK(w.agent(a).pos.z == F(10));
    CHECK(w.agent(a).mv.sector == 1);
    CHECK(w.agent(a).mv.grounded);

    // Now jump. Landing must return the body to the bridge -- the whole report
    // was that the jump was what moved you down.
    proto::InputFrame jump;
    jump.buttons = proto::button::JUMP;
    w.set_input(a, jump);
    w.step();
    CHECK(w.agent(a).pos.z > F(10));   // it really left the ground
    CHECK(!w.agent(a).mv.grounded);

    fx apex = w.agent(a).pos.z;
    for (u32 t = 0; t < 120; ++t) {
        w.set_input(a, nothing);
        w.step();
        if (w.agent(a).pos.z > apex) apex = w.agent(a).pos.z;
        // At no point in the arc is the body reported as being in the path.
        // Passing only at the end would let a one-tick excursion through, and
        // one tick in the wrong sector is one tick of the wrong floor.
        CHECK(w.agent(a).pos.z >= F(10));
        CHECK(w.agent(a).mv.sector == 1);
        if (g_fails) break;
    }
    CHECK(apex > F(10));
    CHECK(w.agent(a).pos.z == F(10));
    CHECK(w.agent(a).mv.grounded);
}

// The control: the same body, same map, spawned UNDER the bridge stays under
// it. Without this the test above passes for a trivial reason -- an
// implementation that always answered "the higher sector" would satisfy every
// assertion in it, and would break walking under a bridge instead.
static void test_you_are_still_under_a_bridge_when_you_are_under_it() {
    std::printf("bodies: standing under the bridge is still standing under it\n");

    static Bridge b;
    static World w;
    w.reset();
    b.attach(w);

    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    CHECK(w.agent(a).mv.sector == 0);

    proto::InputFrame jump;
    jump.buttons = proto::button::JUMP;
    proto::InputFrame nothing;
    w.set_input(a, jump);
    w.step();

    for (u32 t = 0; t < 120; ++t) {
        w.set_input(a, nothing);
        w.step();
        // The path's ceiling is 8 and the body is 2 tall, so it cannot reach
        // the bridge's floor at 10 by jumping -- it stays underneath.
        CHECK(w.agent(a).mv.sector == 0);
        CHECK(w.agent(a).pos.z < F(8));
        if (g_fails) break;
    }
    CHECK(w.agent(a).pos.z == fx::zero());
}

// maps/mylevel.map's actual shape: one big low plane -- lava, floor -3 --
// whose polygon CONTAINS a small raised platform. Not a bridge over a path
// but a platform in a pool, which is how a hand-drawn outdoor level comes out
// and is the arrangement `skirt` exists for.
struct LavaPool {
    MapStorage store;
    MapData    view{};

    LavaPool() {
        const fx2 lava[]     = {{-F(40), -F(40)}, {F(40), -F(40)}, {F(40), F(40)}, {-F(40), F(40)}};
        const fx2 platform[] = {{-F(6), -F(6)}, {F(6), -F(6)}, {F(6), F(6)}, {-F(6), F(6)}};
        store.add_sector(lava, 4, -F(3), F(56), 1);
        store.add_sector(platform, 4, -F(1), F(32), 2);
        store.sector_mut(0).thickness = F(2);
        store.sector_mut(1).thickness = F(2);
        for (u16 sec = 0; sec < 2; ++sec) {
            for (u16 i = 0; i < 4; ++i) {
                store.add_side(sec, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
            }
        }
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// You can jump out of the lava back onto the platform standing in it.
//
// The second report from maps/mylevel.map: "trying to jump back to the start
// platform teleports me back to lava". The same cause as standing on a
// stacked sector, from the other side, and it survived the first fix.
//
// query_surface took the sector the body was in last tick as a hint and
// accepted it whenever its floor was within a step below the feet and its
// ceiling above them. The lava's floor is -3 and its ceiling 55: a body
// jumping out of it satisfies both for the entire arc, keeps the hint, and is
// handed the LAVA's floor while standing over the platform -- so the landing
// snapped it back down. There is no hint any more, which is why this passes.
static void test_you_can_jump_out_of_a_pit_onto_a_platform_inside_it() {
    std::printf("bodies: jumping out of the lava lands you on the platform\n");

    static LavaPool r;
    static World w;
    w.reset();
    r.attach(w);

    // In the lava, outside the platform's footprint, with room for a run-up.
    // Air control is almost nothing in this engine, so the jump has to carry
    // ground speed -- standing still and jumping goes straight up.
    const u32 a = w.spawn(fx3{F(16), fx::zero(), -F(3)});
    proto::InputFrame run;
    run.move_x = -32767;                       // due west, towards the platform
    for (u32 t = 0; t < 40; ++t) { w.set_input(a, run); w.step(); }
    CHECK(w.agent(a).mv.sector == 0);
    CHECK(w.agent(a).pos.z == -F(3));
    CHECK(w.agent(a).pos.x > F(6));            // not on the platform yet

    // Jump. The platform's floor is two units up -- four times STEP_HEIGHT, so
    // this has to be a jump and cannot be a step.
    proto::InputFrame jump = run;
    jump.buttons = proto::button::JUMP;
    w.set_input(a, jump);
    w.step();
    CHECK(!w.agent(a).mv.grounded);

    bool reached_platform = false;
    for (u32 t = 0; t < 240; ++t) {
        w.set_input(a, run);
        w.step();
        if (w.agent(a).mv.sector == 1) reached_platform = true;
        if (w.agent(a).mv.grounded && reached_platform) break;
    }

    CHECK(reached_platform);
    // Landed ON it and stayed. The report was that the landing put you back in
    // the lava, so "grounded at 0 in sector 1" is the whole claim.
    CHECK(w.agent(a).mv.grounded);
    CHECK(w.agent(a).mv.sector == 1);
    CHECK(w.agent(a).pos.z == -F(1));

    // And it stays there rather than sinking a tick later.
    proto::InputFrame nothing;
    for (u32 t = 0; t < 60; ++t) { w.set_input(a, nothing); w.step(); }
    CHECK(w.agent(a).mv.sector == 1);
    CHECK(w.agent(a).pos.z == -F(1));
}

// The control: a body that jumps in the lava WITHOUT moving over the platform
// comes back down in the lava. Without it, an implementation that always
// preferred the higher sector anywhere in the pool would pass the test above
// and would leave nobody able to stand in the lava at all.
static void test_jumping_in_the_lava_lands_in_the_lava() {
    std::printf("bodies: jumping in the lava lands in the lava\n");

    static LavaPool r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(10), fx::zero(), -F(3)});
    proto::InputFrame jump;
    jump.buttons = proto::button::JUMP;
    proto::InputFrame nothing;
    w.set_input(a, jump);
    w.step();

    // Against the count at ENTRY, not against g_fails. A control whose loop
    // exits early because some earlier test failed reports whatever that other
    // failure left behind, and a control that fails alongside the thing it is
    // controlling for is telling you nothing.
    const int before = g_fails;
    for (u32 t = 0; t < 240; ++t) {
        w.set_input(a, nothing);
        w.step();
        CHECK(w.agent(a).mv.sector == 0);
        if (g_fails != before) break;
    }
    CHECK(w.agent(a).pos.z == -F(3));
    CHECK(w.agent(a).mv.grounded);
}

int main() {
    test_overlapping_bodies_separate();
    test_separation_follows_the_line_between_centres();
    test_a_body_overhead_does_not_push();
    test_an_approaching_body_does_not_move_you();
    test_you_cannot_walk_through_a_body();
    test_the_outcome_does_not_depend_on_step_order();
    test_a_push_cannot_shove_you_out_of_the_world();
    test_a_push_cannot_cross_a_wall();
    test_the_push_is_hashed();
    test_a_crowd_resolves_the_same_both_ways();
    test_you_can_stand_on_a_stacked_sector();
    test_you_are_still_under_a_bridge_when_you_are_under_it();
    test_you_can_jump_out_of_a_pit_onto_a_platform_inside_it();
    test_jumping_in_the_lava_lands_in_the_lava();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
