//
// CORDIC sin/cos/atan2 in fixed point.
//
// Internally the angle accumulator `z` runs at a FINER scale than the brad type
// it is called with: a full turn is 2^62 rather than 2^32. At the public 2^32
// scale, one step is 1.46e-9 rad, and the CORDIC rotation for i >= 30 is smaller
// than that -- those iterations would quantise to zero and be wasted, capping
// precision well above what Q32.32 can hold. Carrying 30 extra fractional bits
// internally lets all 32 iterations contribute.
//
// The atan table below was generated at 80 decimal digits (Python `decimal`,
// Machin's formula for pi, Taylor series for atan) and is baked in as integer
// literals, so it is identical on every target by construction. Entry 0 is
// atan(1) = pi/4, which is exactly one eighth of a turn = 2^59; that it comes
// out exact is a useful check that the scale is right.
//
#include "dnp/math/trig.hpp"

namespace dnp {
namespace {

// ATAN_B62[i] = atan(2^-i) expressed in turns * 2^62.
constexpr i64 ATAN_B62[32] = {
    576460752303423488LL, 340304653033718298LL, 179807632645220259LL,  91273161881380487LL,
     45813697873323707LL,  22929182573009054LL,  11467389120678282LL,   5734044481687724LL,
      2867065987018958LL,   1433538461969102LL,    716769914547871LL,    358385042719534LL,
       179192532040472LL,     89596267355325LL,     44798133844548LL,     22399066943135LL,
        11199533474175LL,      5599766737413LL,      2799883368747LL,      1399941684379LL,
          699970842190LL,       349985421095LL,       174992710548LL,        87496355274LL,
           43748177637LL,        21874088818LL,        10937044409LL,         5468522205LL,
            2734261102LL,        1367130551LL,          683565276LL,          341782638LL,
};

constexpr int CORDIC_ITERS = 32;

// 1 / prod(sqrt(1 + 2^-2i)) over all 32 iterations, in Q32.32. Pre-seeding x
// with this cancels the gain the rotation accumulates, so no post-scale is
// needed and unit vectors come out unit length.
constexpr i64 CORDIC_INV_GAIN = 2608131496LL;

}  // namespace

void fx_sincos(brad a, fx& out_sin, fx& out_cos) noexcept {
    // Exact at the four cardinal angles. CORDIC converges to within a couple of
    // ulp but never lands exactly on 0 or 1, and axis-aligned directions are
    // everywhere in a sector-based engine: rotate(v, 0) must return v itself,
    // and a wall normal of exactly +X must have exactly zero Y component or
    // wall-sliding would leak a sliver of velocity through it.
    if ((a.raw & 0x3FFFFFFFu) == 0) {
        switch (a.raw >> 30) {
            case 0:  out_sin =  fx::zero(); out_cos =  fx::one();  return;
            case 1:  out_sin =  fx::one();  out_cos =  fx::zero(); return;
            case 2:  out_sin =  fx::zero(); out_cos = -fx::one();  return;
            default: out_sin = -fx::one();  out_cos =  fx::zero(); return;
        }
    }

    // Fold into the first quadrant. The remaining angle is at most a quarter
    // turn (2^60), and the atan table sums to 1.2795e18 > 2^60 = 1.1529e18, so
    // every reduced angle is inside CORDIC's convergence range.
    const u32 quadrant = a.raw >> 30;
    i64 z = static_cast<i64>(a.raw & 0x3FFFFFFFu) << 30;

    i64 x = CORDIC_INV_GAIN;
    i64 y = 0;

    for (int i = 0; i < CORDIC_ITERS; ++i) {
        const i64 dx = asr(x, i);
        const i64 dy = asr(y, i);
        if (z >= 0) {
            const i64 nx = x - dy;
            y = y + dx;
            x = nx;
            z -= ATAN_B62[i];
        } else {
            const i64 nx = x + dy;
            y = y - dx;
            x = nx;
            z += ATAN_B62[i];
        }
    }

    // x = cos(reduced), y = sin(reduced); map back out of the first quadrant.
    switch (quadrant) {
        case 0:  out_cos = fx::from_raw( x); out_sin = fx::from_raw( y); break;
        case 1:  out_cos = fx::from_raw(-y); out_sin = fx::from_raw( x); break;
        case 2:  out_cos = fx::from_raw(-x); out_sin = fx::from_raw(-y); break;
        default: out_cos = fx::from_raw( y); out_sin = fx::from_raw(-x); break;
    }
}

fx fx_sin(brad a) noexcept { fx s, c; fx_sincos(a, s, c); return s; }
fx fx_cos(brad a) noexcept { fx s, c; fx_sincos(a, s, c); return c; }

brad fx_atan2(fx y, fx x) noexcept {
    // Exact on the axes, for the same reason fx_sincos is exact at the cardinal
    // angles. This also covers atan2(0, 0), which is defined here to be 0.
    if (y.raw == 0) return x.raw >= 0 ? brad(0) : BRAD_HALF;
    if (x.raw == 0) return y.raw > 0 ? BRAD_QUARTER : BRAD_QUARTER + BRAD_HALF;

    i64 vx = x.raw;
    i64 vy = y.raw;
    u32 base = 0;

    // CORDIC vectoring only converges for vectors in the right half-plane, so
    // reflect through the origin first and add half a turn to the result.
    if (vx < 0) {
        // Reflecting I64_MIN would overflow; one ulp of nudge is far below the
        // angular resolution and keeps the operation total.
        if (vx == I64_MIN) vx = -I64_MAX;
        if (vy == I64_MIN) vy = -I64_MAX;
        vx = -vx;
        vy = -vy;
        base = 0x80000000u;
    }

    // Angle is scale-invariant, so rescale the vector into a fixed window
    // before iterating. This both prevents the ~1.647x CORDIC growth from
    // overflowing on large inputs and recovers full precision on small ones --
    // without it, a short vector would iterate on only a handful of significant
    // bits and the answer would be far coarser than Q32.32 can represent.
    {
        i64 ax = vx < 0 ? -vx : vx;
        i64 ay = vy < 0 ? -vy : vy;
        i64 m  = ax > ay ? ax : ay;
        constexpr i64 LO = static_cast<i64>(1) << 59;
        constexpr i64 HI = static_cast<i64>(1) << 60;
        while (m < LO) { vx <<= 1; vy <<= 1; m <<= 1; }
        while (m >= HI) { vx = asr(vx, 1); vy = asr(vy, 1); m >>= 1; }
    }

    i64 z = 0;
    for (int i = 0; i < CORDIC_ITERS; ++i) {
        const i64 dx = asr(vx, i);
        const i64 dy = asr(vy, i);
        if (vy > 0) {
            const i64 nx = vx + dy;
            vy = vy - dx;
            vx = nx;
            z += ATAN_B62[i];
        } else {
            const i64 nx = vx - dy;
            vy = vy + dx;
            vx = nx;
            z -= ATAN_B62[i];
        }
    }

    // Drop from the internal 2^62 scale back to the public 2^32 one. The u32
    // cast wraps for negative angles, which is precisely the binary-radian
    // representation of a negative angle.
    return brad(base + static_cast<u32>(static_cast<i64>(asr(z, 30))));
}

}  // namespace dnp
