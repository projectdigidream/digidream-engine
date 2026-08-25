//
// The extended movement abilities of design §7.1.1.
//
// Each ability is tested for what it is SUPPOSED to do and, just as
// importantly, for the thing it must not do: a jump that re-triggers while
// held, a dash eaten by friction, hover that never runs out, an ability that
// still works while riding a rail. Those are the failures that look fine in a
// playtest and are wrong.
//
#include <cstdio>

#include "dnp/math/rng.hpp"
#include "dnp/physics/controller.hpp"

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

// One 40x40 room, floor 0, ceiling 40. The west wall is climbable, the north
// wall is grapple-attachable.
struct Room {
    fx2 pts[4];
    Sector secs[1];
    Side sides[4];
    MapData map;

    Room() {
        pts[0] = {F(0), F(0)};
        pts[1] = {F(40), F(0)};
        pts[2] = {F(40), F(40)};
        pts[3] = {F(0), F(40)};
        secs[0] = {0, 4, 0, 4, F(0), F(40), 0};
        sides[0] = {0, 1, NO_SECTOR, 0};
        sides[1] = {1, 2, NO_SECTOR, side_flag::GRAPPLE};    // east wall
        sides[2] = {2, 3, NO_SECTOR, 0};
        sides[3] = {3, 0, NO_SECTOR, side_flag::CLIMBABLE};  // west wall
        map = MapData{pts, 4, secs, 1, sides, 4};
    }
};

struct Agent {
    MoveParams p;
    MoveState  st;
    fx3 pos{};
    fx3 vel{};

    Agent(fx3 start = fx3{F(20), F(20), F(0)}) {
        pos = start;
        st.sector = 0;
        st.grounded = true;
        st.hover_fuel = p.hover_fuel_max;
        st.flight_fuel = p.flight_fuel_max;
    }

    void step(const MapData& map, MoveCommand cmd) {
        step_controller(map, p, st, cmd, pos, vel, sim::DT);
    }
    void run(const MapData& map, MoveCommand cmd, int ticks) {
        for (int i = 0; i < ticks; ++i) step(map, cmd);
    }
};

static MoveCommand C(u16 buttons = 0, fx mx = fx::zero(), fx my = fx::zero(), fx up = fx::zero()) {
    MoveCommand c;
    c.buttons = buttons;
    c.move = fx2{mx, my};
    c.up = up;
    return c;
}

// ------------------------------------------------------------------- jumping

static void test_multi_jump() {
    std::printf("ability: multi-jump is a parameter, not a code branch\n");
    Room room;

    // Held input must not re-trigger. This is the single most common way a jump
    // implementation is wrong, and it looks fine until someone holds the button.
    {
        Agent a;
        a.run(room.map, C(act::JUMP), 30);
        CHECK(a.st.jumps_used <= a.p.max_jumps);
        CHECK(a.st.jumps_used == 1);   // one press == one jump, however long it is held
    }

    // Releasing and pressing again spends the next charge, up to max_jumps.
    {
        Agent a;
        a.step(room.map, C(act::JUMP));
        a.step(room.map, C(0));
        a.step(room.map, C(act::JUMP));
        CHECK(a.st.jumps_used == 2);
        a.step(room.map, C(0));
        a.step(room.map, C(act::JUMP));
        CHECK(a.st.jumps_used == 2);   // exhausted, and stays exhausted
    }

    // max_jumps really is just a value: three works with no code change.
    {
        Agent a;
        a.p.max_jumps = 3;
        for (int i = 0; i < 3; ++i) { a.step(room.map, C(act::JUMP)); a.step(room.map, C(0)); }
        CHECK(a.st.jumps_used == 3);
    }

    // Landing replenishes.
    {
        Agent a;
        a.step(room.map, C(act::JUMP));
        CHECK(a.st.jumps_used == 1);
        a.run(room.map, C(0), 200);          // fall back down
        CHECK(a.st.grounded);
        CHECK(a.st.jumps_used == 0);
    }
}

// ---------------------------------------------------------------------- dash

static void test_dash() {
    std::printf("ability: dash adds to velocity and respects its cooldown\n");
    Room room;

    Agent a;
    a.p.max_dashes = 1;

    const fx before = a.vel.x;
    a.step(room.map, C(act::DASH, fx::one(), fx::zero()));
    CHECK(a.st.dash_timer > fx::zero());
    // Added on top of existing velocity, and NOT immediately eaten by friction
    // -- a dash that decelerates on the tick it starts feels like a nudge.
    CHECK(a.vel.x > before + F(20));
    // On the ground the cooldown is what paces dashing, not the charge count:
    // charges reset every grounded tick so a standing agent is never left
    // unable to act. Releasing and pressing again inside the cooldown does
    // nothing.
    CHECK(a.st.dash_cooldown_timer > fx::zero());
    a.step(room.map, C(0));
    a.step(room.map, C(act::DASH, fx::one(), fx::zero()));
    CHECK(a.st.dash_timer < a.p.dash_duration);   // no fresh dash started

    // max_dashes bounds AIRBORNE use, which is what it is for.
    {
        Agent air(fx3{F(20), F(20), F(20)});
        air.st.grounded = false;
        air.p.max_dashes = 1;
        air.p.dash_cooldown = fx::zero();
        air.step(room.map, C(act::DASH, fx::one(), fx::zero()));
        CHECK(air.st.dashes_used == 1);
        air.step(room.map, C(0));
        air.step(room.map, C(act::DASH, fx::one(), fx::zero()));
        CHECK(air.st.dashes_used == 1);           // charge spent, still airborne
        air.run(room.map, C(0), 400);             // land
        CHECK(air.st.grounded);
        CHECK(air.st.dashes_used == 0);           // and it is replenished
    }

    // Dash is high-velocity, so it must engage CCD rather than tunnel. Fire one
    // straight at a wall from close range and confirm containment.
    {
        Agent b(fx3{F(38), F(20), F(0)});
        b.p.dash_speed = F(200);
        b.step(room.map, C(act::DASH, fx::one(), fx::zero()));
        b.run(room.map, C(0), 10);
        CHECK(b.st.sector == 0);
        CHECK(point_in_sector(room.map, 0, fx2{b.pos.x, b.pos.y}));
    }
}

// ------------------------------------------------------------ hover / flight

static void test_hover() {
    std::printf("ability: hover holds altitude and burns fuel until empty\n");
    Room room;

    Agent a(fx3{F(20), F(20), F(20)});
    a.st.grounded = false;
    a.p.hover_enabled = true;
    a.p.hover_fuel_max = F(1);
    a.p.hover_drain = F(1);
    a.st.hover_fuel = F(1);

    const fx z0 = a.pos.z;
    a.run(room.map, C(act::HOVER), 30);   // half a second
    CHECK(a.st.hovering);
    // Barely descends: that is the point of hover, versus free fall which would
    // be several units in this time.
    CHECK(z0 - a.pos.z < F(1));
    CHECK(a.st.hover_fuel < F(1));

    // Fuel runs out and gravity resumes -- no special "forced landing" logic,
    // it is simply gravity being applied again.
    a.run(room.map, C(act::HOVER), 45);
    CHECK(a.st.hover_fuel == fx::zero());
    CHECK(!a.st.hovering);
    const fx z1 = a.pos.z;
    a.run(room.map, C(act::HOVER), 30);
    CHECK(z1 - a.pos.z > F(2));           // now genuinely falling

    // Landing refills it.
    a.run(room.map, C(0), 300);
    CHECK(a.st.grounded);
    CHECK(a.st.hover_fuel == a.p.hover_fuel_max);
}

static void test_flight() {
    std::printf("ability: flight maps input to full 3D velocity, then falls\n");
    Room room;

    Agent a(fx3{F(20), F(20), F(5)});
    a.st.grounded = false;
    a.p.flight_enabled = true;
    a.p.flight_fuel_max = Ff(5, 10);
    a.st.flight_fuel = Ff(5, 10);

    // Unlike hover, the vertical axis is directly controllable.
    a.run(room.map, C(act::FLIGHT, fx::zero(), fx::zero(), fx::one()), 20);
    CHECK(a.st.flying);
    CHECK(a.pos.z > F(6));

    a.run(room.map, C(act::FLIGHT, fx::zero(), fx::zero(), fx::one()), 30);
    CHECK(a.st.flight_fuel == fx::zero());
    CHECK(!a.st.flying);

    // Gravity resumes mid-air exactly as if flight had never engaged -- which
    // means the upward velocity flight left behind is decelerated first. It
    // rises briefly, then falls; there is no special "forced landing".
    const fx z = a.pos.z;
    // flight_speed was +12 when the fuel ran out, so gravity needs ~45 ticks
    // just to cancel it before anything descends.
    a.run(room.map, C(act::FLIGHT, fx::zero(), fx::zero(), fx::one()), 60);
    CHECK(a.vel.z < fx::zero());                  // gravity has taken over
    a.run(room.map, C(act::FLIGHT, fx::zero(), fx::zero(), fx::one()), 60);
    CHECK(a.pos.z < z);
}

static void test_shared_fuel() {
    std::printf("ability: hover/flight fuel pooling is a server choice\n");
    Room room;

    Agent a(fx3{F(20), F(20), F(20)});
    a.st.grounded = false;
    a.p.hover_enabled = a.p.flight_enabled = true;
    a.p.shared_fuel = true;
    a.p.hover_fuel_max = a.p.flight_fuel_max = F(2);
    a.st.hover_fuel = a.st.flight_fuel = F(2);

    a.run(room.map, C(act::FLIGHT), 30);
    CHECK(a.st.flight_fuel < F(2));
    CHECK(a.st.hover_fuel == a.st.flight_fuel);   // one pool

    Agent b(fx3{F(20), F(20), F(20)});
    b.st.grounded = false;
    b.p.hover_enabled = b.p.flight_enabled = true;
    b.p.shared_fuel = false;
    b.st.hover_fuel = b.p.hover_fuel_max;
    b.st.flight_fuel = b.p.flight_fuel_max;

    b.run(room.map, C(act::FLIGHT), 30);
    CHECK(b.st.flight_fuel < b.p.flight_fuel_max);
    CHECK(b.st.hover_fuel == b.p.hover_fuel_max);  // independent pools
}

// ------------------------------------------------------------------ climbing

static void test_climbing() {
    std::printf("ability: climbing suspends gravity and hands velocity back\n");
    Room room;

    // Start against the climbable west wall.
    Agent a(fx3{Ff(6, 10), F(20), F(10)});
    a.st.grounded = false;

    a.run(room.map, C(act::CLIMB, -fx::one(), fx::zero()), 5);
    CHECK(a.st.climbing);

    // Gravity is suspended: holding still on the wall must not sink.
    const fx z = a.pos.z;
    a.run(room.map, C(act::CLIMB), 30);
    CHECK(a.pos.z == z);

    // Vertical input climbs at climb_speed, not move_speed.
    a.run(room.map, C(act::CLIMB, fx::zero(), fx::zero(), fx::one()), 30);
    CHECK(a.pos.z > z + F(1));

    // Releasing hands control back to standard physics and gravity resumes.
    a.run(room.map, C(0), 5);
    CHECK(!a.st.climbing);
    const fx z2 = a.pos.z;
    a.run(room.map, C(0), 30);
    CHECK(a.pos.z < z2);
}

static void test_wall_jump() {
    std::printf("ability: jumping off a wall pushes away and upward\n");
    Room room;
    Agent a(fx3{Ff(6, 10), F(20), F(10)});
    a.st.grounded = false;
    a.run(room.map, C(act::CLIMB, -fx::one(), fx::zero()), 5);
    CHECK(a.st.climbing);

    a.step(room.map, C(act::CLIMB | act::JUMP, fx::zero(), fx::zero()));
    CHECK(!a.st.climbing);
    CHECK(a.vel.z > fx::zero());   // upward
    CHECK(a.vel.x > fx::zero());   // and away from the west wall
}

// ------------------------------------------------------------------- grapple

static void test_grapple_swing() {
    std::printf("ability: grapple constrains distance and damps the swing\n");
    Room room;

    Agent a(fx3{F(30), F(20), F(10)});
    a.st.grounded = false;
    a.p.grapple_enabled = true;
    a.p.grapple_range = F(20);

    // Aim at the east wall, which carries the GRAPPLE flag.
    MoveCommand cmd = C(act::GRAPPLE);
    cmd.aim = brad(0);   // +X
    a.step(room.map, cmd);
    CHECK(a.st.grappling);

    const fx rope = a.st.grapple_dist;
    CHECK(rope > fx::zero());

    // Swinging must not let the rope stretch.
    fx worst = fx::zero();
    for (int i = 0; i < 120; ++i) {
        a.step(room.map, cmd);
        const fx d = length(a.st.grapple_anchor - a.pos);
        worst = fx_max(worst, d - rope);
    }
    CHECK(worst < Ff(1, 100));

    // Damping bleeds energy off. Comparing instantaneous speeds would measure
    // the pendulum's PHASE rather than its energy -- it is fast at the bottom
    // and slow at the top -- so compare peak speed across two windows instead.
    fx peak_early = fx::zero();
    for (int i = 0; i < 120; ++i) { a.step(room.map, cmd); peak_early = fx_max(peak_early, length(a.vel)); }
    fx peak_late = fx::zero();
    for (int i = 0; i < 120; ++i) { a.step(room.map, cmd); peak_late = fx_max(peak_late, length(a.vel)); }
    CHECK(peak_late < peak_early);

    // Releasing hands the current velocity into standard physics. The rest of
    // the tick still runs, so the check is that momentum is CARRIED -- not that
    // velocity is frozen, which would mean the handoff had stopped the
    // simulation rather than fed it. Released from a fresh, fast swing so the
    // measurement is not dominated by one tick of air drag on a nearly-stopped
    // pendulum.
    {
        Agent b(fx3{F(30), F(20), F(10)});
        b.st.grounded = false;
        b.p.grapple_enabled = true;
        b.p.grapple_range = F(20);
        MoveCommand g = C(act::GRAPPLE);
        g.aim = brad(0);
        b.step(room.map, g);
        CHECK(b.st.grappling);
        for (int i = 0; i < 25; ++i) b.step(room.map, g);   // build up swing speed

        const fx speed_before = length(fx2{b.vel.x, b.vel.y});
        CHECK(speed_before > F(1));
        b.step(room.map, C(0));
        CHECK(!b.st.grappling);
        const fx speed_after = length(fx2{b.vel.x, b.vel.y});
        CHECK(speed_after > speed_before / 2);              // not reset to a standstill
        CHECK(fx_abs(speed_after - speed_before) < F(1));   // and continuous
    }
}

static void test_grapple_occlusion() {
    std::printf("ability: a grapple cannot be thrown through a wall\n");
    // Two rooms with a solid divider; the far room's wall is grapple-flagged.
    fx2 pts[8] = {{F(0), F(0)}, {F(10), F(0)}, {F(10), F(10)}, {F(0), F(10)},
                  {F(10), F(0)}, {F(20), F(0)}, {F(20), F(10)}, {F(10), F(10)}};
    Sector secs[2] = {{0, 4, 0, 4, F(0), F(20), 0}, {4, 4, 4, 4, F(0), F(20), 0}};
    Side sides[8] = {
        {0, 1, NO_SECTOR, 0}, {1, 2, 1, side_flag::BLOCKING}, {2, 3, NO_SECTOR, 0}, {3, 0, NO_SECTOR, 0},
        {4, 5, NO_SECTOR, 0}, {5, 6, NO_SECTOR, side_flag::GRAPPLE}, {6, 7, NO_SECTOR, 0}, {7, 4, 0, side_flag::BLOCKING}
    };
    MapData map{pts, 8, secs, 2, sides, 8};

    Agent a(fx3{F(5), F(5), F(5)});
    a.st.grounded = false;
    a.p.grapple_enabled = true;
    a.p.grapple_range = F(30);

    MoveCommand cmd = C(act::GRAPPLE);
    cmd.aim = brad(0);
    a.step(map, cmd);
    // The anchor is in range and directly ahead, but a solid wall is between.
    CHECK(!a.st.grappling);
}

// ------------------------------------------------------- mutual exclusivity

static void test_controller_mode_gate() {
    std::printf("ability: every ability is suppressed off standard physics\n");
    Room room;

    Agent a(fx3{F(20), F(20), F(10)});
    a.p.hover_enabled = a.p.flight_enabled = a.p.grapple_enabled = true;
    a.st.mode = ControllerMode::Rail;

    const fx3 pos0 = a.pos;
    const fx3 vel0 = a.vel;
    a.run(room.map, C(act::JUMP | act::DASH | act::HOVER | act::FLIGHT | act::GRAPPLE |
                      act::CLIMB, fx::one(), fx::one(), fx::one()), 60);

    // A rail controller owns motion entirely; standard physics and every
    // ability inside it must be inert. One gate covers all of them, which is
    // why there is no per-ability exclusion logic to forget.
    CHECK(a.pos == pos0);
    CHECK(a.vel == vel0);
    CHECK(!a.st.grappling);
    CHECK(!a.st.climbing);
    CHECK(!a.st.hovering);
    CHECK(!a.st.flying);
    CHECK(a.st.jumps_used == 0);
    CHECK(a.st.dashes_used == 0);

    // Handing back restores it.
    handoff_to_standard(a.st, a.vel, fx3{F(3), fx::zero(), fx::zero()});
    CHECK(a.st.mode == ControllerMode::Standard);
    CHECK(a.vel.x == F(3));
    a.run(room.map, C(0), 5);
    CHECK(a.pos != pos0);
}

// --------------------------------------------------------------- determinism

static void test_determinism() {
    std::printf("ability: the controller is a pure function of its inputs\n");
    Room room;


    for (int trial = 0; trial < 300; ++trial) {
        Agent a(fx3{F(20), F(20), F(3)});
        Agent b(fx3{F(20), F(20), F(3)});
        a.p.hover_enabled = b.p.hover_enabled = true;
        a.p.flight_enabled = b.p.flight_enabled = true;
        a.p.grapple_enabled = b.p.grapple_enabled = true;

        Rng seq(static_cast<u64>(trial));
        for (int i = 0; i < 120; ++i) {
            MoveCommand c;
            c.buttons = static_cast<u16>(seq.next_u32() & 0x3F);
            c.move = fx2{seq.next_fx(-fx::one(), fx::one()), seq.next_fx(-fx::one(), fx::one())};
            c.up = seq.next_fx(-fx::one(), fx::one());
            c.aim = brad(seq.next_u32());
            a.step(room.map, c);
            b.step(room.map, c);
        }
        CHECK(a.pos == b.pos);
        CHECK(a.vel == b.vel);
        CHECK(a.st.jumps_used == b.st.jumps_used);
        CHECK(a.st.hover_fuel == b.st.hover_fuel);
        CHECK(a.st.grappling == b.st.grappling);

        // And the agent stays inside the world no matter what it was told to do.
        CHECK(point_in_sector(room.map, 0, fx2{a.pos.x, a.pos.y}));
    }
}

int main() {
    std::printf("== core/physics ability suite ==\n");
    test_multi_jump();
    test_dash();
    test_hover();
    test_flight();
    test_shared_fuel();
    test_climbing();
    test_wall_jump();
    test_grapple_swing();
    test_grapple_occlusion();
    test_controller_mode_gate();
    test_determinism();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
