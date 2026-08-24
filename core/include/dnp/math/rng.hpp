#pragma once
//
// Deterministic PRNG (PCG-XSH-RR 64/32). Pure integer arithmetic, fixed state
// size, identical stream on every target.
//
// The core must never call rand(), std::mt19937 (whose seeding differs across
// standard libraries) or anything seeded from wall-clock time. Any randomness
// the simulation consumes -- damage spread, enemy jitter, particle seeds -- has
// to come from a generator whose state is part of the replicated world state,
// or the client's prediction replay would diverge from the server the first
// time a random number was drawn.
//
#include "dnp/math/fixed.hpp"

namespace dnp {

class Rng {
public:
    constexpr Rng() noexcept : state_(0x853c49e6748fea9bULL), inc_(0xda3e39cb94b95bdbULL) {}

    constexpr explicit Rng(u64 seed, u64 stream = 0) noexcept : state_(0), inc_((stream << 1u) | 1u) {
        next_u32();
        state_ += seed;
        next_u32();
    }

    constexpr u32 next_u32() noexcept {
        const u64 old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const u32 xorshifted = static_cast<u32>(((old >> 18u) ^ old) >> 27u);
        const u32 rot        = static_cast<u32>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    constexpr u64 next_u64() noexcept {
        const u64 hi = static_cast<u64>(next_u32());
        return (hi << 32) | static_cast<u64>(next_u32());
    }

    // Uniform in [0, bound). Rejection-sampled rather than a plain modulo, so
    // the distribution has no low-value bias -- and, more importantly, so the
    // number of draws consumed is a deterministic function of the stream.
    constexpr u32 next_below(u32 bound) noexcept {
        if (bound == 0) return 0;
        const u32 threshold = (0u - bound) % bound;
        for (;;) {
            const u32 r = next_u32();
            if (r >= threshold) return r % bound;
        }
    }

    constexpr i64 next_range(i64 lo, i64 hi) noexcept {
        if (hi <= lo) return lo;
        return lo + static_cast<i64>(next_below(static_cast<u32>(hi - lo)));
    }

    // Uniform fx in [0, 1).
    constexpr fx next_unit() noexcept { return fx::from_raw(static_cast<i64>(next_u32())); }

    constexpr fx next_fx(fx lo, fx hi) noexcept { return lo + (hi - lo) * next_unit(); }

    constexpr u64 state() const noexcept { return state_; }
    constexpr u64 stream_inc() const noexcept { return inc_; }
    constexpr void set_state(u64 s, u64 inc) noexcept { state_ = s; inc_ = inc | 1u; }

private:
    u64 state_;
    u64 inc_;
};

}  // namespace dnp
