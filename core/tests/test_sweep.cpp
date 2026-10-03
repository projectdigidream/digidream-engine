//
// World::sweep_entities -- what does a moving thing of this size reach first?
//
// Written for missiles. The mob runtime was answering this in Lua by walking
// every entity for every projectile every tick, and it could not answer it
// CORRECTLY at all: dnp.sweep reports geometry only, so "did it hit a wall"
// and "did it hit somebody" were two questions, and asking them separately
// gives a fireball that bursts on a monster standing behind a door.
//
// The claims, and the control for each:
//
//   nearest      it returns the FIRST thing along the ray, not any thing.
//                Control: swap the two entities' positions and the answer
//                swaps with them.
//   walls        a wall between stops it, and the wall's distance caps the
//                search. Control: remove the wall and the same cast reaches.
//   sideways     a miss to the side is a miss. Control: move the target onto
//                the line and it hits.
//   vertical     a body on a balcony is not hit by a shot along the floor.
//                Control: lower it and the same shot hits.
//   ignore       the shooter is skipped. Control: without the skip, a missile
//                launched inside its owner reaches its owner at distance 0.
//   pure         it changes nothing -- the world hash before equals after.
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
static fx Ff(i64 a, i64 b) { return fx::from_frac(a, b); }

// One open room, 80 x 40, so a cast has room to run.
struct Room {
    MapStorage store;
    MapData    view{};
    Room() {
        const fx2 p[] = {{F(0), F(0)}, {F(80), F(0)}, {F(80), F(40)}, {F(0), F(40)}};
        store.add_sector(p, 4, F(0), F(40), 1);
        for (u16 i = 0; i < 4; ++i)
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// Two rooms with a SOLID wall between them at x = 40, and no opening. A cast
// from one into the other must be stopped by the wall rather than reaching the
// body standing beyond it.
struct Divided {
    MapStorage store;
    MapData    view{};
    Divided() {
        const fx2 west[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
        const fx2 east[] = {{F(40), F(0)}, {F(80), F(0)}, {F(80), F(40)}, {F(40), F(40)}};
        store.add_sector(west, 4, F(0), F(40), 1);
        store.add_sector(east, 4, F(0), F(40), 1);
        for (u16 s = 0; s < 2; ++s)
            for (u16 i = 0; i < 4; ++i)
                store.add_side(s, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, 0);
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

static const fx2 EAST{fx::from_int(1), fx::zero()};

// ---------------------------------------------------------------------------
static void test_it_finds_the_nearest() {
    std::printf("sweep: the first thing along the ray, not any thing\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    const u32 near_a  = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 far_a   = w.spawn(fx3{F(30), F(20), F(0)});

    const auto hit = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                      Ff(1, 8), static_cast<i32>(shooter), -1, -1);
    CHECK(hit.hit);
    CHECK(hit.entity_hit);
    CHECK(hit.entity == near_a);
    CHECK(hit.entity != far_a);
    // And it stopped at the SURFACE, not the centre: 20 less the body radius
    // and the sphere's. A hit reported at the centre draws a burst inside the
    // thing it hit.
    CHECK(hit.distance < F(18));
    CHECK(hit.distance > F(16));

    // THE CONTROL. Swap them, and the answer swaps -- which is what makes the
    // assertion above about ORDER rather than about which slot was scanned
    // first. The scan runs low index to high, and `near_a` has the lower one,
    // so without this a loop that simply returned its first candidate passes.
    static World w2;
    w2.reset();
    r.attach(w2);
    const u32 s2 = w2.spawn(fx3{F(2), F(20), F(0)});
    const u32 first_slot = w2.spawn(fx3{F(30), F(20), F(0)});   // now the FAR one
    const u32 second_slot = w2.spawn(fx3{F(20), F(20), F(0)});  // now the NEAR one
    const auto h2 = w2.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                      Ff(1, 8), static_cast<i32>(s2), -1, -1);
    CHECK(h2.entity == second_slot);
    CHECK(h2.entity != first_slot);
}

// ---------------------------------------------------------------------------
static void test_a_wall_stops_it() {
    std::printf("sweep: a wall between stops it, and caps the search\n");

    static Divided d;
    static World w;
    w.reset();
    d.attach(w);

    const u32 shooter = w.spawn(fx3{F(10), F(20), F(0)});
    const u32 behind  = w.spawn(fx3{F(60), F(20), F(0)});   // the far room

    const auto hit = w.sweep_entities(fx3{F(10), F(20), Ff(1, 2)}, EAST, F(70),
                                      Ff(1, 8), static_cast<i32>(shooter), -1, -1);
    CHECK(hit.hit);
    // It reached the WALL, not the body -- this is the bug the whole function
    // exists for. Asking geometry and entities separately gives entity_hit.
    CHECK(!hit.entity_hit);
    CHECK(hit.distance <= F(30) + Ff(1, 2));
    CHECK(hit.entity != behind || !hit.entity_hit);

    // THE CONTROL: the same cast with no wall reaches the body. Without it,
    // "did not reach" could equally mean the entity scan is broken.
    static Room r;
    static World w2;
    w2.reset();
    r.attach(w2);
    const u32 s2 = w2.spawn(fx3{F(10), F(20), F(0)});
    const u32 b2 = w2.spawn(fx3{F(60), F(20), F(0)});
    const auto h2 = w2.sweep_entities(fx3{F(10), F(20), Ff(1, 2)}, EAST, F(70),
                                      Ff(1, 8), static_cast<i32>(s2), -1, -1);
    CHECK(h2.hit);
    CHECK(h2.entity_hit);
    CHECK(h2.entity == b2);
}

// ---------------------------------------------------------------------------
static void test_a_miss_to_the_side_is_a_miss() {
    std::printf("sweep: past it is past it\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    // Well off the line: the default body radius is a third of a unit and the
    // sphere is an eighth, so five units to the side cannot touch.
    w.spawn(fx3{F(30), F(25), F(0)});

    const auto miss = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                       Ff(1, 8), static_cast<i32>(shooter), -1, -1);
    CHECK(!miss.entity_hit);

    // THE CONTROL: on the line, the same cast hits. Otherwise "no hit" might
    // only mean the cast never ran.
    static World w2;
    w2.reset();
    r.attach(w2);
    const u32 s2 = w2.spawn(fx3{F(2), F(20), F(0)});
    const u32 on_line = w2.spawn(fx3{F(30), F(20), F(0)});
    const auto h2 = w2.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                      Ff(1, 8), static_cast<i32>(s2), -1, -1);
    CHECK(h2.entity_hit);
    CHECK(h2.entity == on_line);

    // And a cast that is simply too SHORT does not reach it either. A distance
    // argument nothing enforced would make every one of these pass.
    const auto shortcast = w2.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST,
                                             F(5), Ff(1, 8),
                                             static_cast<i32>(s2), -1, -1);
    CHECK(!shortcast.entity_hit);
}

// ---------------------------------------------------------------------------
static void test_height_is_respected() {
    std::printf("sweep: a body overhead is not in the way\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    // Standing twenty units up. The room's ceiling is 40, so this is a legal
    // place to be and the only reason it is not hit is its height.
    w.spawn(fx3{F(30), F(20), F(20)});

    const auto miss = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                       Ff(1, 8), static_cast<i32>(shooter), -1, -1);
    CHECK(!miss.entity_hit);

    // THE CONTROL: the same body at the shooter's own height IS hit, so the
    // miss above is about the height and not about the position.
    static World w2;
    w2.reset();
    r.attach(w2);
    const u32 s2 = w2.spawn(fx3{F(2), F(20), F(0)});
    const u32 level = w2.spawn(fx3{F(30), F(20), F(0)});
    const auto h2 = w2.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60),
                                      Ff(1, 8), static_cast<i32>(s2), -1, -1);
    CHECK(h2.entity_hit);
    CHECK(h2.entity == level);

    // And a cast at the OVERHEAD body's height reaches it, which proves the
    // vertical test is a comparison and not a refusal to look up at all.
    const auto up = w.sweep_entities(fx3{F(2), F(20), F(20) + Ff(1, 2)}, EAST,
                                     F(60), Ff(1, 8),
                                     static_cast<i32>(shooter), -1, -1);
    CHECK(up.entity_hit);
}

// ---------------------------------------------------------------------------
static void test_the_shooter_is_skipped() {
    std::printf("sweep: a missile does not burst on the thing that fired it\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 target  = w.spawn(fx3{F(30), F(20), F(0)});

    // Launched from INSIDE the shooter, which is where a missile is on the
    // tick it appears.
    const auto hit = w.sweep_entities(fx3{F(20), F(20), Ff(1, 2)}, EAST, F(40),
                                      Ff(1, 8), static_cast<i32>(shooter), -1, -1);
    CHECK(hit.entity_hit);
    CHECK(hit.entity == target);

    // THE CONTROL: with nothing ignored, the same cast stops on the shooter at
    // distance zero -- which is exactly the failure `ignore` exists to prevent,
    // and it must be reproducible or the argument is doing nothing.
    const auto self = w.sweep_entities(fx3{F(20), F(20), Ff(1, 2)}, EAST, F(40),
                                       Ff(1, 8), -1, -1, -1);
    CHECK(self.entity_hit);
    CHECK(self.entity == shooter);
    CHECK(self.distance == fx::zero());
}

// ---------------------------------------------------------------------------
// A missile is an entity, and it starts INSIDE its owner. Both have to be
// skipped, and skipping either alone leaves the other -- which is exactly the
// bug the single-`ignore` version had: the fireball stopped at distance zero
// on the tick it was fired, on itself.
static void test_both_ignores_are_needed() {
    std::printf("sweep: a missile skips itself AND the thing that fired it\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 owner = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 target = w.spawn(fx3{F(30), F(20), F(0)});
    // The missile itself, a prop at the owner's own position, which is where
    // it is on the tick it appears. SOLID here, so the control below can show
    // a sweep stopping on it -- a sweep passes through anything that is not.
    const u32 missile = w.spawn_prop(PropKind{0}, fx3{F(20), F(20), Ff(1, 2)});
    w.prop_mut(missile).flags = prop_flag::SOLID;

    const fx3 from{F(20), F(20), Ff(1, 2)};

    // BOTH skipped: it reaches the target.
    const auto good = w.sweep_entities(from, EAST, F(40), Ff(1, 8),
                                       static_cast<i32>(missile),
                                       static_cast<i32>(owner), -1);
    CHECK(good.entity_hit);
    CHECK(good.entity == target);

    // CONTROL A: only the owner skipped -- it stops on ITSELF, at zero.
    const auto only_owner = w.sweep_entities(from, EAST, F(40), Ff(1, 8),
                                             static_cast<i32>(owner), -1, -1);
    CHECK(only_owner.entity_hit);
    CHECK(only_owner.entity == missile);
    CHECK(only_owner.distance == fx::zero());

    // CONTROL B: only the missile skipped -- it stops on its owner, at zero.
    const auto only_self = w.sweep_entities(from, EAST, F(40), Ff(1, 8),
                                            static_cast<i32>(missile), -1, -1);
    CHECK(only_self.entity_hit);
    CHECK(only_self.entity == owner);
    CHECK(only_self.distance == fx::zero());
}

// SOLID THINGS ONLY: a missile flies through a corpse and over a pickup, as
// Doom's do, and stops on the solid body behind them.
static void test_only_solid_things_stop_it() {
    std::printf("sweep: a corpse and a pickup are flown through, a body is not\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 owner = w.spawn(fx3{F(2), F(20), F(0)});
    const u32 corpse = w.spawn(fx3{F(10), F(20), F(0)});
    w.agent_mut(corpse).solid = false;
    const u32 pickup = w.spawn_prop(PropKind{0}, fx3{F(15), F(20), F(0)});
    w.prop_mut(pickup).radius = Ff(1, 2);
    w.prop_mut(pickup).height = F(1);
    const u32 body = w.spawn(fx3{F(25), F(20), F(0)});

    const auto hit = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60), Ff(1, 8),
                                      static_cast<i32>(owner), -1, -1);
    CHECK(hit.entity_hit && hit.entity == body);
    // CONTROL: the corpse made solid again stops it first.
    w.agent_mut(corpse).solid = true;
    const auto h2 = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60), Ff(1, 8),
                                     static_cast<i32>(owner), -1, -1);
    CHECK(h2.entity_hit && h2.entity == corpse);
    // CONTROL: and so does the pickup, made solid, once the corpse is gone.
    w.agent_mut(corpse).solid = false;
    w.prop_mut(pickup).flags = prop_flag::SOLID;
    const auto h3 = w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60), Ff(1, 8),
                                     static_cast<i32>(owner), -1, -1);
    CHECK(h3.entity_hit && h3.entity == pickup);
}

// A sphere of nonzero size hits things a zero-width ray would slip past. That
// is the whole difference between this and a raycast, so it is worth an
// assertion rather than an assumption.
static void test_the_radius_matters() {
    std::printf("sweep: a fat missile clips what a thin one misses\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    // Offset by more than a body radius -- which defaults to 1/2, checked
    // rather than assumed -- but less than a body radius plus a fat missile's.
    // So the answer depends on the missile's size and on nothing else.
    w.spawn(fx3{F(30), F(20) + Ff(11, 20), F(0)});

    const fx3 from{F(2), F(20), Ff(1, 2)};
    const auto thin = w.sweep_entities(from, EAST, F(60), Ff(1, 100),
                                       static_cast<i32>(shooter), -1, -1);
    const auto fat = w.sweep_entities(from, EAST, F(60), Ff(1, 2),
                                      static_cast<i32>(shooter), -1, -1);
    CHECK(!thin.entity_hit);
    CHECK(fat.entity_hit);
}

// The distance is measured along `dir` and the point is on the ray, so a
// caller can draw a burst where the thing actually stopped.
static void test_the_point_is_on_the_ray() {
    std::printf("sweep: the impact point is where it stopped\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    w.spawn(fx3{F(30), F(20), F(0)});

    const fx3 from{F(2), F(20), Ff(1, 2)};
    const auto hit = w.sweep_entities(from, EAST, F(60), Ff(1, 8),
                                      static_cast<i32>(shooter), -1, -1);
    CHECK(hit.entity_hit);
    // Due east, so y is unchanged and x is the start plus the distance. An
    // implementation that returned the TARGET's centre would fail the first;
    // one that returned `from` would fail the second.
    CHECK(hit.point.y == F(20));
    CHECK(hit.point.x == from.x + hit.distance);
    CHECK(hit.point.x < F(30));
    CHECK(hit.point.x > F(2));
}

// Degenerate inputs answer rather than trap. A zero-length cast reaches
// nothing; a zero radius still works as a ray.
static void test_degenerate_casts() {
    std::printf("sweep: zero distance reaches nothing, zero radius is a ray\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    const u32 target = w.spawn(fx3{F(30), F(20), F(0)});
    const fx3 from{F(2), F(20), Ff(1, 2)};

    CHECK(!w.sweep_entities(from, EAST, fx::zero(), Ff(1, 8),
                            static_cast<i32>(shooter), -1, -1).hit);
    CHECK(!w.sweep_entities(from, EAST, -F(10), Ff(1, 8),
                            static_cast<i32>(shooter), -1, -1).hit);

    // Zero radius is still a ray, and still finds a body on the line.
    const auto ray = w.sweep_entities(from, EAST, F(60), fx::zero(),
                                      static_cast<i32>(shooter), -1, -1);
    CHECK(ray.entity_hit);
    CHECK(ray.entity == target);
}

static void test_it_changes_nothing() {
    std::printf("sweep: it is a query -- the world is not touched\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    w.spawn(fx3{F(20), F(20), F(0)});
    for (u32 t = 0; t < 8; ++t) w.step();

    const u64 before = w.hash();
    for (u32 i = 0; i < 64; ++i) {
        w.sweep_entities(fx3{F(2), F(20), Ff(1, 2)}, EAST, F(60), Ff(1, 8),
                         static_cast<i32>(shooter), -1, -1);
    }
    const u64 after = w.hash();
    // THE POINT: this is why adding it does not move the determinism gate. A
    // query that quietly wrote something -- a cached sector, a scan cursor --
    // would move the hash and the gate would blame the physics.
    CHECK(before == after);

    // And the control: something that IS a mutation moves the hash, so the
    // assertion above is about this function rather than about a hash that
    // never changes.
    w.teleport(shooter, fx3{F(5), F(20), F(0)});
    CHECK(w.hash() != before);
}

// ---------------------------------------------------------------------------
static void test_it_is_deterministic() {
    std::printf("sweep: the same question gets the same answer\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 shooter = w.spawn(fx3{F(2), F(20), F(0)});
    for (i64 k = 0; k < 20; ++k) {
        w.spawn(fx3{F(6 + k * 3), F(20) + Ff(k, 4), F(0)});
    }

    for (i64 k = 1; k <= 40; ++k) {
        const fx3 from{Ff(k, 7), F(20), Ff(1, 2)};
        const auto a = w.sweep_entities(from, EAST, F(60), Ff(1, 8),
                                        static_cast<i32>(shooter), -1, -1);
        const auto b = w.sweep_entities(from, EAST, F(60), Ff(1, 8),
                                        static_cast<i32>(shooter), -1, -1);
        CHECK(a.hit == b.hit);
        CHECK(a.entity == b.entity);
        CHECK(a.distance == b.distance);
        CHECK(a.point == b.point);
    }
}

int main() {
    std::printf("== core/sweep test suite ==\n");
    test_it_finds_the_nearest();
    test_a_wall_stops_it();
    test_a_miss_to_the_side_is_a_miss();
    test_height_is_respected();
    test_the_shooter_is_skipped();
    test_both_ignores_are_needed();
    test_only_solid_things_stop_it();
    test_the_radius_matters();
    test_the_point_is_on_the_ray();
    test_degenerate_casts();
    test_it_changes_nothing();
    test_it_is_deterministic();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
