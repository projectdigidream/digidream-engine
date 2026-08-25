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
#include "dnp/sim/testmap.hpp"
#include "dnp/sim/world.hpp"

using namespace dnp;

namespace {

constexpr int BODY_COUNT = 64;

// The fixed simulation timestep (design 7.1.1). 60 Hz, as an exact rational --
// never a measured frame time, and never a decimal literal that would round
// differently if the rate were ever edited.
const fx DT = fx::from_frac(1, 60);

const fx GRAVITY   = fx::from_frac(-1600, 100);   // -16.00 units/s^2
const fx BOUNDS    = fx::from_int(48);
const fx RESTITUTION = fx::from_frac(60, 100);

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
    w.sim_view = w.sim_map.view();
    w.sim.set_map(&w.sim_view);
    w.sim.set_map_storage(&w.sim_map);

    // Sector 1 ping-pongs vertically, and sector 2 slides horizontally, so both
    // the lift case and the sweeping-footprint case are exercised.
    w.sim.motions().add(w.sim_map, 1);
    w.sim.motions().start_parametric(1, fx3::zero(), fx3{fx::zero(), fx::zero(), fx::from_int(3)},
                                     0, 137, dnp::sim::LoopMode::PingPong);
    w.sim.motions().add(w.sim_map, 2);
    w.sim.motions().start_parametric(2, fx3::zero(), fx3{fx::from_frac(3, 2), fx::zero(), fx::zero()},
                                     0, 211, dnp::sim::LoopMode::PingPong);
    w.sim_rng = Rng(0x5EED, 2);
    for (int i = 0; i < 8; ++i) {
        w.sim.spawn(fx3{fx::from_int(5 + i * 4), fx::from_int(5 + i * 3), fx::from_int(3)});
    }
    // Turn every ability on for at least some agents, so the trace exercises
    // hover, flight and grapple rather than only walking and jumping.
    for (u32 i = 0; i < 8; ++i) {
        auto& pr = w.sim.params_mut(i);
        pr.hover_enabled = (i % 2) == 0;
        pr.flight_enabled = (i % 3) == 0;
        pr.grapple_enabled = (i % 2) == 1;
        pr.max_jumps = static_cast<u8>(1 + (i % 3));
    }
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
    for (u32 i = 0; i < 8; ++i) {
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
    for (u32 i = 0; i < 8; ++i) {
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
    h.u64v(w.sim_rng.state());
    return h.value();
}

}  // namespace

int main(int argc, char** argv) {
    int  ticks    = 10000;
    bool summary  = false;
    int  dump_at  = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--summary") == 0) {
            summary = true;
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
        if (!summary) std::printf("%d %016llX\n", t, static_cast<unsigned long long>(last));
    }
    if (summary) std::printf("final %d %016llX\n", ticks, static_cast<unsigned long long>(last));
    return 0;
}
