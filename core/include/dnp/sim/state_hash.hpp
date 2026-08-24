#pragma once
//
// Per-tick world-state hash (FNV-1a, 64-bit).
//
// This is the instrument the whole determinism contract is measured with. Every
// build target runs the same input trace and emits one hash per tick; if the
// hash streams are not byte-identical the build fails. Without it, a divergence
// introduced today surfaces weeks later as an unreproducible "rubber-banding"
// bug with no way to bisect it.
//
// Feed values through the typed helpers rather than hashing raw structs: struct
// padding is uninitialised memory and would make the hash differ between builds
// for reasons that have nothing to do with the simulation.
//
#include "dnp/math/fixed.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"

namespace dnp {

class StateHasher {
public:
    using u8_t = unsigned char;

    static constexpr u64 OFFSET_BASIS = 14695981039346656037ULL;
    static constexpr u64 PRIME        = 1099511628211ULL;

    constexpr StateHasher() noexcept : h_(OFFSET_BASIS) {}

    constexpr void byte(u8_t b) noexcept {
        h_ ^= static_cast<u64>(b);
        h_ *= PRIME;
    }

    // Little-endian byte order is written out explicitly rather than memcpy'd,
    // so the hash does not depend on the host's endianness.
    constexpr void u64v(u64 v) noexcept {
        for (int i = 0; i < 8; ++i) byte(static_cast<u8_t>((v >> (i * 8)) & 0xFF));
    }
    constexpr void u32v(u32 v) noexcept {
        for (int i = 0; i < 4; ++i) byte(static_cast<u8_t>((v >> (i * 8)) & 0xFF));
    }
    constexpr void i64v(i64 v) noexcept { u64v(static_cast<u64>(v)); }
    constexpr void i32v(i32 v) noexcept { u32v(static_cast<u32>(v)); }
    constexpr void boolv(bool v) noexcept { byte(v ? 1 : 0); }

    constexpr void fxv(fx v)   noexcept { i64v(v.raw); }
    constexpr void bradv(brad v) noexcept { u32v(v.raw); }
    constexpr void vec2(fx2 v) noexcept { fxv(v.x); fxv(v.y); }
    constexpr void vec3(fx3 v) noexcept { fxv(v.x); fxv(v.y); fxv(v.z); }

    constexpr u64 value() const noexcept { return h_; }
    constexpr void reset() noexcept { h_ = OFFSET_BASIS; }

private:
    u64 h_;
};

}  // namespace dnp
