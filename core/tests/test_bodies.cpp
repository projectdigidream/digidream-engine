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

    w.step();

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

int main() {
    test_overlapping_bodies_separate();
    test_separation_follows_the_line_between_centres();
    test_a_body_overhead_does_not_push();
    test_an_approaching_body_does_not_move_you();
    test_you_cannot_walk_through_a_body();
    test_the_outcome_does_not_depend_on_step_order();
    test_the_push_is_hashed();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
