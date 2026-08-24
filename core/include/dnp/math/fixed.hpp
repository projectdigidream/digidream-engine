#pragma once
//
// Deterministic fixed-point scalar, Q32.32.
//
// CONTRACT: no `float`, `double` or `long double` appears anywhere under core/.
// Every value the simulation carries is an integer, so the wasm client and the
// native server produce bit-identical state. That is the premise client-side
// prediction rests on (design 7.1, 9.3) -- if it ever stops holding, prediction
// silently rots into constant mispredictions with no gameplay cause behind them.
//
// Two rounding rules are fixed here and must not drift:
//   * multiply and divide both TRUNCATE TOWARD ZERO.
//     Truncation (rather than floor) is deliberate: it keeps (-a)*b == -(a*b),
//     so a level mirrored across an axis simulates identically to its mirror.
//     Floor rounding breaks that symmetry.
//   * add and subtract SATURATE at the i64 range rather than wrapping.
//     Saturation is defined behaviour on every target; signed overflow is UB
//     and a compiler is free to make it differ between builds.
//
#include <cstdint>

namespace dnp {

using i8   = std::int8_t;
using i16  = std::int16_t;
using i32  = std::int32_t;
using i64  = std::int64_t;
using u8   = std::uint8_t;
using u16  = std::uint16_t;
using u32  = std::uint32_t;
using u64  = std::uint64_t;
using i128 = __int128;
using u128 = unsigned __int128;

inline constexpr i64 I64_MIN = (-9223372036854775807LL - 1);
inline constexpr i64 I64_MAX = 9223372036854775807LL;

// Arithmetic shift right spelled out. `>>` on a negative signed value is only
// guaranteed arithmetic from C++20 on; this is well-defined everywhere and
// compiles to the same single instruction. Rounds toward -inf.
constexpr i64 asr(i64 v, int s) noexcept {
    return v < 0 ? ~((~v) >> s) : (v >> s);
}

constexpr i128 asr128(i128 v, int s) noexcept {
    return v < 0 ? ~((~v) >> s) : (v >> s);
}

// Left shift, performed in unsigned arithmetic.
//
// `E1 << E2` on a NEGATIVE E1 is undefined behaviour before C++20, and the
// compiler is entitled to resolve UB differently between two builds -- which is
// exactly the divergence class the whole fixed-point contract exists to
// eliminate. Doing the shift on the unsigned bit pattern is well-defined,
// compiles to the same single instruction, and cannot surprise us on one target.
// (Converting back out of range is implementation-defined rather than undefined,
// and is modular on every compiler this project supports.)
constexpr i64 asl(i64 v, int s) noexcept {
    return static_cast<i64>(static_cast<u64>(v) << s);
}

constexpr i128 asl128(i128 v, int s) noexcept {
    return static_cast<i128>(static_cast<u128>(v) << s);
}

// Shift right truncating toward zero (the rule this codebase uses for products).
constexpr i128 tsr128(i128 v, int s) noexcept {
    return v < 0 ? -((-v) >> s) : (v >> s);
}

constexpr i64 sat_add(i64 a, i64 b) noexcept {
    const u64 ua = static_cast<u64>(a);
    const u64 ub = static_cast<u64>(b);
    const u64 s  = ua + ub;
    // Overflowed iff the operands shared a sign and the result does not.
    if (((ua ^ s) & (ub ^ s)) >> 63) return a < 0 ? I64_MIN : I64_MAX;
    return static_cast<i64>(s);
}

constexpr i64 sat_from_i128(i128 v) noexcept {
    if (v > static_cast<i128>(I64_MAX)) return I64_MAX;
    if (v < static_cast<i128>(I64_MIN)) return I64_MIN;
    return static_cast<i64>(v);
}

struct fx {
    i64 raw;

    static constexpr int FRAC = 32;
    static constexpr i64 ONE  = static_cast<i64>(1) << FRAC;
    static constexpr i64 HALF = ONE >> 1;

    constexpr fx() noexcept : raw(0) {}

    static constexpr fx from_raw(i64 r) noexcept { fx f; f.raw = r; return f; }
    static constexpr fx from_int(i64 v) noexcept { return from_raw(v * ONE); }

    // Exact-to-one-ulp rational literal, evaluated in 128-bit: from_frac(1, 3).
    // Preferred over any float-derived constant, which would not be portable.
    static constexpr fx from_frac(i64 n, i64 d) noexcept {
        return from_raw(sat_from_i128(asl128(static_cast<i128>(n), FRAC) / d));
    }

    static constexpr fx zero() noexcept { return from_raw(0); }
    static constexpr fx one()  noexcept { return from_raw(ONE); }
    static constexpr fx min()  noexcept { return from_raw(I64_MIN); }
    static constexpr fx max()  noexcept { return from_raw(I64_MAX); }
    // Smallest representable step: 2^-32.
    static constexpr fx eps()  noexcept { return from_raw(1); }

    constexpr i64 floor_int() const noexcept { return asr(raw, FRAC); }
    constexpr i64 trunc_int() const noexcept { return raw < 0 ? -((-raw) >> FRAC) : (raw >> FRAC); }
};

constexpr fx operator+(fx a, fx b) noexcept { return fx::from_raw(sat_add(a.raw, b.raw)); }
constexpr fx operator-(fx a, fx b) noexcept {
    // Negating I64_MIN would overflow, so saturate the operand first.
    return fx::from_raw(sat_add(a.raw, b.raw == I64_MIN ? I64_MAX : -b.raw));
}
constexpr fx operator-(fx a) noexcept { return fx::from_raw(a.raw == I64_MIN ? I64_MAX : -a.raw); }

constexpr fx operator*(fx a, fx b) noexcept {
    return fx::from_raw(sat_from_i128(tsr128(static_cast<i128>(a.raw) * static_cast<i128>(b.raw), fx::FRAC)));
}

// Scaling by a plain integer is exact and much cheaper than a full fx multiply.
constexpr fx operator*(fx a, i64 k) noexcept { return fx::from_raw(sat_from_i128(static_cast<i128>(a.raw) * k)); }
constexpr fx operator*(i64 k, fx a) noexcept { return a * k; }

constexpr fx operator/(fx a, fx b) noexcept {
    if (b.raw == 0) return a.raw >= 0 ? fx::max() : fx::min();  // defined, never a trap
    return fx::from_raw(sat_from_i128(asl128(static_cast<i128>(a.raw), fx::FRAC) / b.raw));
}

constexpr fx operator/(fx a, i64 k) noexcept {
    if (k == 0) return a.raw >= 0 ? fx::max() : fx::min();
    return fx::from_raw(static_cast<i64>(static_cast<i128>(a.raw) / k));
}

constexpr fx& operator+=(fx& a, fx b) noexcept { a = a + b; return a; }
constexpr fx& operator-=(fx& a, fx b) noexcept { a = a - b; return a; }
constexpr fx& operator*=(fx& a, fx b) noexcept { a = a * b; return a; }
constexpr fx& operator/=(fx& a, fx b) noexcept { a = a / b; return a; }

constexpr bool operator==(fx a, fx b) noexcept { return a.raw == b.raw; }
constexpr bool operator!=(fx a, fx b) noexcept { return a.raw != b.raw; }
constexpr bool operator< (fx a, fx b) noexcept { return a.raw <  b.raw; }
constexpr bool operator<=(fx a, fx b) noexcept { return a.raw <= b.raw; }
constexpr bool operator> (fx a, fx b) noexcept { return a.raw >  b.raw; }
constexpr bool operator>=(fx a, fx b) noexcept { return a.raw >= b.raw; }

constexpr fx fx_abs(fx a) noexcept { return a.raw < 0 ? -a : a; }
constexpr fx fx_min(fx a, fx b) noexcept { return a.raw < b.raw ? a : b; }
constexpr fx fx_max(fx a, fx b) noexcept { return a.raw > b.raw ? a : b; }
constexpr fx fx_clamp(fx v, fx lo, fx hi) noexcept { return fx_min(fx_max(v, lo), hi); }
constexpr i32 fx_sign(fx a) noexcept { return a.raw > 0 ? 1 : (a.raw < 0 ? -1 : 0); }

// Linear interpolation. Written as a + (b-a)*t rather than a*(1-t) + b*t so that
// t == 0 returns exactly a and t == 1 returns exactly b, with no rounding drift
// at the endpoints -- parametric sector motion (design 3.1.2) depends on hitting
// its path endpoints exactly on both client and server.
constexpr fx fx_lerp(fx a, fx b, fx t) noexcept { return a + (b - a) * t; }

// Integer square root of a 128-bit value, bit-by-bit. No float, no libm, no
// convergence loop whose iteration count could vary by platform.
constexpr u128 isqrt128(u128 n) noexcept {
    u128 res = 0;
    u128 bit = static_cast<u128>(1) << 126;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

// sqrt(x) for x >= 0. Negative input yields zero rather than trapping: the core
// must never fault on data a script could produce (design 8.7's "defined no-op"
// philosophy applied to arithmetic).
constexpr fx fx_sqrt(fx x) noexcept {
    if (x.raw <= 0) return fx::zero();
    // r = sqrt(raw / 2^32) * 2^32 = sqrt(raw * 2^32)
    return fx::from_raw(static_cast<i64>(isqrt128(static_cast<u128>(x.raw) << fx::FRAC)));
}

}  // namespace dnp
