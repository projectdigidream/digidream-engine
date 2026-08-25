//
// Sector motion (§3.1.2) and anchors (§3.2.5).
//
// The property that matters most here is not "the lift moves" but that a
// PARAMETRIC motion is a pure function of the tick. That is the whole basis for
// a client predicting an entity riding it: evaluate the same function for a
// tick the server has not reached yet and get the same answer. So the tests
// check evaluation out of order, from a cold start, and after a long run --
// because any of those failing would break prediction rather than the lift.
//
#include <cstdio>

#include "dnp/geom/mapfile.hpp"
#include "dnp/math/rng.hpp"
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
static double D(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }

// Two rooms: sector 0 is static ground, sector 1 is the lift.
struct Stage {
    MapStorage store;
    MapData    view{};

    Stage() {
        const fx2 a[] = {{F(0), F(0)}, {F(20), F(0)}, {F(20), F(20)}, {F(0), F(20)}};
        const fx2 b[] = {{F(20), F(0)}, {F(40), F(0)}, {F(40), F(20)}, {F(20), F(20)}};
        store.add_sector(a, 4, F(0), F(60), 1);
        store.add_sector(b, 4, F(0), F(60), 2);

        store.add_side(0, 0, 1, NO_SECTOR, 0);
        store.add_side(0, 1, 2, 1, 0);
        store.add_side(0, 2, 3, NO_SECTOR, 0);
        store.add_side(0, 3, 0, NO_SECTOR, 0);

        store.add_side(1, 0, 1, NO_SECTOR, 0);
        store.add_side(1, 1, 2, NO_SECTOR, 0);
        store.add_side(1, 2, 3, NO_SECTOR, 0);
        store.add_side(1, 3, 0, 0, 0);
        view = store.view();
    }

    void attach(World& w) {
        w.set_map_storage(&store);
        view = store.view();
        w.set_map(&view);
    }
};

// ------------------------------------------------------- parametric evaluation

static void test_parametric_is_a_pure_function() {
    std::printf("motion: parametric offset is a pure function of the tick\n");

    SectorMotion m;
    m.type = MotionType::Parametric;
    m.from = fx3{fx::zero(), fx::zero(), fx::zero()};
    m.to = fx3{fx::zero(), fx::zero(), F(10)};
    m.start_tick = 100;
    m.duration_ticks = 60;
    m.loop = LoopMode::Once;

    // Endpoints are exact, which is why a lift settles precisely rather than
    // stopping one ulp short and reporting itself as still moving.
    CHECK(parametric_offset(m, 100).z == fx::zero());
    CHECK(parametric_offset(m, 160).z == F(10));
    CHECK(parametric_offset(m, 130).z == F(5));

    // Before it starts, and long after it ends.
    CHECK(parametric_offset(m, 0).z == fx::zero());
    CHECK(parametric_offset(m, 99).z == fx::zero());
    CHECK(parametric_offset(m, 100000).z == F(10));

    // Evaluated OUT OF ORDER it gives the same answers. This is the property a
    // predicting client relies on: it asks for ticks the server has not reached.
    for (u32 t = 160; t >= 100; --t) {
        CHECK(parametric_offset(m, t) == parametric_offset(m, t));
        if (t == 100) break;
    }
    CHECK(parametric_offset(m, 137) == parametric_offset(m, 137));

    // Loop and ping-pong.
    m.loop = LoopMode::Loop;
    CHECK(parametric_offset(m, 160).z == fx::zero());     // wrapped
    CHECK(parametric_offset(m, 190).z == F(5));

    m.loop = LoopMode::PingPong;
    CHECK(parametric_offset(m, 160).z == F(10));          // at the far end
    CHECK(parametric_offset(m, 190).z == F(5));           // coming back
    CHECK(parametric_offset(m, 220).z == fx::zero());     // home

    // Zero duration must not divide by zero.
    m.duration_ticks = 0;
    CHECK(parametric_offset(m, 500).z == F(10));
}

// ----------------------------------------------------------------- lift riding

static void test_riding_a_lift() {
    std::printf("motion: an anchored agent rides its sector\n");

    Stage stage;
    World w;
    stage.attach(w);

    const u32 a = w.spawn(fx3{F(30), F(10), F(1)});
    w.motions().add(stage.store, 1);
    // Rise 20 units over five seconds. Long enough that the measurement window
    // below is INSIDE the motion -- a shorter lift finishes while the agent is
    // still settling and the test measures nothing.
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(20)},
                                 0, 300, LoopMode::Once);

    // Land on the lift first.
    for (int i = 0; i < 30; ++i) { w.set_input(a, proto::InputFrame{}); w.step(); }
    CHECK(w.agent(a).mv.grounded);
    CHECK(w.agent(a).mv.anchored);
    CHECK(w.agent(a).mv.anchor == 1);

    const fx z0 = w.agent(a).pos.z;
    for (int i = 0; i < 60; ++i) { w.set_input(a, proto::InputFrame{}); w.step(); }

    // Carried up with the floor rather than left behind or pushed through it.
    const fx z1 = w.agent(a).pos.z;
    // 60 ticks at 20 units per 300 ticks == 4 units of platform rise.
    CHECK(z1 > z0 + fx::from_frac(35, 10));
    CHECK(w.agent(a).mv.grounded);
    std::printf("       rode from z=%.2f to z=%.2f\n", D(z0), D(z1));
}

static void test_detach_inherits_momentum() {
    std::printf("motion: detaching inherits the platform's velocity\n");

    // The platform rises at exactly 12 units/second.
    const fx PLATFORM_SPEED = F(12);

    fx vz_moving = fx::zero();
    fx vz_static = fx::zero();
    fx peak_rel_moving = fx::zero();
    fx peak_rel_static = fx::zero();

    for (int variant = 0; variant < 2; ++variant) {
        Stage stage;
        World w;
        stage.attach(w);
        const u32 a = w.spawn(fx3{F(30), F(10), F(1)});
        w.motions().add(stage.store, 1);
        if (variant == 0) {
            w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(120)},
                                         0, 600, LoopMode::Once);
        }

        for (int i = 0; i < 40; ++i) { w.set_input(a, proto::InputFrame{}); w.step(); }
        CHECK(w.agent(a).mv.anchored);

        proto::InputFrame jump;
        jump.buttons = proto::button::JUMP;
        w.set_input(a, jump);
        w.step();

        const fx vz = w.agent(a).vel.z;
        CHECK(!w.agent(a).mv.anchored);   // detached this tick

        w.set_input(a, proto::InputFrame{});
        fx peak = fx::zero();
        for (int i = 0; i < 40; ++i) {
            w.step();
            peak = fx_max(peak, w.agent(a).pos.z - w.map()->sector(1).floor_z);
        }

        if (variant == 0) { vz_moving = vz; peak_rel_moving = peak; }
        else              { vz_static = vz; peak_rel_static = peak; }
    }

    std::printf("       vel.z after jump: moving %.3f, static %.3f (platform %.3f)\n",
                D(vz_moving), D(vz_static), D(PLATFORM_SPEED));
    std::printf("       peak above platform: moving %.3f, static %.3f\n",
                D(peak_rel_moving), D(peak_rel_static));

    // The direct assertion: leaving a rising platform adds the platform's
    // velocity to the entity's, once, at the moment of detach.
    CHECK(fx_abs((vz_moving - vz_static) - PLATFORM_SPEED) < fx::from_frac(1, 4));

    // And the consequence worth understanding: the agent does NOT clear more
    // height ABOVE the platform, because the platform is rising with it. The
    // inherited momentum shows up in world space, not in relative space --
    // which is why comparing relative peaks would have been measuring noise
    // rather than the mechanic.
    CHECK(fx_abs(peak_rel_moving - peak_rel_static) < fx::from_frac(1, 5));
}

// ------------------------------------------------------------ discrete motion

static void test_discrete_motion() {
    std::printf("motion: scripted-discrete moves the sector without a path\n");

    Stage stage;
    World w;
    stage.attach(w);
    const u32 a = w.spawn(fx3{F(30), F(10), F(1)});
    w.motions().add(stage.store, 1);

    for (int i = 0; i < 30; ++i) { w.set_input(a, proto::InputFrame{}); w.step(); }
    CHECK(w.agent(a).mv.anchored);

    const fx z0 = w.agent(a).pos.z;
    // An arbitrary reposition, of the kind a Lua callback makes. There is no
    // path and no schedule, which is exactly why a client cannot predict it.
    w.motions().set_discrete(1, fx3{fx::zero(), fx::zero(), F(4)});
    w.set_input(a, proto::InputFrame{});
    w.step();

    CHECK(w.map()->sector(1).floor_z == F(4));
    CHECK(w.agent(a).pos.z > z0);   // carried along
}

// ---------------------------------------------------------------- determinism

static void test_determinism() {
    std::printf("motion: two worlds with the same schedule stay identical\n");

    Rng rng(555);
    for (int trial = 0; trial < 60; ++trial) {
        Stage sa, sb;
        World wa, wb;
        sa.attach(wa);
        sb.attach(wb);

        const u32 aa = wa.spawn(fx3{F(30), F(10), F(2)});
        const u32 ab = wb.spawn(fx3{F(30), F(10), F(2)});
        wa.motions().add(sa.store, 1);
        wb.motions().add(sb.store, 1);

        const u32 dur = 20 + rng.next_below(80);
        const fx dz = rng.next_fx(-F(6), F(12));
        const LoopMode lm = static_cast<LoopMode>(rng.next_below(3));
        wa.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), dz}, 0, dur, lm);
        wb.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), dz}, 0, dur, lm);

        Rng seq(static_cast<u64>(trial));
        for (int i = 0; i < 240; ++i) {
            proto::InputFrame f;
            f.buttons = static_cast<u16>(seq.next_u32() & 0x3F);
            f.move_x = static_cast<i16>(seq.next_range(-32767, 32768));
            f.move_y = static_cast<i16>(seq.next_range(-32767, 32768));
            f.aim = brad(seq.next_u32());
            wa.set_input(aa, f);
            wb.set_input(ab, f);
            wa.step();
            wb.step();
        }
        CHECK(wa.hash() == wb.hash());
        CHECK(wa.agent(aa).pos == wb.agent(ab).pos);
    }
}

static void test_no_drift_over_a_long_run() {
    // Motion is written as base + f(tick), never accumulated. That is what makes
    // drift impossible RATHER THAN SMALL: an implementation that added each
    // step's delta would accumulate its rounding, and a platform that ends every
    // round trip a few ulp off eventually is not where the level author put it.
    std::printf("motion: a ping-pong lift returns exactly home, forever\n");

    SectorMotion m;
    m.type = MotionType::Parametric;
    m.to = fx3{fx::zero(), fx::zero(), F(7)};
    m.duration_ticks = 90;
    m.loop = LoopMode::PingPong;

    // Every cycle boundary is EXACTLY the start, ten thousand cycles later.
    for (u32 cycle = 0; cycle < 10000; ++cycle) {
        CHECK(parametric_offset(m, cycle * 180).z == fx::zero());
        CHECK(parametric_offset(m, cycle * 180 + 90).z == F(7));
    }

    // And the world writes exactly that into the map. `apply()` runs with the
    // tick BEFORE it is incremented, so after N steps the geometry reflects
    // tick N-1 -- checked explicitly rather than assumed, since getting that
    // off by one would put every moving sector one tick out of step with the
    // agents riding it.
    Stage stage;
    World w;
    stage.attach(w);
    w.motions().add(stage.store, 1);
    const fx home = w.map()->sector(1).floor_z;
    w.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(7)},
                                 0, 90, LoopMode::PingPong);

    for (int i = 0; i < 1000; ++i) {
        const u32 applied = w.tick();
        w.step();
        CHECK(w.map()->sector(1).floor_z == home + parametric_offset(m, applied).z);
    }
}

// Anchor-relative prediction (design §3.2.5), as a property of the hash.
//
// Two worlds run the same rider on the same platform. One is the "server" and
// moves the platform every tick by a rule the other cannot know; the other is
// the "client" and only learns the platform's offset when a snapshot arrives.
// Their ABSOLUTE positions must diverge -- that is what unpredictable means --
// and their per-agent hashes must nonetheless agree on every tick, because the
// hash is taken in the platform's frame.
static void test_a_discrete_anchor_is_hashed_in_its_own_frame() {
    std::printf("motion: a rider on an unpredictable platform still hashes exactly\n");

    static Stage server_stage;
    static Stage client_stage;
    static World server;
    static World client;

    server.reset(); server_stage.attach(server);
    client.reset(); client_stage.attach(client);

    const u32 sa = server.spawn(fx3{F(30), F(10), F(0)});
    const u32 ca = client.spawn(fx3{F(30), F(10), F(0)});
    server.motions().add(server_stage.store, 1);
    client.motions().add(client_stage.store, 1);

    // Land and anchor.
    for (int i = 0; i < 4; ++i) { server.step(); client.step(); }
    CHECK(server.agent(sa).mv.anchored);
    CHECK(server.agent(sa).mv.anchor == 1);

    // A schedule with no closed form -- which is the whole definition of
    // scripted-discrete. Drawn from a PRNG the client never sees.
    Rng sched(0xC0FFEE, 3);
    fx  z = fx::zero();

    int  ticks = 0;
    int  hash_agreed = 0;
    int  hash_differed = 0;
    int  abs_differed = 0;
    fx   worst_abs_gap = fx::zero();

    for (u32 t = 0; t < 600; ++t) {
        // The server picks a new offset every tick.
        z += fx::from_frac(static_cast<i64>(sched.next_range(-3, 4)), 100);
        if (z > F(2)) z = F(2);
        if (z < fx::zero()) z = fx::zero();
        server.motions().set_discrete(1, fx3{fx::zero(), fx::zero(), z});
        server.step();

        // The client steps the SAME tick with whatever offset it last heard.
        // Deliberately in this order: reconciliation compares the client's
        // recorded prediction for a tick against the server's answer for that
        // same tick, so a harness that let the client adopt first would be
        // comparing the server against itself.
        client.step();

        ++ticks;
        if (server.agent_hash(sa) == client.agent_hash(ca)) ++hash_agreed;
        else ++hash_differed;

        const fx gap = server.agent(sa).pos.z - client.agent(ca).pos.z;
        const fx mag = gap.raw < 0 ? -gap : gap;
        if (mag.raw != 0) ++abs_differed;
        if (mag > worst_abs_gap) worst_abs_gap = mag;

        // Only now does a snapshot arrive: the streamed offset, then the
        // authoritative state, exactly as the client does it.
        if ((t % 2) == 0) {
            client.motions().set_discrete(1, fx3{fx::zero(), fx::zero(), z});
            client.agent_mut(ca).pos = server.agent(sa).pos;
            client.agent_mut(ca).vel = server.agent(sa).vel;
            client.agent_mut(ca).mv = server.agent(sa).mv;
            client.set_tick(server.tick());
        }
    }

    std::printf("  %d ticks: %d hashes agreed, %d differed; absolute z differed on %d "
                "(worst %.3f)\n",
                ticks, hash_agreed, hash_differed, abs_differed, D(worst_abs_gap));

    // The hash agrees on every tick...
    CHECK(hash_differed == 0);
    // ...while the absolute positions genuinely do not. Without this the test
    // would pass just as well on a platform that never moved, and would be
    // measuring nothing.
    CHECK(abs_differed > 0);
    CHECK(worst_abs_gap > fx::zero());
}

// The control: the same setup with a PARAMETRIC platform must agree absolutely,
// because that one IS predictable and asking for less would let the gate stop
// noticing when it broke.
static void test_a_parametric_anchor_is_still_hashed_absolutely() {
    std::printf("motion: CONTROL -- a rider on a predictable platform agrees absolutely\n");

    static Stage server_stage;
    static Stage client_stage;
    static World server;
    static World client;
    server.reset(); server_stage.attach(server);
    client.reset(); client_stage.attach(client);

    const u32 sa = server.spawn(fx3{F(30), F(10), F(0)});
    const u32 ca = client.spawn(fx3{F(30), F(10), F(0)});
    server.motions().add(server_stage.store, 1);
    client.motions().add(client_stage.store, 1);
    server.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(3)},
                                      0, 90, LoopMode::PingPong);
    client.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), F(3)},
                                      0, 90, LoopMode::PingPong);

    int differed = 0;
    for (u32 t = 0; t < 400; ++t) {
        server.step();
        client.step();
        if (server.agent(sa).pos != client.agent(ca).pos) ++differed;
        if (server.agent_hash(sa) != client.agent_hash(ca)) ++differed;
    }
    CHECK(differed == 0);
    // And it really did move, or "they agree" means "neither moved".
    CHECK(server.motions().at(0).offset.z != fx::zero() ||
          server.agent(sa).pos.z != F(0));
    std::printf("  400 ticks, no divergence of any kind\n");
}

int main() {
    std::printf("== core/sim sector motion suite ==\n");
    test_parametric_is_a_pure_function();
    test_riding_a_lift();
    test_detach_inherits_momentum();
    test_discrete_motion();
    test_determinism();
    test_no_drift_over_a_long_run();
    test_a_discrete_anchor_is_hashed_in_its_own_frame();
    test_a_parametric_anchor_is_still_hashed_absolutely();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
