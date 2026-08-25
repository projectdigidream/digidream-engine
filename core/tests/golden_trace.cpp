//
// Golden-vector trace generator.
//
// Runs a fixed synthetic workload at the engine's fixed timestep and emits one
// world-state hash per tick. Every build target (native arm64, native x86-64,
// wasm) must produce a byte-identical hash stream. That comparison is the CI
// gate the determinism contract is enforced with.
//
// The workload is deliberately chosen to exercise the operations most likely to
// diverge between targets rather than to be realistic physics: multiplies and
// divides near the saturation boundary, sqrt, normalize, CORDIC sin/cos/atan2,
// and the PRNG. When real controllers land in Phase 2 this file gains a second
// trace driven by recorded input; the harness around it does not change.
//
// Usage:
//   golden_trace [ticks]            emit "<tick> <hash>" per line
//   golden_trace [ticks] --summary  emit only the final hash
//
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dnp/math/fixed.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"
#include "dnp/sim/state_hash.hpp"
#include "dnp/geom/mapstore.hpp"
#include "dnp/geom/rail.hpp"
#include "dnp/nav/navgraph.hpp"
#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"

using namespace dnp;

namespace {

constexpr int BODY_COUNT = 64;

// Agents driven in the gameplay half of the trace. Eight roam the open room;
// three more stand where geometry closes on them -- under a descending ceiling,
// beside a sweeping footprint, and in the dead end a piston runs into -- so the
// determinism gate covers crushing and §3.1.3 as well. A twelfth rides a
// SCRIPTED-DISCRETE platform, which is the only way the anchor-relative hashing
// of §3.2.5 is ever executed rather than merely compiled. Without them those paths
// are compiled by every target and executed by none, and `--coverage` is what
// keeps that honest rather than assumed.
constexpr u32 SIM_AGENTS = 12;

// The fixed simulation timestep (design 7.1.1). 60 Hz, as an exact rational --
// never a measured frame time, and never a decimal literal that would round
// differently if the rate were ever edited.
const fx DT = fx::from_frac(1, 60);

const fx GRAVITY   = fx::from_frac(-1600, 100);   // -16.00 units/s^2
const fx BOUNDS    = fx::from_int(48);
const fx RESTITUTION = fx::from_frac(60, 100);

constexpr fx F(i64 n) { return fx::from_int(n); }

struct Body {
    fx3  pos;
    fx3  vel;
    brad heading;
    brad spin;
    Rng  rng;
};

struct World {
    Body  bodies[BODY_COUNT];
    Rng   global;
    u32   tick;

    // The actual gameplay simulation, driven alongside the synthetic workload.
    //
    // The bodies above stress the arithmetic most likely to diverge; this covers
    // the code the game actually runs. Both belong under the gate: the synthetic
    // half would keep passing while a real controller quietly desynced, and the
    // real half alone would not exercise saturation or CORDIC hard enough to
    // catch a math regression.
    dnp::sim::World      sim;
    dnp::geom::MapStorage sim_map;
    dnp::geom::MapData    sim_view{};
    Rng                   sim_rng;

    // The nav graph, queried periodically and folded into the hash.
    //
    // Nothing in the simulation routes yet -- enemies arrive in Phase 4 -- but
    // A* over the sector graph is integer division, sqrt and a heap whose order
    // decides the answer, which is exactly the shape of code that resolves one
    // way on arm64 and another on wasm. Getting it under the gate before
    // anything depends on it is cheaper than bisecting it afterwards.
    u64                 rail_digest;
    Rng                 drift_rng;
    fx                  drift_z;

    dnp::nav::NavGraph  nav;
    dnp::nav::NavQuery  nav_query;
    u32                 nav_path[dnp::nav::MAX_NAV_NODES];
    u64                 nav_digest;
    u32                 nav_queries;
    u32                 nav_routes_found;

    // Not hashed and not part of the simulation -- see --coverage in main().
    u32 vertical_crush_ticks;
    u32 horizontal_pin_ticks;
    u32 discrete_anchor_ticks;
};

void world_init(World& w) {
    w.tick = 0;
    w.global = Rng(0xD00Du, 1);
    for (int i = 0; i < BODY_COUNT; ++i) {
        Body& b = w.bodies[i];
        b.rng = Rng(static_cast<u64>(i) * 7919u + 13u, static_cast<u64>(i));
        b.pos = fx3{b.rng.next_fx(-BOUNDS, BOUNDS),
                    b.rng.next_fx(-BOUNDS, BOUNDS),
                    b.rng.next_fx(fx::zero(), BOUNDS)};
        b.vel = fx3{b.rng.next_fx(-fx::from_int(8), fx::from_int(8)),
                    b.rng.next_fx(-fx::from_int(8), fx::from_int(8)),
                    fx::zero()};
        b.heading = brad(b.rng.next_u32());
        b.spin    = brad(b.rng.next_u32() >> 12);
    }

    w.sim.reset();
    // Real geometry in MUTABLE storage, so sector motion and anchor riding are
    // under the determinism gate too -- not just the collision rules and the
    // abilities, but the moving platforms agents stand on.
    dnp::sim::build_test_map(w.sim_map);

    // A fourth room, north of sector 0, whose CEILING comes down. The test map
    // proper has no crusher and should not grow one: it is the shared fixture
    // several other tests compare against byte for byte. This one exists only
    // for the gate, so it is built here.
    {
        const fx2 room[] = {{F(0), F(40)}, {F(40), F(40)}, {F(40), F(60)}, {F(0), F(60)}};
        const u32 s3 = w.sim_map.add_sector(room, 4, fx::zero(), F(40), 4);
        w.sim_map.add_side(s3, 0, 1, 0, 0);            // south: back into sector 0
        w.sim_map.add_side(s3, 1, 2, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s3, 2, 3, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s3, 3, 0, dnp::geom::NO_SECTOR, 0);
        // And the other half of that shared boundary: sector 0's north wall was
        // solid, and a one-sided portal would let an agent walk out of the
        // crusher but never into it.
        w.sim_map.side_mut(2).neighbor = static_cast<i32>(s3);

        // A piston east of that room, which runs the whole length of it. Its
        // floor is above step height, so a body cannot be where it is -- that,
        // and not the fact that it moves, is what makes it solid to the sweep.
        // The room's west wall is the dead end that turns a shove into a pin.
        // A room north of the crusher, which DRIFTS on a schedule with no closed
        // form. A rider on it is hashed in its frame (design §3.2.5), and that
        // branch is unreachable without a scripted-discrete mover to stand on.
        const fx2 drift[] = {{F(0), F(60)}, {F(40), F(60)}, {F(40), F(80)}, {F(0), F(80)}};
        const u32 s5 = w.sim_map.add_sector(drift, 4, fx::zero(), F(40), 6);
        w.sim_map.add_side(s5, 0, 1, static_cast<i32>(s3), 0);
        w.sim_map.add_side(s5, 1, 2, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s5, 2, 3, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s5, 3, 0, dnp::geom::NO_SECTOR, 0);
        w.sim_map.side_mut(w.sim_map.view().sector(s3).first_side + 2).neighbor =
            static_cast<i32>(s5);

        const fx2 piston[] = {{F(40), F(40)}, {F(50), F(40)}, {F(50), F(60)}, {F(40), F(60)}};
        const u32 s4 = w.sim_map.add_sector(piston, 4, F(4), F(40), 5);
        w.sim_map.add_side(s4, 0, 1, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s4, 1, 2, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s4, 2, 3, dnp::geom::NO_SECTOR, 0);
        w.sim_map.add_side(s4, 3, 0, static_cast<i32>(s3), 0);
        w.sim_map.side_mut(w.sim_map.view().sector(s3).first_side + 1).neighbor =
            static_cast<i32>(s4);

        // A rail bolted to the sector that slides horizontally, so §3.4's
        // waypoint pass and the polyline queries run on every target rather
        // than only being compiled by them. rail_point_at() is a divide and a
        // lerp per segment, which is the shape of arithmetic worth watching.
        const u32 rail = w.sim_map.add_rail(1, dnp::geom::rail_flag::LOOP);
        w.sim_map.add_waypoint(rail, fx3{F(62), F(4), F(6)}, 2);
        w.sim_map.add_waypoint(rail, fx3{F(78), F(4), F(6)}, 2);
        w.sim_map.add_waypoint(rail, fx3{F(78), F(36), F(6)}, 2);
        w.sim_map.add_waypoint(rail, fx3{F(62), F(36), F(6)}, 2);
    }

    w.sim_view = w.sim_map.view();
    w.sim.set_map(&w.sim_view);
    w.sim.set_map_storage(&w.sim_map);

    // Sector 1 ping-pongs vertically, and sector 2 slides horizontally, so both
    // the lift case and the sweeping-footprint case are exercised. Sector 2
    // slides WEST, into the room agents can reach: a footprint that retreats
    // from everyone sweeps through nothing.
    w.sim.motions().add(w.sim_map, 1);
    w.sim.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), fx::from_int(3)},
                                     0, 137, dnp::sim::LoopMode::PingPong);
    w.sim.motions().add(w.sim_map, 2);
    w.sim.motions().start_parametric(2, fx3::zero(), fx3{fx::from_frac(-3, 2), fx::zero(), fx::zero()},
                                     0, 211, dnp::sim::LoopMode::PingPong);
    // The crusher: ceiling only, so the gap actually closes.
    w.sim.motions().add(w.sim_map, 3);
    w.sim.motions().start_parametric(3, fx3::zero(), fx3{fx::zero(), fx::zero(), fx::from_int(-39)},
                                     0, 173, dnp::sim::LoopMode::PingPong,
                                     dnp::sim::MotionTarget::Ceiling);
    // The drifting room. Registered here; its offset is chosen per tick in
    // world_step from the trace's own PRNG, which is deterministic across
    // targets while still being something no client could derive.
    w.sim.motions().add(w.sim_map, 5);
    w.sim.motions().set_discrete(5, fx3::zero());

    // The piston: all the way to the far wall, so the shove becomes a pin.
    w.sim.motions().add(w.sim_map, 4);
    w.sim.motions().start_parametric(4, fx3::zero(), fx3{fx::from_int(-40), fx::zero(), fx::zero()},
                                     0, 293, dnp::sim::LoopMode::PingPong);

    w.sim_rng = Rng(0x5EED, 2);
    for (int i = 0; i < 8; ++i) {
        w.sim.spawn(fx3{fx::from_int(5 + i * 4), fx::from_int(5 + i * 3), fx::from_int(3)});
    }
    // Under the crusher, beside the sweeping footprint, and in the piston's path.
    w.sim.spawn(fx3{fx::from_int(20), fx::from_int(50), fx::from_int(1)});
    w.sim.spawn(fx3{fx::from_int(59), fx::from_int(20), fx::from_int(1)});
    w.sim.spawn(fx3{fx::from_int(35), fx::from_int(50), fx::from_int(1)});
    w.sim.spawn(fx3{fx::from_int(20), fx::from_int(70), fx::from_int(1)});   // on the drifter

    // Turn every ability on for at least some agents, so the trace exercises
    // hover, flight and grapple rather than only walking and jumping.
    for (u32 i = 0; i < SIM_AGENTS; ++i) {
        auto& pr = w.sim.params_mut(i);
        pr.hover_enabled = (i % 2) == 0;
        pr.flight_enabled = (i % 3) == 0;
        pr.grapple_enabled = (i % 2) == 1;
        pr.max_jumps = static_cast<u8>(1 + (i % 3));
    }
    w.rail_digest = 0;
    w.drift_rng = Rng(0xD21F7u, 5);
    w.drift_z = fx::zero();
    w.nav.build(w.sim_view);
    w.nav_digest = 0;
    w.nav_queries = 0;
    w.nav_routes_found = 0;

    w.vertical_crush_ticks = 0;
    w.horizontal_pin_ticks = 0;
    w.discrete_anchor_ticks = 0;
}

// One fixed-timestep step. Not the real controller -- that arrives in Phase 2 --
// but it walks the same arithmetic paths the real one will.
void world_step(World& w) {
    for (int i = 0; i < BODY_COUNT; ++i) {
        Body& b = w.bodies[i];

        b.heading = b.heading + b.spin;

        // Steering thrust: exercises CORDIC on a continuously varying angle.
        const fx2 dir = fx_dir(b.heading);
        const fx  thrust = fx::from_frac(35, 10);
        b.vel.x += dir.x * thrust * DT;
        b.vel.y += dir.y * thrust * DT;
        b.vel.z += GRAVITY * DT;

        // Quadratic-ish drag, via sqrt and normalize.
        const fx speed = length(b.vel);
        if (speed > fx::from_frac(1, 100)) {
            const fx3 n    = normalize(b.vel);
            const fx  drag = speed * speed * fx::from_frac(2, 1000);
            b.vel -= n * drag * DT;
        }

        b.pos += b.vel * DT;

        // Box walls, resolved with the same project-out rule wall sliding uses.
        if (b.pos.z < fx::zero()) {
            b.pos.z = fx::zero();
            if (b.vel.z < fx::zero()) b.vel.z = -b.vel.z * RESTITUTION;
            // A grounded body draws from its own stream: any divergence in the
            // draw COUNT (not just the values) shows up immediately in the hash.
            b.heading = b.heading + brad(b.rng.next_u32() >> 16);
        }
        if (b.pos.x >  BOUNDS) { b.pos.x =  BOUNDS; b.vel.x = -b.vel.x * RESTITUTION; }
        if (b.pos.x < -BOUNDS) { b.pos.x = -BOUNDS; b.vel.x = -b.vel.x * RESTITUTION; }
        if (b.pos.y >  BOUNDS) { b.pos.y =  BOUNDS; b.vel.y = -b.vel.y * RESTITUTION; }
        if (b.pos.y < -BOUNDS) { b.pos.y = -BOUNDS; b.vel.y = -b.vel.y * RESTITUTION; }

        // Re-derive heading from velocity now and then: exercises atan2 on
        // arbitrary magnitudes, including very short vectors.
        if ((w.tick % 37) == static_cast<u32>(i % 37)) {
            b.heading = fx_angle_of(fx2{b.vel.x, b.vel.y});
        }
    }

    // Pairwise soft separation, standing in for the agent-vs-agent soft push of
    // design 7.1. Quadratic in BODY_COUNT, which is fine for a trace.
    for (int i = 0; i < BODY_COUNT; ++i) {
        for (int j = i + 1; j < BODY_COUNT; ++j) {
            Body& a = w.bodies[i];
            Body& b = w.bodies[j];
            const fx3 d   = b.pos - a.pos;
            const fx  len = length(d);
            const fx  minr = fx::from_int(2);
            if (len > fx::zero() && len < minr) {
                const fx3 n    = normalize(d);
                const fx  push = (minr - len) * fx::from_frac(1, 2);
                a.pos -= n * push;
                b.pos += n * push;
            }
        }
    }

    // Drive the gameplay simulation with deterministic pseudo-input. Buttons are
    // drawn per agent so jump edge-detection, the jump counter and the grounded
    // transition all get exercised rather than sitting at their defaults.
    for (u32 i = 0; i < SIM_AGENTS; ++i) {
        proto::InputFrame f;
        f.move_x  = static_cast<i16>(w.sim_rng.next_range(-32767, 32768));
        f.move_y  = static_cast<i16>(w.sim_rng.next_range(-32767, 32768));
        // Draw the whole action set, not just jump: edge detection, fuel
        // drain, dash cooldown and grapple attach all need to be walked.
        const u32 bits = w.sim_rng.next_u32();
        u16 buttons = 0;
        if ((bits & 7) == 0) buttons |= proto::button::JUMP;
        if ((bits & 0x38) == 0) buttons |= proto::button::DASH;
        if ((bits & 0x1C0) == 0) buttons |= proto::button::CLIMB;
        if ((bits & 0xE00) == 0) buttons |= proto::button::HOVER;
        if ((bits & 0x7000) == 0) buttons |= proto::button::FLIGHT;
        if ((bits & 0x38000) == 0) buttons |= proto::button::GRAPPLE;
        f.buttons = buttons;
        f.aim     = brad(w.sim_rng.next_u32());
        w.sim.set_input(i, f);
    }
    w.sim.step();

    // Route across the map periodically. The floors are moving underneath it,
    // so the answer genuinely changes over the run rather than being one cached
    // result hashed ten thousand times.
    if ((w.tick % 97) == 0) {
        const dnp::nav::NavBody body;
        const u32 from = (w.tick / 97) % w.nav.node_count();
        const u32 to = (w.tick / 97 + 3) % w.nav.node_count();
        const dnp::nav::PathResult pr =
            dnp::nav::find_path(w.sim_view, w.nav, w.nav_query, from, to, body,
                                w.nav_path, dnp::nav::MAX_NAV_NODES);
        StateHasher nh;
        nh.u64v(w.nav_digest);
        nh.byte(static_cast<unsigned char>(pr.status));
        nh.u32v(pr.length);
        nh.fxv(pr.cost);
        nh.u32v(pr.expanded);
        for (u32 i = 0; i < pr.length; ++i) nh.u32v(w.nav_path[i]);
        w.nav_digest = nh.value();
        ++w.nav_queries;
        if (pr.status == dnp::nav::PathStatus::Ok) ++w.nav_routes_found;
    }

    // Ride the rail. The sector it is bolted to slides every tick, so this is
    // sampling a moving polyline rather than a fixed one.
    {
        const fx along = fx::from_int(static_cast<i64>(w.tick % 64));
        const dnp::geom::RailPoint rp = dnp::geom::rail_point_at(w.sim_view, 0, along);
        StateHasher rh;
        rh.u64v(w.rail_digest);
        rh.vec3(rp.pos);
        rh.u32v(rp.segment);
        rh.fxv(rp.t);
        rh.boolv(rp.clamped);
        rh.fxv(dnp::geom::rail_length(w.sim_view, 0));
        w.rail_digest = rh.value();
    }

    // Coverage bookkeeping, deliberately outside the hash: this counts what the
    // trace EXERCISED, and a counter in the hashed state would make the gate
    // compare its own instrumentation.
    for (u32 i = 0; i < SIM_AGENTS; ++i) {
        const dnp::sim::AgentState& a = w.sim.agent(i);
        if (a.active && a.mv.anchored && a.mv.anchor == 5) ++w.discrete_anchor_ticks;
    }
    for (u32 i = 0; i < w.sim.crushes().count(); ++i) {
        if (w.sim.crushes().at(i).kind == dnp::sim::CrushKind::Vertical) ++w.vertical_crush_ticks;
        else ++w.horizontal_pin_ticks;
    }

    ++w.tick;
}

// Full state dump for one tick, in a diffable line-per-field form. This is what
// makes a divergence actionable: the bisector finds the first bad tick, then
// dumps both sides here and diffs them to name the exact field that drifted.
void world_dump(const World& w) {
    std::printf("tick %u global_rng %016llX\n", w.tick,
                static_cast<unsigned long long>(w.global.state()));
    for (int i = 0; i < BODY_COUNT; ++i) {
        const Body& b = w.bodies[i];
        std::printf("body %02d pos %020lld %020lld %020lld\n", i,
                    static_cast<long long>(b.pos.x.raw),
                    static_cast<long long>(b.pos.y.raw),
                    static_cast<long long>(b.pos.z.raw));
        std::printf("body %02d vel %020lld %020lld %020lld\n", i,
                    static_cast<long long>(b.vel.x.raw),
                    static_cast<long long>(b.vel.y.raw),
                    static_cast<long long>(b.vel.z.raw));
        std::printf("body %02d ang %010u %010u rng %016llX\n", i,
                    b.heading.raw, b.spin.raw,
                    static_cast<unsigned long long>(b.rng.state()));
    }
    for (u32 i = 0; i < SIM_AGENTS; ++i) {
        const dnp::sim::AgentState& a = w.sim.agent(i);
        std::printf("agent %02u pos %020lld %020lld %020lld vel %020lld %020lld %020lld "
                    "sec %d j %u d %u dt %020lld hf %020lld ff %020lld g %d c %d\n",
                    i,
                    static_cast<long long>(a.pos.x.raw), static_cast<long long>(a.pos.y.raw),
                    static_cast<long long>(a.pos.z.raw),
                    static_cast<long long>(a.vel.x.raw), static_cast<long long>(a.vel.y.raw),
                    static_cast<long long>(a.vel.z.raw),
                    a.mv.sector, a.mv.jumps_used, a.mv.dashes_used,
                    static_cast<long long>(a.mv.dash_timer.raw),
                    static_cast<long long>(a.mv.hover_fuel.raw),
                    static_cast<long long>(a.mv.flight_fuel.raw),
                    a.mv.grappling ? 1 : 0, a.mv.climbing ? 1 : 0);
    }
    for (u32 i = 0; i < w.sim.motions().count(); ++i) {
        const dnp::sim::SectorMotion& m = w.sim.motions().at(i);
        std::printf("motion %u sector %u off %020lld %020lld %020lld\n", i, m.sector,
                    static_cast<long long>(m.offset.x.raw),
                    static_cast<long long>(m.offset.y.raw),
                    static_cast<long long>(m.offset.z.raw));
    }
}

u64 world_hash(const World& w) {
    StateHasher h;
    h.u32v(w.tick);
    h.u64v(w.global.state());
    for (int i = 0; i < BODY_COUNT; ++i) {
        const Body& b = w.bodies[i];
        h.vec3(b.pos);
        h.vec3(b.vel);
        h.bradv(b.heading);
        h.bradv(b.spin);
        h.u64v(b.rng.state());
        h.u64v(b.rng.stream_inc());
    }
    h.u64v(w.sim.hash());
    h.u64v(w.rail_digest);
    h.u64v(w.nav_digest);
    h.u64v(w.sim_rng.state());
    return h.value();
}

}  // namespace

int main(int argc, char** argv) {
    int  ticks    = 10000;
    bool summary  = false;
    bool coverage = false;
    int  dump_at  = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--summary") == 0) {
            summary = true;
        } else if (std::strcmp(argv[i], "--coverage") == 0) {
            coverage = true;
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_at = std::atoi(argv[++i]);
        } else {
            ticks = std::atoi(argv[i]);
        }
    }

    // Static, not a stack local. This struct now carries a MapStorage and a
    // SectorMotionSet -- around half a megabyte -- and Emscripten's stack is
    // 64 KB. Large simulation state belongs off the stack on every target; wasm
    // is just where forgetting shows up immediately rather than by luck.
    static World w;
    world_init(w);

    if (dump_at >= 0) {
        for (int t = 0; t <= dump_at; ++t) world_step(w);
        world_dump(w);
        return 0;
    }

    u64 last = 0;
    for (int t = 0; t < ticks; ++t) {
        world_step(w);
        last = world_hash(w);
        if (!summary && !coverage) std::printf("%d %016llX\n", t, static_cast<unsigned long long>(last));
    }

    // Which paths this trace actually walked. A gate that compares three targets
    // over code none of them executes proves only that they agree about nothing,
    // so the counts are worth being able to read.
    if (coverage) {
        std::printf("ticks %d\n", ticks);
        std::printf("vertical_crush_events %u\n", w.vertical_crush_ticks);
        std::printf("horizontal_pin_events %u\n", w.horizontal_pin_ticks);
        std::printf("nav_queries %u\n", w.nav_queries);
        std::printf("nav_routes_found %u\n", w.nav_routes_found);
        std::printf("discrete_anchor_ticks %u\n", w.discrete_anchor_ticks);
        // Routes must be found AND refused over the run: a gate that only ever
        // saw "unreachable" would compare three targets agreeing about nothing.
        return (w.vertical_crush_ticks > 0 && w.horizontal_pin_ticks > 0 &&
                w.nav_routes_found > 0 && w.nav_routes_found < w.nav_queries &&
                w.discrete_anchor_ticks > 0) ? 0 : 1;
    }
    if (summary) std::printf("final %d %016llX\n", ticks, static_cast<unsigned long long>(last));
    return 0;
}
