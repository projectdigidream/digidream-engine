#pragma once
//
// Universal simulation constants.
//
// These are part of the fixed contract of design section 2, NOT per-server
// tunables. A forked server may change player limits, snapshot rate and all
// gameplay Lua; it may not change anything here, because a standard client
// predicts with these values and a server that used different ones would break
// prediction for every client built against the standard.
//
// Anything a server IS allowed to vary belongs in its own configuration, not in
// this file.
//
#include "dnp/math/fixed.hpp"

namespace dnp::sim {

// Fixed simulation rate (docs/physics-contract.md rule 3). Rendering may run at
// any frame rate and interpolate; the simulation step is always exactly this.
inline constexpr u32 TICK_RATE_HZ = 60;

// Written as an exact rational rather than a decimal literal, so that changing
// the rate cannot introduce a rounding difference that only shows up as drift.
inline constexpr fx DT = fx::from_frac(1, TICK_RATE_HZ);

// Step-up / step-down threshold (design 7.1). One constant serves both
// directions by construction, so "walkable" is symmetric and there is no second
// value to keep in sync. Slope walkability is a consequence of this applied
// incrementally, not a separate parameter.
inline constexpr fx STEP_HEIGHT = fx::from_frac(1, 2);

// Default gravity. Per-entity `gravity_scale` (design 7.2) multiplies this;
// Lua tunes the scale, never the base.
inline constexpr fx GRAVITY = fx::from_frac(-1600, 100);

// Above this per-tick displacement, collision uses a swept test instead of a
// discrete one (design 7.1, CCD). Below it, the cheaper check is used.
inline constexpr fx CCD_DISPLACEMENT_THRESHOLD = fx::from_frac(1, 4);

}  // namespace dnp::sim
