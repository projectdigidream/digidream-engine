#pragma once
//
// A BLAST'S REACH AND SHOVE: the arithmetic of mobs.lua's damage_area and
// apply_hit, in the core so the two sides that need it run ONE rule.
//
// The server's Lua calls it through dnp.blast_reach and dnp.knock_impulse. A
// predicting client calls it through its wasm world when its own rocket bursts
// at its own feet, so the rocket jump is felt on the tick it happens rather
// than a round trip later -- and lands on the same bits, so reconciliation
// finds nothing to correct (docs/WEAPON_PREDICTION.md, phase G).
//
// Written to be the Lua it replaced, operation for operation: the same fx
// operators in the same order. In particular the falloff's distance is PLANAR
// (dnp.length is two-dimensional) while the reach is spherical -- which is
// what the script did, and changing either here would change every blast.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"

namespace dnp::sim {

struct BlastReach {
    bool reached = false;
    bool scaled = false;   // a falloff applies: `share` is meaningful
    fx   share{};          // (radius - planar distance) / radius
};

// Is a body at `at` inside a blast of `radius` centred on `centre`, and by
// what share when it falls off?
inline BlastReach blast_reach(fx3 centre, fx3 at, fx radius, bool falloff) noexcept {
    BlastReach out;
    const fx dx = at.x - centre.x;
    const fx dy = at.y - centre.y;
    const fx dz = at.z - centre.z;
    const fx d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > radius * radius) return out;
    out.reached = true;
    if (falloff && radius.raw != 0) {
        const fx d = length(fx2{dx, dy});
        out.share = (radius - d) / radius;
        out.scaled = true;
    }
    return out;
}

// The velocity a shove of `knockback` and `lift`, already scaled by `k` (the
// share times 100 / mass), gives a body at `at` thrown from `origin`: outward
// in the plane, straight along +x when it is exactly on the origin.
inline fx3 knock_impulse(fx3 origin, fx3 at, fx knockback, fx lift, fx k) noexcept {
    const fx dx = at.x - origin.x;
    const fx dy = at.y - origin.y;
    const brad ang = (dx.raw == 0 && dy.raw == 0) ? brad(0u) : fx_atan2(dy, dx);
    const fx2 d = fx_dir(ang);
    const fx kb = knockback * k;
    return fx3{d.x * kb, d.y * kb, lift * k};
}

// The scale a share and a mass make, as apply_hit computes it.
inline fx knock_scale(const BlastReach& r, i64 mass) noexcept {
    const fx share = r.scaled ? r.share : fx::from_int(1);
    return share * fx::from_frac(100, mass);
}

}  // namespace dnp::sim
