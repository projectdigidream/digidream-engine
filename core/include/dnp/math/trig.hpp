#pragma once
//
// Deterministic trigonometry via CORDIC. No libm: sin/cos/atan2 in libm are not
// correctly-rounded and differ between platforms and libc versions, which is
// exactly the kind of divergence the fixed-point contract exists to rule out.
//
// Angles are BINARY RADIANS: a full turn is 2^32, held in a u32. Wrapping is
// free (it is just unsigned overflow), there is no pi-modulo reduction step to
// get subtly wrong, and every u32 is a valid angle.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/vec.hpp"

namespace dnp {

struct brad {
    u32 raw;
    constexpr brad() noexcept : raw(0) {}
    explicit constexpr brad(u32 r) noexcept : raw(r) {}

    static constexpr brad turns_frac(i64 num, i64 den) noexcept {
        return brad(static_cast<u32>((static_cast<i128>(num) << 32) / den));
    }
    static constexpr brad from_degrees(i64 deg) noexcept { return turns_frac(deg, 360); }
};

inline constexpr brad BRAD_QUARTER = brad(0x40000000u);
inline constexpr brad BRAD_HALF    = brad(0x80000000u);

// Wrapping is intentional: angles have no valid range to saturate against.
constexpr brad operator+(brad a, brad b) noexcept { return brad(a.raw + b.raw); }
constexpr brad operator-(brad a, brad b) noexcept { return brad(a.raw - b.raw); }
constexpr brad operator-(brad a)         noexcept { return brad(0u - a.raw); }
constexpr bool operator==(brad a, brad b) noexcept { return a.raw == b.raw; }
constexpr bool operator!=(brad a, brad b) noexcept { return a.raw != b.raw; }

// Shortest signed difference a-b, as a value in (-half turn, +half turn].
// Reinterpreting the wrapped u32 difference as i32 gives this for free.
constexpr i32 brad_delta(brad a, brad b) noexcept { return static_cast<i32>(a.raw - b.raw); }

// pi and tau in Q32.32, for callers that must convert to/from real radians.
inline constexpr fx FX_PI  = fx::from_raw(13493037705LL);
inline constexpr fx FX_TAU = fx::from_raw(26986075409LL);

void  fx_sincos(brad a, fx& out_sin, fx& out_cos) noexcept;
fx    fx_sin(brad a) noexcept;
fx    fx_cos(brad a) noexcept;

// Angle of the vector (x, y): 0 is +X, increasing counter-clockwise.
// atan2(0, 0) is defined to be 0 rather than an error.
brad  fx_atan2(fx y, fx x) noexcept;

inline brad fx_angle_of(fx2 v) noexcept { return fx_atan2(v.y, v.x); }

// Unit vector at the given angle.
inline fx2 fx_dir(brad a) noexcept {
    fx s, c;
    fx_sincos(a, s, c);
    return fx2{c, s};
}

// Rotate about +Z. Used by the camera's single permitted rotation axis
// (design 4) and by rigid sector transforms (design 3.1.1).
inline fx2 rotate(fx2 v, brad a) noexcept {
    fx s, c;
    fx_sincos(a, s, c);
    return fx2{v.x * c - v.y * s, v.x * s + v.y * c};
}

inline fx3 rotate_z(fx3 v, brad a) noexcept {
    const fx2 r = rotate(fx2{v.x, v.y}, a);
    return fx3{r.x, r.y, v.z};
}

}  // namespace dnp
