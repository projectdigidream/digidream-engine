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

// A swing hits what is in front of it, and the arc has edges.
//
// Every assertion here is paired with the same swing that must NOT connect,
// because "the attack found something" is satisfied by an implementation that
// hits everything in the room. What is being pinned down is the SHAPE: a
// bounded reach, a bounded arc, and a bounded height.
static void test_a_swing_hits_what_is_in_front_of_it() {
    std::printf("props: a swing has a reach, an arc and a height, and each of them ends\n");

    // Put a target at `angle` around the agent at `dist`, swing east, and run
    // the whole swing -- because a swing is a volume moving through time and
    // asking what it touched on tick one asks where the sword STARTED.
    auto swing_at = [](double angle_turns, double dist, fx z) {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
        // Placed by hand rather than via the engine's trig, so the test does
        // not depend on the thing it is testing being right.
        const fx dx = fx::from_frac(static_cast<i64>(dist * 1000.0 * __builtin_cos(angle_turns * 6.283185307179586)), 1000);
        const fx dy = fx::from_frac(static_cast<i64>(dist * 1000.0 * __builtin_sin(angle_turns * 6.283185307179586)), 1000);
        const u32 p = w.spawn_prop(10, fx3{F(20) + dx, F(20) + dy, z});
        w.prop_mut(p).radius = fx::from_frac(1, 5);
        w.prop_mut(p).height = fx::from_frac(9, 10);

        bool hit = false;
        for (u32 t = 0; t < 20; ++t) {
            proto::InputFrame f;
            // Pressed on the first tick only: a swing is started by a press and
            // then runs on its own.
            if (t == 0) f.buttons = proto::button::ATTACK_PRIMARY;
            f.aim = brad(0u);                       // east
            w.set_input(a, f);
            w.step();
            for (u32 i = 0; i < w.attacks().count(); ++i) {
                if (w.attacks().at(i).target == p) hit = true;
            }
        }
        return hit;
    };

    // Straight ahead, well inside the reach. The sweep passes through it.
    CHECK(swing_at(0.0, 1.0, F(0)));
    // Reach is 2.0 to the SURFACE and this prop's radius is 0.2, so 2.2 from
    // centre to centre is the boundary. Inside it and outside it:
    CHECK(swing_at(0.0, 2.0, F(0)));
    CHECK(!swing_at(0.0, 3.0, F(0)));

    // The sweep runs from -1/8 of a turn to +1/8 with a half-arc of 1/8, so
    // over its whole travel it covers -1/4 to +1/4 -- a quarter turn either
    // side. Inside that, and outside it:
    CHECK(swing_at(1.0 / 8.0, 1.0, F(0)));
    CHECK(swing_at(-1.0 / 8.0, 1.0, F(0)));
    CHECK(!swing_at(3.0 / 8.0, 1.0, F(0)));
    CHECK(!swing_at(-3.0 / 8.0, 1.0, F(0)));

    // CONTROL: directly behind. If this connected, the arc would not be one.
    CHECK(!swing_at(0.5, 1.0, F(0)));

    // And height: a barrel well above is not hit by a swing at floor level.
    CHECK(!swing_at(0.0, 1.0, F(8)));
}

// The swing takes time, and that is observable rather than decorative.
//
// Two things follow from a collider that moves, and neither is true of the
// one-tick arc test this replaces: a target is hit on the tick the sweep
// REACHES it, not on the tick the button went down; and a target that leaves
// before the sweep arrives is missed.
static void test_the_swing_is_not_instantaneous() {
    std::printf("props: the collider sweeps, so WHEN it connects depends on where the target is\n");

    // Two targets, at the two ends of the sweep. The one the sweep starts at is
    // hit first; the one it ends at is hit last. If the swing were an instant,
    // both would land on the same tick.
    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    // -1/8 turn from east is south-east; +1/8 is north-east.
    const u32 first = w.spawn_prop(10, fx3{fx::from_frac(20707, 1000), fx::from_frac(19293, 1000), F(0)});
    const u32 last  = w.spawn_prop(10, fx3{fx::from_frac(20707, 1000), fx::from_frac(20707, 1000), F(0)});
    w.prop_mut(first).radius = fx::from_frac(1, 5);
    w.prop_mut(last).radius  = fx::from_frac(1, 5);

    i32 first_tick = -1;
    i32 last_tick = -1;
    for (u32 t = 0; t < 20; ++t) {
        proto::InputFrame f;
        if (t == 0) f.buttons = proto::button::ATTACK_PRIMARY;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
        for (u32 i = 0; i < w.attacks().count(); ++i) {
            const u32 target = w.attacks().at(i).target;
            if (target == first && first_tick < 0) first_tick = static_cast<i32>(t);
            if (target == last && last_tick < 0) last_tick = static_cast<i32>(t);
        }
    }

    std::printf("   swept onto the near target at tick %d, the far one at tick %d\n",
                first_tick, last_tick);
    CHECK(first_tick >= 0);
    CHECK(last_tick >= 0);
    // The whole claim: they are not the same tick, and they are in sweep order.
    CHECK(last_tick > first_tick);

    // And each was hit ONCE, not once per tick the collider covered it. A
    // ten-tick swing that reported ten hits would make every script deduplicate
    // something the engine knows and it does not.
    int first_hits = 0;
    w.reset();
    r.attach(w);
    const u32 b = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 p = w.spawn_prop(10, fx3{fx::from_frac(21, 1), F(20), F(0)});
    w.prop_mut(p).radius = fx::from_frac(1, 5);
    for (u32 t = 0; t < 20; ++t) {
        proto::InputFrame f;
        if (t == 0) f.buttons = proto::button::ATTACK_PRIMARY;
        f.aim = brad(0u);
        w.set_input(b, f);
        w.step();
        for (u32 i = 0; i < w.attacks().count(); ++i) {
            if (w.attacks().at(i).target == p) ++first_hits;
        }
    }
    CHECK(first_hits == 1);
}

// The characteristics are per-entity parameters, so a script can change them.
static void test_the_swing_is_shaped_by_parameters() {
    std::printf("props: reach and arc come from the parameter table, not from the engine\n");

    auto reaches = [](fx reach, fx dist) {
        static Room r;
        static World w;
        w.reset();
        r.attach(w);
        const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
        w.params_mut(a).attack_reach = reach;
        const u32 p = w.spawn_prop(10, fx3{F(20) + dist, F(20), F(0)});
        w.prop_mut(p).radius = fx::from_frac(1, 5);
        bool hit = false;
        for (u32 t = 0; t < 20; ++t) {
            proto::InputFrame f;
            if (t == 0) f.buttons = proto::button::ATTACK_PRIMARY;
            f.aim = brad(0u);
            w.set_input(a, f);
            w.step();
            for (u32 i = 0; i < w.attacks().count(); ++i) if (w.attacks().at(i).target == p) hit = true;
        }
        return hit;
    };

    // A target at 5 units is out of a default 2-unit reach and inside a spear's.
    CHECK(!reaches(F(2), F(5)));
    CHECK(reaches(F(8), F(5)));

    // A thrust -- from and to the same angle -- does not sweep, so it covers
    // only its own arc and misses what a swing would have caught on the way.
    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    w.params_mut(a).attack_sweep_from = fx::zero();
    w.params_mut(a).attack_sweep_to   = fx::zero();
    w.params_mut(a).attack_half_arc   = fx::from_frac(1, 32);   // narrow
    // North-east, which a default swing reaches and a narrow thrust does not.
    const u32 p = w.spawn_prop(10, fx3{fx::from_frac(20707, 1000), fx::from_frac(20707, 1000), F(0)});
    w.prop_mut(p).radius = fx::from_frac(1, 5);
    bool hit = false;
    for (u32 t = 0; t < 20; ++t) {
        proto::InputFrame f;
        if (t == 0) f.buttons = proto::button::ATTACK_PRIMARY;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
        for (u32 i = 0; i < w.attacks().count(); ++i) if (w.attacks().at(i).target == p) hit = true;
    }
    CHECK(!hit);
}

// Not swinging hits nothing, and holding the button is one swing.
static void test_the_swing_is_edge_triggered() {
    std::printf("props: holding attack swings once, and not pressing it swings never\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 p = w.spawn_prop(10, fx3{F(21), F(20), F(0)});
    w.prop_mut(p).radius = fx::from_frac(1, 5);

    // CONTROL: standing right next to it, facing it, and not pressing.
    // Without this every assertion below could be satisfied by an
    // implementation that reports a hit on every tick regardless of input.
    int idle = 0;
    for (u32 t = 0; t < 10; ++t) {
        proto::InputFrame f;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
        idle += static_cast<int>(w.attacks().count());
    }
    CHECK(idle == 0);

    // Held down for thirty ticks is one swing.
    int hits = 0;
    for (u32 t = 0; t < 30; ++t) {
        proto::InputFrame f;
        f.buttons = proto::button::ATTACK_PRIMARY;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
        hits += static_cast<int>(w.attacks().count());
    }
    CHECK(hits == 1);

    // Released and pressed again is a second.
    {
        proto::InputFrame f;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
    }
    {
        proto::InputFrame f;
        f.buttons = proto::button::ATTACK_PRIMARY;
        f.aim = brad(0u);
        w.set_input(a, f);
        w.step();
    }
    CHECK(w.attacks().count() == 1);
}

// Agents and props come back through one list, which is what lets a script that
// damages both write it once.
static void test_a_swing_reports_agents_and_props_alike() {
    std::printf("props: a swing reports agents and props through one list\n");

    static Room r;
    static World w;
    w.reset();
    r.attach(w);
    const u32 a = w.spawn(fx3{F(20), F(20), F(0)});
    const u32 victim = w.spawn(fx3{fx::from_frac(21, 1), F(20), F(0)});
    const u32 barrel = w.spawn_prop(10, fx3{fx::from_frac(211, 10), F(20), F(0)});
    w.prop_mut(barrel).radius = fx::from_frac(1, 5);

    proto::InputFrame f;
    f.buttons = proto::button::ATTACK_PRIMARY;
    f.aim = brad(0u);
    w.set_input(a, f);
    w.step();

    bool hit_agent = false;
    bool hit_prop = false;
    for (u32 i = 0; i < w.attacks().count(); ++i) {
        const AttackEvent& e = w.attacks().at(i);
        CHECK(e.attacker == a);
        if (e.target == victim) hit_agent = true;
        if (e.target == barrel) hit_prop = true;
    }
    CHECK(hit_agent);
    CHECK(hit_prop);
    // And never the attacker itself, which a radius test written without the
    // exclusion reports first because the distance is zero.
    for (u32 i = 0; i < w.attacks().count(); ++i) CHECK(w.attacks().at(i).target != a);
}

int main() {
    test_props_share_the_entity_index_space();
    test_a_swing_hits_what_is_in_front_of_it();
    test_the_swing_is_not_instantaneous();
    test_the_swing_is_shaped_by_parameters();
    test_the_swing_is_edge_triggered();
    test_a_swing_reports_agents_and_props_alike();
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
