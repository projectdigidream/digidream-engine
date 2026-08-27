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
    for (u32 i = 0; i < w.prop_events().count(); ++i) {
        const PropEvent& e = w.prop_events().at(i);
        if (e.kind == PropEventKind::Touch && e.agent == agent && e.prop == prop) return true;
    }
    return false;
}

static i32 used_prop(const World& w, u32 agent) {
    for (u32 i = 0; i < w.prop_events().count(); ++i) {
        const PropEvent& e = w.prop_events().at(i);
        if (e.kind == PropEventKind::Use && e.agent == agent) return static_cast<i32>(e.prop);
    }
    return -1;
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
    CHECK(w.prop_events().count() == 1);
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
    CHECK(w.prop_events().count() == 0);
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

// A solid prop stops a body; the same prop without the flag does not.
//
// The control is the whole test. Walking east and stopping proves nothing on
// its own -- a wall, a bug in the mover and a prop all look identical from the
// end position. What distinguishes them is that removing ONE FLAG from the
// catalogue lets the same walk pass straight through.
static void test_a_solid_prop_stops_a_body() {
    std::printf("props: `solid` stops a body, and nothing else about the prop does\n");

    auto walk_east = [](u8 flags) {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{F(5), F(20), F(0)});
        const u32 p = w.spawn_prop(1, fx3{F(20), F(20), F(0)});
        w.prop_mut(p).flags = flags;
        w.prop_mut(p).radius = F(2);
        w.prop_mut(p).height = F(4);
        for (u32 t = 0; t < 240; ++t) {
            proto::InputFrame f;
            f.aim = brad(0u);            // east
            f.move_x = 32767;
            w.set_input(a, f);
            w.step();
        }
        return w.agent(a).pos.x;
    };

    const fx blocked = walk_east(prop_flag::SOLID);
    const fx through = walk_east(0);

    std::printf("   solid: stopped at x=%.2f    not solid: reached x=%.2f\n",
                static_cast<double>(blocked.raw) / 4294967296.0,
                static_cast<double>(through.raw) / 4294967296.0);

    // The prop's centre is at 20 with radius 2, and the body's radius is 0.5,
    // so a body pushed out of it rests at 20 - 2.5 = 17.5.
    CHECK(blocked < F(18));
    CHECK(blocked > F(17));
    // CONTROL: without the flag the same walk crosses it entirely and ends
    // against the room's east wall at 40.
    CHECK(through > F(30));
}

// Standing on top of one is not standing inside it.
static void test_a_solid_prop_is_not_a_wall_from_above() {
    std::printf("props: a solid prop stops being in the way once you are above it\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);

    const u32 a = w.spawn(fx3{F(20), F(20), F(8)});   // well above it
    const u32 p = w.spawn_prop(1, fx3{F(20), F(20), F(0)});
    w.prop_mut(p).flags = prop_flag::SOLID;
    w.prop_mut(p).radius = F(2);
    w.prop_mut(p).height = F(4);

    // Directly overhead, which in plan view is a total overlap. A push-out that
    // ignored height would fling this body sideways for no visible reason.
    w.step();
    CHECK(w.agent(a).pos.x == F(20));
    CHECK(w.agent(a).pos.y == F(20));
}

static void test_use_reaches_a_usable_prop() {
    std::printf("props: pressing use names the prop in front, and only a usable one\n");

    auto press = [](u8 flags, fx at_x) {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{at_x, F(20), F(0)});
        const u32 p = w.spawn_prop(1, fx3{F(20), F(20), F(0)});
        w.prop_mut(p).flags = flags;
        proto::InputFrame f;
        f.buttons = proto::button::INTERACT;
        f.aim = brad(0u);                // east, toward the prop
        w.set_input(a, f);
        w.step();
        return used_prop(w, a);
    };

    // One unit away, facing it.
    CHECK(press(prop_flag::USABLE, F(19)) == static_cast<i32>(MAX_AGENTS));

    // CONTROL 1: the same prop without the flag is scenery. If this fired, the
    // test above would only be saying "there is a prop nearby".
    CHECK(press(0, F(19)) < 0);

    // CONTROL 2: usable, but out of reach. Reach is 1.5 plus the prop's own
    // radius of 0.5, so 2.0 -- and this stands 4 away.
    CHECK(press(prop_flag::USABLE, F(16)) < 0);

    // CONTROL 3: in reach, facing away. Without this, "found it" could mean the
    // search ignores where the player is looking.
    {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{F(19), F(20), F(0)});
        const u32 p = w.spawn_prop(1, fx3{F(20), F(20), F(0)});
        w.prop_mut(p).flags = prop_flag::USABLE;
        proto::InputFrame f;
        f.buttons = proto::button::INTERACT;
        f.aim = brad(0x80000000u);       // west, away from it
        w.set_input(a, f);
        w.step();
        CHECK(used_prop(w, a) < 0);
    }
}

int main() {
    test_props_share_the_entity_index_space();
    test_a_solid_prop_stops_a_body();
    test_a_solid_prop_is_not_a_wall_from_above();
    test_use_reaches_a_usable_prop();
    test_standing_in_a_prop_is_reported();
    test_the_overlap_holds_while_you_stand_there();
    test_a_despawned_prop_takes_its_handles_with_it();
    test_props_are_hashed();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
