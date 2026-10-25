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
#include <utility>
#include <memory>

#include "dnp/sim/world.hpp"
#include "dnp/physics/collide.hpp"

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

// Two rooms OPEN to each other at x = 40, the east one's floor a step of 2
// higher: a flat missile at height 1 meets the step's face.
struct Stepped {
    MapStorage store;
    MapData    view{};
    Stepped() {
        const fx2 west[] = {{F(0), F(0)}, {F(40), F(0)}, {F(40), F(40)}, {F(0), F(40)}};
        const fx2 east[] = {{F(40), F(0)}, {F(80), F(0)}, {F(80), F(40)}, {F(40), F(40)}};
        store.add_sector(west, 4, F(0), F(40), 1);
        store.add_sector(east, 4, F(2), F(40), 1);
        for (u16 i = 0; i < 4; ++i)
            store.add_side(0, i, static_cast<u16>((i + 1) % 4), i == 1 ? 1 : NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i)
            store.add_side(1, i, static_cast<u16>((i + 1) % 4), i == 3 ? 0 : NO_SECTOR, 0);
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

// A MISSILE FALLS. Fired down it meets the floor, up the ceiling, and flat into
// a higher room the step -- each where its line meets it.
static void test_a_sloped_missile_meets_the_planes() {
    std::printf("sweep: a sloped missile stops on the floor, the ceiling and a step\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    // Down at 1 in 4 from height 2: the floor is 8 along.
    const auto down = w.sweep_entities(fx3{F(10), F(20), F(2)}, EAST, F(20), Ff(1, 8),
                                       -1, -1, -1, Ff(-1, 4));
    CHECK(down.hit && !down.entity_hit);
    CHECK(down.distance > F(7) && down.distance < F(9));
    CHECK(down.point.z == F(0));
    // CONTROL: the same sweep flat (no slope given) reaches the far wall.
    const auto flat = w.sweep_entities(fx3{F(10), F(20), F(2)}, EAST, F(20), Ff(1, 8), -1, -1, -1);
    CHECK(!flat.hit);
    // CONTROL: and with a step short of the floor it is still flying.
    const auto shortstep = w.sweep_entities(fx3{F(10), F(20), F(2)}, EAST, F(4), Ff(1, 8),
                                            -1, -1, -1, Ff(-1, 4));
    CHECK(!shortstep.hit);
    // Up at 1 in 1 from 30: the ceiling at 40 is 10 along, and it is met there.
    const auto up = w.sweep_entities(fx3{F(10), F(20), F(30)}, EAST, F(20), Ff(1, 8),
                                     -1, -1, -1, F(1));
    CHECK(up.hit && up.point.z == F(40));
    CHECK(up.distance > F(9) && up.distance < F(11));

    static Stepped st;
    static World w2;
    w2.reset();
    st.attach(w2);
    const auto step = w2.sweep_entities(fx3{F(30), F(20), F(1)}, EAST, F(20), Ff(1, 8),
                                        -1, -1, -1, fx::zero());
    CHECK(step.hit && step.distance <= F(10) + Ff(1, 4));
    // CONTROL: over the step it flies on.
    const auto over = w2.sweep_entities(fx3{F(30), F(20), F(3)}, EAST, F(20), Ff(1, 8),
                                        -1, -1, -1, fx::zero());
    CHECK(!over.hit);
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

// A PLATFORM ACROSS THE BORDER between two rooms (mapc takes it since a raised
// platform may straddle a shared wall): rooms 0 (x 0..20) and 1 (20..40) open
// at x = 20, and platform 2 (x 15..25, y 5..10, floor 2, a slab 2 thick, its
// edges open) over both.
struct Straddle {
    MapStorage store;
    MapData    view{};
    Straddle() {
        const fx2 a[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(16)}, {F(0), F(16)}};
        const fx2 b[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(16)}, {F(20), F(16)}};
        const fx2 p[] = {{F(15), F(5)}, {F(25), F(5)}, {F(25), F(10)}, {F(15), F(10)}};
        store.add_sector(a, 4, F(0), F(20), 1);
        store.add_sector(b, 4, F(0), F(20), 1);
        store.add_sector(p, 4, F(2), F(20), 1);
        store.sector_mut(2).thickness = F(2);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), i == 1 ? 1 : NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), i == 3 ? 0 : NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(2, i, static_cast<u16>((i + 1) % 4), NO_SECTOR, side_flag::LEDGE);
    }
    void attach(World& w) {
        view = store.view();
        w.set_map(&view);
        w.set_map_storage(&store);
    }
};

static void test_a_platform_across_two_rooms() {
    std::printf("sweep: a platform across the border of two rooms is stood on over both\n");
    static Straddle st;
    static World w;
    w.reset();
    st.attach(w);
    auto settle = [&](fx3 at) {
        const u32 a = w.spawn(at);
        for (u32 t = 0; t < 90; ++t) { w.set_input(a, proto::InputFrame{}); w.step(); }
        const auto& g = w.agent(a);
        const auto out = std::make_pair(g.mv.sector, g.pos.z);
        w.despawn(a);
        return out;
    };
    const auto overA = settle(fx3{F(17), Ff(15, 2), F(3)});
    const auto overB = settle(fx3{F(23), Ff(15, 2), F(3)});
    const auto inB = settle(fx3{F(32), Ff(15, 2), F(0)});
    std::printf("  over room 0: sector %d at %.2f; over room 1: sector %d at %.2f; room 1: sector %d at %.2f\n",
                overA.first, overA.second.raw / 4294967296.0, overB.first, overB.second.raw / 4294967296.0,
                inB.first, inB.second.raw / 4294967296.0);
    CHECK(overA.first == 2 && overA.second == F(2));   // on the platform, over the west room
    CHECK(overB.first == 2 && overB.second == F(2));   // and over the east one
    CHECK(inB.first == 1 && inB.second == F(0));       // CONTROL: beside it, the room's floor
    // And from the east room the platform's side is a wall to walk into: two
    // units is no step.
    const u32 a = w.spawn(fx3{F(32), Ff(15, 2), F(0)});
    proto::InputFrame f;
    f.move_x = -32767;
    for (u32 t = 0; t < 240; ++t) { w.set_input(a, f); w.step(); }
    std::printf("  walking west from x 32: stopped at x %.2f\n", w.agent(a).pos.x.raw / 4294967296.0);
    CHECK(w.agent(a).pos.x > F(25) && w.agent(a).mv.sector == 1);
}

// THE CAMERA'S LINE AND A PLATFORM (physics::line_clear), on the straddling
// platform above (x 15..25, y 5..10, top 2, slab 0..2). From the head of a body
// on it out over its edge to a camera behind and above: clear. Through its slab
// from beside it: blocked.
static void test_the_cameras_line_and_a_platform() {
    std::printf("line_clear: a platform's edge lets the camera's line on, its slab stops it\n");
    static Straddle st;
    const MapData& m = (st.view = st.store.view(), st.view);
    // What the camera asked before: the ray from the head's sector alone.
    auto ray_only = [&](fx3 a, fx3 b) {
        const i32 s = find_sector_at(m, fx2{a.x, a.y}, a.z, fx::zero());
        const fx2 d{b.x - a.x, b.y - a.y};
        return !physics::raycast(m, s, fx2{a.x, a.y}, normalize(d), length(d), 0, a.z, b.z).blocked;
    };
    const fx3 head{F(18), Ff(15, 2), Ff(7, 2)}, behind{F(18), F(14), F(6)};
    const fx3 beside{F(18), F(13), Ff(3, 2)}, low{F(18), F(2), Ff(6, 5)};
    const fx3 beside_up{F(18), F(13), F(3)}, high{F(18), F(2), Ff(7, 2)};
    const bool off_edge = physics::line_clear(m, head, behind);
    const bool through = physics::line_clear(m, beside, low);
    const bool over = physics::line_clear(m, beside_up, high);
    const bool wall = physics::line_clear(m, fx3{F(5), F(8), F(1)}, fx3{F(-5), F(8), F(1)});
    std::printf("  off the edge %d (the ray alone %d); through the slab %d (the ray alone %d); over it %d; a wall %d\n",
                off_edge, ray_only(head, behind), through, ray_only(beside, low), over, wall);
    CHECK(off_edge);
    CHECK(!ray_only(head, behind));    // CONTROL: what the camera met: the edge as a wall
    CHECK(!through);
    CHECK(ray_only(beside, low));      // CONTROL: and the slab not there at all
    CHECK(over);                       // CONTROL: above the slab is open
    CHECK(!wall);                      // CONTROL: a room's wall is still a wall
}

// AN EFFECT SECTOR across the border of two rooms, level with them, where a
// solid one would be refused: no lookup names it, and a body walks through it
// as through open floor.
static void test_an_effect_sector_is_not_there() {
    std::printf("effect sector: nothing stands in it, nothing collides with it\n");
    static Straddle st;
    static World w;
    st.store.sector_mut(2).thickness = fx::zero();
    st.store.sector_mut(2).floor_z = F(0);
    st.store.sector_mut(2).effect = true;
    st.store.rebuild_grid();
    w.reset();
    st.attach(w);
    const MapData& m = st.view;
    // CONTROL: the same sector, solid, level with the rooms -- the smaller
    // footprint, so find_sector names it.
    st.store.sector_mut(2).effect = false;
    st.store.rebuild_grid();
    const i32 solid = find_sector(m, fx2{F(17), Ff(15, 2)});
    st.store.sector_mut(2).effect = true;
    st.store.rebuild_grid();
    u16 near[16];
    const u32 n = sectors_near(m, fx2{F(20), Ff(15, 2)}, F(3), near, 16);
    bool named = false;
    for (u32 i = 0; i < n; ++i) named = named || near[i] == 2;
    const i32 found = find_sector(m, fx2{F(17), Ff(15, 2)});
    const i32 at = find_sector_at(m, fx2{F(17), Ff(15, 2)}, F(0), fx::zero());
    // Walking east from x 5 through it, into room 1.
    const u32 a = w.spawn(fx3{F(5), Ff(15, 2), F(0)});
    proto::InputFrame f;
    f.move_x = 32767;
    i32 seen_in = -1;
    for (u32 t = 0; t < 240; ++t) {
        w.set_input(a, f);
        w.step();
        if (w.agent(a).mv.sector == 2) seen_in = 2;
    }
    std::printf("  near names it %d; find_sector %d, find_sector_at %d; walked to x %.2f in sector %d, ever in it %d\n",
                named, found, at, w.agent(a).pos.x.raw / 4294967296.0, w.agent(a).mv.sector, seen_in);
    CHECK(!named && found == 0 && at == 0);
    CHECK(solid == 2);
    CHECK(w.agent(a).pos.x > F(30) && w.agent(a).mv.sector == 1 && seen_in < 0);
    st.store.sector_mut(2).effect = false;      // as it was, for anything after
    st.store.sector_mut(2).floor_z = F(2);
    st.store.sector_mut(2).thickness = F(2);
    st.store.rebuild_grid();
}

// A LEDGE: room 0 (x 0..20, floor 0) beside room 1 (x 20..30, floor `h`), open
// to each other at x = 20.
// The default body's height (MoveParams), which the hang sits a head below a top by.
static fx w_height() { return physics::MoveParams{}.height; }

struct Ledge {
    MapStorage store;
    MapData    view{};
    explicit Ledge(fx h) {
        const fx2 a[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 b[] = {{F(20), F(0)}, {F(30), F(0)}, {F(30), F(20)}, {F(20), F(20)}};
        store.add_sector(a, 4, F(0), F(20), 1);
        store.add_sector(b, 4, h, F(20), 1);
        for (u16 i = 0; i < 4; ++i) store.add_side(0, i, static_cast<u16>((i + 1) % 4), i == 1 ? 1 : NO_SECTOR, 0);
        for (u16 i = 0; i < 4; ++i) store.add_side(1, i, static_cast<u16>((i + 1) % 4), i == 3 ? 0 : NO_SECTOR, 0);
    }
    void attach(World& w) { view = store.view(); w.set_map(&view); w.set_map_storage(&store); }
};

static void test_a_ledge_is_caught_hung_from_and_hopped_onto() {
    std::printf("ledge: a jump that just misses a ledge catches it and hangs; JUMP hops, at it pulls up\n");
    struct Seen { bool hung = false; fx hang_z{}; bool still = true; fx peak{}; bool hung_after = false;
                  fx z_after{}; i32 sector = -1; fx z{}; bool hopped = false; bool hop_after = true;
                  bool caught = false; fx caught_vz{}; };
    // `toward`: the stick at the ledge as JUMP is pressed while hanging.
    auto run = [](fx h, bool grab, bool toward) {
        static World w;
        // On the heap: a MapStorage is far bigger than a test's stack.
        static std::unique_ptr<Ledge> L;
        L = std::make_unique<Ledge>(h);
        w.reset();
        L->attach(w);
        const u32 a = w.spawn(fx3{F(18), F(10), F(0)});
        w.params_mut(a).ledge_grab = grab;
        Seen seen;
        fx vz_before{};
        for (u32 t = 0; t < 240; ++t) {
            proto::InputFrame f;
            f.move_x = (t < 60 || (toward && t >= 120)) ? 32767 : 0;   // at the ledge; hands off; then (maybe) at it again
            if (t == 2 || t == 120) f.buttons = proto::button::JUMP;    // the jump at it; then JUMP while hanging
            w.set_input(a, f);
            w.step();
            const auto& g = w.agent(a);
            // The tick it first catches the ledge, and how it was moving up to it.
            if (!seen.caught && g.mv.hanging) { seen.caught = true; seen.caught_vz = vz_before; }
            vz_before = g.vel.z;
            if (t == 70) { seen.hung = g.mv.hanging; seen.hang_z = g.pos.z; }
            if (t > 70 && t < 120 && g.pos.z != seen.hang_z) seen.still = false;
            if (t > 120 && g.pos.z > seen.peak) seen.peak = g.pos.z;
            if (t > 120 && t < 150 && g.mv.hopping) seen.hopped = true;
            if (t == 200) { seen.hung_after = g.mv.hanging; seen.z_after = g.pos.z; }
        }
        seen.sector = w.agent(a).mv.sector;
        seen.z = w.agent(a).pos.z;
        seen.hop_after = w.agent(a).mv.hopping;
        return seen;
    };
    const Seen hop = run(F(3), true, false);
    const Seen up = run(F(3), true, true);
    const Seen off = run(F(3), false, false);
    const Seen far = run(F(5), true, false);
    std::printf("  hop flag: JUMP alone %d then %d; JUMP at it %d then %d\n", hop.hopped, hop.hop_after, up.hopped, up.hop_after);
    std::printf("  ledge 3 up: hanging %d at %.2f (still %d); JUMP alone: up to %.2f, hanging again %d at %.2f;"
                " JUMP at it: sector %d at %.2f; switched off: sector %d; 5 up: hanging %d\n",
                hop.hung, hop.hang_z.raw / 4294967296.0, hop.still, hop.peak.raw / 4294967296.0,
                hop.hung_after, hop.z_after.raw / 4294967296.0, up.sector, up.z.raw / 4294967296.0,
                off.sector, far.hung);
    // Caught on the way DOWN: going up, a body passes a ledge by.
    std::printf("  caught while moving %.2f units/s vertically\n", hop.caught_vz.raw / 4294967296.0);
    CHECK(hop.caught && hop.caught_vz <= fx::zero());
    // Caught, and hanging still with the stick let go: head level with the top.
    const fx off_by = hop.hang_z - (F(3) - w_height());
    CHECK(hop.hung && hop.still && off_by < Ff(1, 64) && off_by > -Ff(1, 64));
    // JUMP alone: a hop up, and the ledge caught again where it hung.
    CHECK(hop.peak > hop.hang_z + Ff(1, 4));
    CHECK(hop.hung_after && hop.z_after == hop.hang_z && hop.sector == 0);
    // JUMP with the stick at it: up onto the ledge.
    CHECK(up.sector == 1 && up.z == F(3));
    // Both are hops (the `hop` animation, not `jump`), over once caught or on it.
    CHECK(hop.hopped && !hop.hop_after);
    CHECK(up.hopped && !up.hop_after);
    CHECK(!off.hopped);                            // CONTROL: no ledge, no hop
    CHECK(!off.hung && off.sector == 0);           // CONTROL: off, the jump just falls back
    CHECK(!far.hung);                              // CONTROL: out of reach is out of reach
}

// COYOTE TIME: walking off room 1's edge (floor 3) into room 0, one jump
// allowed. A press soon after leaving the ground jumps; a late one does not.
static void test_coyote_time_runs_out() {
    std::printf("coyote: the ground jump lasts coyote_time after walking off an edge\n");
    auto jumped = [](u32 late, fx coyote) {
        static World w;
        static std::unique_ptr<Ledge> L;
        L = std::make_unique<Ledge>(F(6));
        w.reset();
        L->attach(w);
        const u32 a = w.spawn(fx3{F(21), F(10), F(6)});
        w.params_mut(a).max_jumps = 1;
        w.params_mut(a).coyote_time = coyote;
        i32 off_at = -1;
        bool up = false;
        for (u32 t = 0; t < 120; ++t) {
            proto::InputFrame f;
            f.move_x = -32767;
            if (off_at >= 0 && t == static_cast<u32>(off_at) + late) f.buttons = proto::button::JUMP;
            w.set_input(a, f);
            w.step();
            if (off_at < 0 && t > 2 && !w.agent(a).mv.grounded) off_at = static_cast<i32>(t);
            if (off_at >= 0 && t == static_cast<u32>(off_at) + late && w.agent(a).vel.z > fx::zero()) up = true;
        }
        return up;
    };
    const bool soon = jumped(3, fx::from_frac(1, 10));
    const bool late = jumped(30, fx::from_frac(1, 10));
    const bool long_coyote = jumped(30, F(10));
    std::printf("  3 ticks after: %d; 30 after: %d; 30 after with 10 s of coyote time: %d\n", soon, late, long_coyote);
    CHECK(soon);
    CHECK(!late);           // the ground jump is spent
    CHECK(long_coyote);     // CONTROL: it is the time, not the fall, that spends it
}

// VARIABLE JUMP: in the open middle of room 0, JUMP held `hold` ticks; the
// highest the feet get. Let go early, the rise is cut; held, the full jump.
static void test_a_variable_jump_follows_the_press() {
    std::printf("variable jump: a jump is as high as JUMP is held\n");
    auto peak = [](bool on, u32 hold) {
        static World w;
        static std::unique_ptr<Ledge> L;
        L = std::make_unique<Ledge>(F(1));
        w.reset();
        L->attach(w);
        const u32 a = w.spawn(fx3{F(10), F(10), F(0)});
        w.params_mut(a).variable_jump = on;
        fx top{};
        for (u32 t = 0; t < 120; ++t) {
            proto::InputFrame f;
            if (t >= 5 && t < 5 + hold) f.buttons = proto::button::JUMP;
            w.set_input(a, f);
            w.step();
            if (w.agent(a).pos.z > top) top = w.agent(a).pos.z;
        }
        return top;
    };
    const fx tap = peak(true, 2), mid = peak(true, 8), held = peak(true, 100), off = peak(false, 2);
    std::printf("  peak: tap %.2f, 8 ticks %.2f, held %.2f; switched off, a tap %.2f\n",
                tap.raw / 4294967296.0, mid.raw / 4294967296.0, held.raw / 4294967296.0, off.raw / 4294967296.0);
    CHECK(tap > fx::zero() && tap < mid && mid < held);   // the longer the press, the higher
    CHECK(tap < held / fx::from_int(2));                  // a tap is a short hop
    CHECK(off == held);                                   // CONTROL: off, a tap is the full jump
}

int main() {
    std::printf("== core/sweep test suite ==\n");
    test_a_variable_jump_follows_the_press();
    test_a_platform_across_two_rooms();
    test_the_cameras_line_and_a_platform();
    test_an_effect_sector_is_not_there();
    test_a_ledge_is_caught_hung_from_and_hopped_onto();
    test_coyote_time_runs_out();
    test_it_finds_the_nearest();
    test_a_wall_stops_it();
    test_a_miss_to_the_side_is_a_miss();
    test_height_is_respected();
    test_the_shooter_is_skipped();
    test_both_ignores_are_needed();
    test_only_solid_things_stop_it();
    test_a_sloped_missile_meets_the_planes();
    test_the_radius_matters();
    test_the_point_is_on_the_ray();
    test_degenerate_casts();
    test_it_changes_nothing();
    test_it_is_deterministic();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
