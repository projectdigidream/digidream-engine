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

    World w;
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
