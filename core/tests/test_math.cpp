//
// Unit and property tests for core/math.
//
// NOTE: this file uses <cmath> doubles purely as an ACCURACY ORACLE. Tests are
// not part of the deterministic core and never link into it -- the CI "no
// floats" check excludes core/tests/ for exactly this reason. Nothing here
// feeds a value back into the fixed-point path.
//
#include <cmath>
#include <cstdio>

#include "dnp/math/fixed.hpp"
#include "dnp/math/rng.hpp"
#include "dnp/math/trig.hpp"
#include "dnp/math/vec.hpp"
#include "dnp/sim/state_hash.hpp"

using namespace dnp;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_fails;                                                           \
            std::printf("  FAIL %s:%d   %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

// Oracle helpers (test-only).
static double to_d(fx v) { return static_cast<double>(v.raw) / 4294967296.0; }
static double brad_to_rad(brad a) { return static_cast<double>(a.raw) / 4294967296.0 * 2.0 * M_PI; }

// ---------------------------------------------------------------- fixed point

static void test_fixed_basics() {
    std::printf("fixed: basics\n");
    CHECK(fx::from_int(1) == fx::one());
    CHECK(fx::from_int(0) == fx::zero());
    CHECK((fx::from_int(3) + fx::from_int(4)) == fx::from_int(7));
    CHECK((fx::from_int(3) - fx::from_int(4)) == fx::from_int(-1));
    CHECK((fx::from_int(3) * fx::from_int(4)) == fx::from_int(12));
    CHECK((fx::from_int(12) / fx::from_int(4)) == fx::from_int(3));

    // Multiplying by exactly one must be the identity, with no rounding drift.
    Rng rng(12345);
    for (int i = 0; i < 20000; ++i) {
        const fx v = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 20);
        CHECK(v * fx::one() == v);
        CHECK(v + fx::zero() == v);
        CHECK(v - v == fx::zero());
    }

    // from_frac is exact to one ulp of the true rational.
    CHECK(fx::from_frac(1, 2) == fx::from_raw(fx::ONE / 2));
    CHECK(fx::from_frac(1, 4) == fx::from_raw(fx::ONE / 4));
    CHECK(fx::from_frac(-1, 2) == fx::from_raw(-(fx::ONE / 2)));
    CHECK(fx::from_frac(1, 3).raw == 1431655765LL);  // floor(2^32 / 3)
}

static void test_mirror_symmetry() {
    // The reason multiply truncates toward zero instead of flooring: a level
    // mirrored across an axis must simulate identically to its mirror image.
    // Floor rounding would break this for every negative product.
    std::printf("fixed: mirror symmetry (-a)*b == -(a*b)\n");
    Rng rng(0xBEEF);
    for (int i = 0; i < 100000; ++i) {
        const fx a = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        const fx b = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        CHECK((-a) * b == -(a * b));
        CHECK(a * (-b) == -(a * b));
        CHECK((-a) * (-b) == a * b);
    }
    for (int i = 0; i < 50000; ++i) {
        const fx a = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        fx b = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        if (b.raw == 0) b = fx::one();
        CHECK((-a) / b == -(a / b));
    }
}

static void test_saturation() {
    std::printf("fixed: saturation instead of signed overflow (which is UB)\n");
    CHECK(fx::max() + fx::one() == fx::max());
    CHECK(fx::min() - fx::one() == fx::min());
    CHECK(fx::max() + fx::max() == fx::max());
    CHECK(fx::min() + fx::min() == fx::min());
    CHECK(-fx::min() == fx::max());          // negating I64_MIN must not wrap
    CHECK(fx::max() * fx::max() == fx::max());
    CHECK(fx::one() / fx::zero() == fx::max());   // defined, never a trap
    CHECK((-fx::one()) / fx::zero() == fx::min());
}

static void test_sqrt() {
    std::printf("fixed: sqrt is the exact floor of the true root\n");
    CHECK(fx_sqrt(fx::zero()) == fx::zero());
    CHECK(fx_sqrt(fx::from_int(-5)) == fx::zero());   // defined, not a trap
    CHECK(fx_sqrt(fx::one()) == fx::one());
    CHECK(fx_sqrt(fx::from_int(4)) == fx::from_int(2));
    CHECK(fx_sqrt(fx::from_int(144)) == fx::from_int(12));

    Rng rng(99);
    for (int i = 0; i < 50000; ++i) {
        const fx x = fx::from_raw(static_cast<i64>(rng.next_u64() >> 12));
        const fx r = fx_sqrt(x);
        // r must be the largest value with r^2 <= x, compared exactly in 128-bit.
        const u128 target = static_cast<u128>(x.raw) << fx::FRAC;
        const u128 lo = static_cast<u128>(r.raw) * static_cast<u128>(r.raw);
        const u128 hi = static_cast<u128>(r.raw + 1) * static_cast<u128>(r.raw + 1);
        CHECK(lo <= target);
        CHECK(hi > target);
    }
}

static void test_lerp_endpoints() {
    // Parametric sector motion (design 3.1.2) must land exactly on its path
    // endpoints on both client and server, or a platform would settle one ulp
    // short and register as still moving.
    std::printf("fixed: lerp hits both endpoints exactly\n");
    Rng rng(7);
    for (int i = 0; i < 20000; ++i) {
        const fx a = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        const fx b = fx::from_raw(static_cast<i64>(rng.next_u64()) >> 24);
        CHECK(fx_lerp(a, b, fx::zero()) == a);
        CHECK(fx_lerp(a, b, fx::one()) == b);
    }
}

// ---------------------------------------------------------------------- vec

static void test_vec() {
    std::printf("vec: length, normalize, projection\n");
    CHECK(length(fx3{fx::from_int(3), fx::from_int(4), fx::zero()}) == fx::from_int(5));
    CHECK(length(fx2{fx::from_int(3), fx::from_int(4)}) == fx::from_int(5));
    CHECK(length(fx3::zero()) == fx::zero());
    CHECK(normalize(fx3::zero()) == fx3::zero());

    Rng rng(4242);
    fx worst_unit = fx::zero();
    for (int i = 0; i < 20000; ++i) {
        const fx3 v{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 26),
                    fx::from_raw(static_cast<i64>(rng.next_u64()) >> 26),
                    fx::from_raw(static_cast<i64>(rng.next_u64()) >> 26)};
        if (length(v).raw < fx::ONE) continue;   // skip degenerate short vectors
        const fx3 n = normalize(v);
        const fx err = fx_abs(length(n) - fx::one());
        worst_unit = fx_max(worst_unit, err);

        // Wall sliding: after removing the normal component, nothing should
        // point into the wall any more.
        const fx3 slid = project_out(v, n);
        CHECK(fx_abs(dot(slid, n)).raw < (fx::ONE >> 8));
    }
    std::printf("       worst |len(normalize(v)) - 1| = %.3e\n", to_d(worst_unit));
    CHECK(worst_unit.raw < (fx::ONE >> 20));
}

// --------------------------------------------------------------------- trig

static void test_trig_exact_points() {
    // Cardinal angles must be EXACT, not merely close: rotate(v, 0) has to
    // return v untouched, and an axis-aligned wall normal must have exactly
    // zero off-axis component or sliding would leak velocity through it.
    std::printf("trig: cardinal angles are exact\n");
    fx s, c;
    fx_sincos(brad(0), s, c);
    CHECK(s == fx::zero());
    CHECK(c == fx::one());

    fx_sincos(BRAD_QUARTER, s, c);
    CHECK(s == fx::one());
    CHECK(c == fx::zero());

    fx_sincos(BRAD_HALF, s, c);
    CHECK(s == fx::zero());
    CHECK(c == -fx::one());

    fx_sincos(BRAD_QUARTER + BRAD_HALF, s, c);
    CHECK(s == -fx::one());
    CHECK(c == fx::zero());

    // ...and therefore rotation by zero, and by whole quarter turns, is exact.
    const fx2 v{fx::from_frac(1234, 100), fx::from_frac(-567, 100)};
    CHECK(rotate(v, brad(0)) == v);
    CHECK(rotate(v, BRAD_QUARTER) == fx2(-v.y, v.x));
    CHECK(rotate(v, BRAD_HALF) == fx2(-v.x, -v.y));
    CHECK(rotate(rotate(v, BRAD_QUARTER), BRAD_QUARTER + BRAD_HALF) == v);
}

static void test_trig_accuracy() {
    std::printf("trig: accuracy vs libm oracle, and the Pythagorean identity\n");
    double worst_sin = 0.0, worst_cos = 0.0;
    i64 worst_ident = 0;

    for (u64 i = 0; i < 200000; ++i) {
        const brad a = brad(static_cast<u32>(i * 21475ULL));
        fx s, c;
        fx_sincos(a, s, c);

        const double ref = brad_to_rad(a);
        worst_sin = std::fmax(worst_sin, std::fabs(to_d(s) - std::sin(ref)));
        worst_cos = std::fmax(worst_cos, std::fabs(to_d(c) - std::cos(ref)));

        // s^2 + c^2 == 1 computed entirely in fixed point.
        const fx ident = s * s + c * c;
        const i64 d = fx_abs(ident - fx::one()).raw;
        if (d > worst_ident) worst_ident = d;
    }
    std::printf("       worst |sin err| = %.3e   |cos err| = %.3e\n", worst_sin, worst_cos);
    std::printf("       worst |s^2+c^2-1| = %lld ulp (%.3e)\n",
                static_cast<long long>(worst_ident),
                static_cast<double>(worst_ident) / 4294967296.0);
    CHECK(worst_sin < 1e-8);
    CHECK(worst_cos < 1e-8);
    CHECK(worst_ident < (fx::ONE >> 20));
}

static void test_atan2() {
    std::printf("trig: atan2 quadrants and round-trip through fx_dir\n");
    const fx Z = fx::zero(), P = fx::one(), N = -fx::one();
    CHECK(fx_atan2(Z, P) == brad(0));                       // exact on the axes
    CHECK(fx_atan2(P, Z) == BRAD_QUARTER);
    CHECK(fx_atan2(Z, N) == BRAD_HALF);
    CHECK(fx_atan2(N, Z) == BRAD_QUARTER + BRAD_HALF);
    CHECK(fx_atan2(Z, Z) == brad(0));   // defined, not an error

    // atan2(dir(a)) must return a. This is the round-trip the aim/facing code
    // depends on, and it exercises the vectoring-mode rescale on unit-length
    // inputs where precision is tightest.
    i32 worst = 0;
    for (u64 i = 0; i < 100000; ++i) {
        const brad a = brad(static_cast<u32>(i * 42949ULL));
        const fx2 d = fx_dir(a);
        const brad back = fx_angle_of(d);
        const i32 err = brad_delta(back, a);
        const i32 mag = err < 0 ? -err : err;
        if (mag > worst) worst = mag;
    }
    std::printf("       worst atan2(dir(a)) round-trip = %d brad (%.3e turns)\n",
                worst, static_cast<double>(worst) / 4294967296.0);
    CHECK(worst < 4096);

    // Scale invariance: the angle of a vector must not depend on its length.
    Rng rng(555);
    for (int i = 0; i < 20000; ++i) {
        const fx2 v{fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30),
                    fx::from_raw(static_cast<i64>(rng.next_u64()) >> 30)};
        if (v.x.raw == 0 && v.y.raw == 0) continue;
        const fx2 big{fx::from_raw(asl(v.x.raw, 8)), fx::from_raw(asl(v.y.raw, 8))};
        const i32 err = brad_delta(fx_angle_of(v), fx_angle_of(big));
        CHECK((err < 0 ? -err : err) < 4096);
    }
}

// ---------------------------------------------------------------------- rng

static void test_rng() {
    std::printf("rng: reproducibility and bounded draws\n");
    Rng a(1234, 7), b(1234, 7);
    for (int i = 0; i < 1000; ++i) CHECK(a.next_u32() == b.next_u32());

    Rng c(1234, 7), d(1234, 8);   // different streams must not coincide
    int same = 0;
    for (int i = 0; i < 1000; ++i) same += (c.next_u32() == d.next_u32()) ? 1 : 0;
    CHECK(same < 10);

    Rng e(99);
    for (int i = 0; i < 10000; ++i) {
        const u32 v = e.next_below(7);
        CHECK(v < 7);
    }
    CHECK(Rng(1).next_below(0) == 0);

    Rng f(3);
    for (int i = 0; i < 10000; ++i) {
        const fx u = f.next_unit();
        CHECK(u >= fx::zero() && u < fx::one());
    }
}

// --------------------------------------------------------------------- hash

static void test_state_hash() {
    std::printf("hash: order sensitivity and stable known value\n");
    StateHasher h1, h2;
    h1.fxv(fx::from_int(1)); h1.fxv(fx::from_int(2));
    h2.fxv(fx::from_int(2)); h2.fxv(fx::from_int(1));
    CHECK(h1.value() != h2.value());

    StateHasher h3;
    CHECK(h3.value() == StateHasher::OFFSET_BASIS);
    h3.u32v(0x01020304u);
    // Pinned. Byte order is written out explicitly rather than memcpy'd, so
    // this literal must hold on a big-endian host too -- if it ever changes,
    // every recorded golden trace in the repository is invalidated at the same
    // moment, and that should require deleting this line on purpose.
    CHECK(h3.value() == 0xB345225E3644EDB5ULL);

    // Padding must not reach the hash: feeding the same values through typed
    // helpers in the same order must reproduce the value exactly.
    StateHasher h4;
    h4.byte(0x04); h4.byte(0x03); h4.byte(0x02); h4.byte(0x01);
    CHECK(h4.value() == h3.value());
}

int main() {
    std::printf("== core/math test suite ==\n");
    test_fixed_basics();
    test_mirror_symmetry();
    test_saturation();
    test_sqrt();
    test_lerp_endpoints();
    test_vec();
    test_trig_exact_points();
    test_trig_accuracy();
    test_atan2();
    test_rng();
    test_state_hash();
    std::printf("== %d checks, %d failures ==\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
