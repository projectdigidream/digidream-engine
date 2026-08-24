#pragma once
//
// Fixed-point 2D/3D vectors. See fixed.hpp for the rounding contract.
//
#include "dnp/math/fixed.hpp"

namespace dnp {

struct fx2 {
    fx x, y;
    constexpr fx2() noexcept = default;
    constexpr fx2(fx x_, fx y_) noexcept : x(x_), y(y_) {}
    static constexpr fx2 zero() noexcept { return fx2{fx::zero(), fx::zero()}; }
};

struct fx3 {
    fx x, y, z;
    constexpr fx3() noexcept = default;
    constexpr fx3(fx x_, fx y_, fx z_) noexcept : x(x_), y(y_), z(z_) {}
    static constexpr fx3 zero() noexcept { return fx3{fx::zero(), fx::zero(), fx::zero()}; }
};

constexpr fx2 operator+(fx2 a, fx2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
constexpr fx2 operator-(fx2 a, fx2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
constexpr fx2 operator-(fx2 a)        noexcept { return {-a.x, -a.y}; }
constexpr fx2 operator*(fx2 a, fx s)  noexcept { return {a.x * s, a.y * s}; }
constexpr fx2 operator*(fx s, fx2 a)  noexcept { return a * s; }
constexpr bool operator==(fx2 a, fx2 b) noexcept { return a.x == b.x && a.y == b.y; }
constexpr bool operator!=(fx2 a, fx2 b) noexcept { return !(a == b); }

constexpr fx3 operator+(fx3 a, fx3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr fx3 operator-(fx3 a, fx3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr fx3 operator-(fx3 a)        noexcept { return {-a.x, -a.y, -a.z}; }
constexpr fx3 operator*(fx3 a, fx s)  noexcept { return {a.x * s, a.y * s, a.z * s}; }
constexpr fx3 operator*(fx s, fx3 a)  noexcept { return a * s; }
constexpr bool operator==(fx3 a, fx3 b) noexcept { return a.x == b.x && a.y == b.y && a.z == b.z; }
constexpr bool operator!=(fx3 a, fx3 b) noexcept { return !(a == b); }

constexpr fx2& operator+=(fx2& a, fx2 b) noexcept { a = a + b; return a; }
constexpr fx2& operator-=(fx2& a, fx2 b) noexcept { a = a - b; return a; }
constexpr fx3& operator+=(fx3& a, fx3 b) noexcept { a = a + b; return a; }
constexpr fx3& operator-=(fx3& a, fx3 b) noexcept { a = a - b; return a; }

constexpr fx dot(fx2 a, fx2 b) noexcept { return a.x * b.x + a.y * b.y; }
constexpr fx dot(fx3 a, fx3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

// 2D "cross" (z of the 3D cross). Sign tells which side of `a` the vector `b`
// lies on -- the primitive ear-clipping and wall-side tests are built from.
constexpr fx cross(fx2 a, fx2 b) noexcept { return a.x * b.y - a.y * b.x; }

constexpr fx3 cross(fx3 a, fx3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

// Squared magnitude as a raw 128-bit value at scale 2^64. Comparisons against a
// squared threshold should use this rather than length_sq(): it cannot overflow
// and it avoids the sqrt entirely, which matters in the broadphase and in the
// CCD velocity-threshold test (design 7.1).
constexpr u128 len_sq_raw(fx2 v) noexcept {
    return static_cast<u128>(static_cast<i128>(v.x.raw) * v.x.raw)
         + static_cast<u128>(static_cast<i128>(v.y.raw) * v.y.raw);
}

constexpr u128 len_sq_raw(fx3 v) noexcept {
    return static_cast<u128>(static_cast<i128>(v.x.raw) * v.x.raw)
         + static_cast<u128>(static_cast<i128>(v.y.raw) * v.y.raw)
         + static_cast<u128>(static_cast<i128>(v.z.raw) * v.z.raw);
}

// Squared magnitude as an fx. Saturates for large vectors; prefer len_sq_raw()
// when the value only feeds a comparison.
constexpr fx length_sq(fx2 v) noexcept { return fx::from_raw(sat_from_i128(static_cast<i128>(len_sq_raw(v) >> fx::FRAC))); }
constexpr fx length_sq(fx3 v) noexcept { return fx::from_raw(sat_from_i128(static_cast<i128>(len_sq_raw(v) >> fx::FRAC))); }

// Exact magnitude: isqrt of the un-shifted sum of squares. Because raw^2 lands
// at scale 2^64, its square root lands back at scale 2^32 with no intermediate
// shift and therefore no intermediate precision loss.
constexpr fx length(fx2 v) noexcept { return fx::from_raw(static_cast<i64>(isqrt128(len_sq_raw(v)))); }
constexpr fx length(fx3 v) noexcept { return fx::from_raw(static_cast<i64>(isqrt128(len_sq_raw(v)))); }

constexpr fx distance(fx2 a, fx2 b) noexcept { return length(b - a); }
constexpr fx distance(fx3 a, fx3 b) noexcept { return length(b - a); }

// Zero-length input returns the zero vector rather than trapping or producing
// garbage -- callers that care must test length() themselves.
constexpr fx2 normalize(fx2 v) noexcept {
    const fx len = length(v);
    if (len.raw == 0) return fx2::zero();
    return {v.x / len, v.y / len};
}

constexpr fx3 normalize(fx3 v) noexcept {
    const fx len = length(v);
    if (len.raw == 0) return fx3::zero();
    return {v.x / len, v.y / len, v.z / len};
}

constexpr fx2 lerp(fx2 a, fx2 b, fx t) noexcept { return {fx_lerp(a.x, b.x, t), fx_lerp(a.y, b.y, t)}; }
constexpr fx3 lerp(fx3 a, fx3 b, fx t) noexcept { return {fx_lerp(a.x, b.x, t), fx_lerp(a.y, b.y, t), fx_lerp(a.z, b.z, t)}; }

// Remove the component of `v` pointing along `n`, keeping the tangential part.
// This is the whole of the wall-sliding rule in design 7.1: velocity into the
// wall is dropped, velocity along it survives, so corners do not feel sticky.
// `n` is expected to be unit length.
constexpr fx3 project_out(fx3 v, fx3 n) noexcept { return v - n * dot(v, n); }
constexpr fx2 project_out(fx2 v, fx2 n) noexcept { return v - n * dot(v, n); }

constexpr fx3 to_fx3(fx2 v, fx z) noexcept { return {v.x, v.y, z}; }
constexpr fx2 xy(fx3 v) noexcept { return {v.x, v.y}; }

}  // namespace dnp
