//
// Entities that are not players (design §4's sprite and mesh entities).
//
// The engine's whole job here is to say which agent is standing inside which
// prop. What a prop IS, what it looks like, and what touching one does are all
// somewhere else -- the catalogue, the renderer and the script respectively --
// so what can be asserted here is exactly the overlap, and the controls are
// about the ways an overlap test passes for the wrong reason:
//
//   vertical    a key on a balcony is not picked up from the floor below it,
//               however close it is in plan view.
//   horizontal  a key across the room is not picked up, however level with you.
//   identity    despawning a prop invalidates handles to it, rather than
//               leaving them pointed at whatever lands in the slot next.
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

// One flat room, 40 x 40, floor at 0.
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

static bool touched(const World& w, u32 agent, u32 prop) {
    for (u32 i = 0; i < w.overlaps().count(); ++i) {
        const OverlapEvent& e = w.overlaps().at(i);
        if (e.agent == agent && e.prop == prop) return true;
    }
    return false;
}

// Props live above the agents in one index space, which is what lets the wire
// carry both without a handle domain or a second message.
static void test_props_share_the_entity_index_space() {
    std::printf("props: a prop is an entity in the slots above the agents\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(10), F(10), F(0)});
    const u32 p = w.spawn_prop(7, fx3{F(20), F(20), F(0)});

    CHECK(a < MAX_AGENTS);
    CHECK(World::is_prop(p));
    CHECK(p >= MAX_AGENTS && p < MAX_ENTITIES);
    CHECK(!World::is_prop(a));
    CHECK(w.prop(p).kind == 7);
    CHECK(w.prop(p).active);
}

static void test_standing_in_a_prop_is_reported() {
    std::printf("props: an agent standing in a prop is reported, and only then\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(10), F(10), F(0)});
    // Right on top of the agent.
    const u32 near_p = w.spawn_prop(1, fx3{F(10), F(10), F(0)});
    // CONTROL 1: the same prop, across the room. Level with the agent and
    // identical in every way except distance.
    const u32 far_p = w.spawn_prop(1, fx3{F(30), F(10), F(0)});
    // CONTROL 2: directly overhead, well above the body. A key on a balcony is
    // not picked up from underneath it, and in plan view this one is a perfect
    // hit -- which is exactly what a test with no vertical check would report.
    const u32 high_p = w.spawn_prop(1, fx3{F(10), F(10), F(12)});

    w.step();

    CHECK(touched(w, a, near_p));
    CHECK(!touched(w, a, far_p));
    CHECK(!touched(w, a, high_p));
    CHECK(w.overlaps().count() == 1);
}

// The overlap holds while you stand there, rather than firing once.
//
// Deliberate: "picked up once" is a decision the script makes by removing the
// prop, and a key that vanishes is self-limiting. An engine-side edge trigger
// would duplicate that for pickups and get it wrong for everything meant to
// keep working while you stand in it.
static void test_the_overlap_holds_while_you_stand_there() {
    std::printf("props: the overlap is reported every tick it holds, not once\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(10), F(10), F(0)});
    const u32 p = w.spawn_prop(1, fx3{F(10), F(10), F(0)});

    int ticks_reported = 0;
    for (u32 t = 0; t < 20; ++t) {
        w.step();
        if (touched(w, a, p)) ++ticks_reported;
    }
    CHECK(ticks_reported == 20);

    // And removing it stops the reports immediately, which is the mechanism a
    // pickup actually uses.
    w.despawn_prop(p);
    w.step();
    CHECK(w.overlaps().count() == 0);
}

static void test_a_despawned_prop_takes_its_handles_with_it() {
    std::printf("props: despawning bumps the generation, so old handles are stale\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 p = w.spawn_prop(1, fx3{F(10), F(10), F(0)});
    const u8 gen_before = w.generation(p);
    w.despawn_prop(p);
    CHECK(!w.prop(p).active);
    CHECK(w.generation(p) != gen_before);

    // The next prop lands in the same slot -- that is the point of a slot -- and
    // must not be reachable through a handle to the one that died there.
    const u32 q = w.spawn_prop(2, fx3{F(11), F(11), F(0)});
    CHECK(q == p);
    CHECK(w.prop(q).kind == 2);
    CHECK(w.generation(q) != gen_before);
}

// Props are part of the hashed world, so a client and server that disagree
// about them disagree about the state hash rather than about nothing.
static void test_props_are_hashed() {
    std::printf("props: a prop changes the world hash; an identical world does not\n");

    static Room r;
    static World a;
    static World b;
    a.reset(); r.attach(a);
    b.reset(); r.attach(b);
    CHECK(a.hash() == b.hash());

    a.spawn_prop(1, fx3{F(10), F(10), F(0)});
    CHECK(a.hash() != b.hash());

    b.spawn_prop(1, fx3{F(10), F(10), F(0)});
    CHECK(a.hash() == b.hash());

    // CONTROL: the same prop somewhere else is a different world. Without this,
    // "the hash changed" could mean it counts props and ignores where they are.
    static World c;
    c.reset(); r.attach(c);
    c.spawn_prop(1, fx3{F(11), F(10), F(0)});
    CHECK(a.hash() != c.hash());

    // And a different KIND in the same place, which is what tells a key from a
    // skull on the wire.
    static World d;
    d.reset(); r.attach(d);
    d.spawn_prop(2, fx3{F(10), F(10), F(0)});
    CHECK(a.hash() != d.hash());
}

int main() {
    test_props_share_the_entity_index_space();
    test_standing_in_a_prop_is_reported();
    test_the_overlap_holds_while_you_stand_there();
    test_a_despawned_prop_takes_its_handles_with_it();
    test_props_are_hashed();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
