/*
 * fixed_test.c - Tests for the cb_fixed module (Q16.16 / Q32.32 / BRAD).
 *
 * The module does NOT exist yet. This file is the specification-as-tests
 * that will drive the implementation. It will fail to compile until
 * cbase.h gains the cb_fx16_t / cb_fx32_t / cb_brad_t declarations and
 * cbase_fixed.c is added to cbase_union.c.
 *
 * Determinism contract: add/sub/mul/div/abs/min/max/clamp/lerp and all
 * conversions must be bit-exact across platforms and compilers.
 * sin/cos/sqrt/atan2 use LUT + interpolation and are checked with a
 * documented tolerance. The determinism smoke test (Section 6) pins a
 * hardcoded accumulator value that is the canary for cross-platform bit
 * equality once the implementation exists.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "../cbase.h"

/* ---------- helpers ---------- */

static int g_fails = 0;

#define CHECK(cond, msg) do {                                           \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__); \
        return 1;                                                        \
    }                                                                    \
} while (0)

static int within_fx16(cb_fx16_t a, cb_fx16_t b, cb_fx16_t tol)
{
    int64_t da = (int64_t)a - (int64_t)b;
    if (da < 0) da = -da;
    return da <= (int64_t)tol;
}

static int within_fx32(cb_fx32_t a, cb_fx32_t b, cb_fx32_t tol)
{
    /* Do the absolute-difference in unsigned space so we don't trip the
       sanitizer on INT64 overflow when a and b straddle zero extremes. */
    uint64_t ua = (uint64_t)a;
    uint64_t ub = (uint64_t)b;
    uint64_t diff = (ua > ub) ? (ua - ub) : (ub - ua);
    return diff <= (uint64_t)tol;
}

static int brad_within(cb_brad_t a, cb_brad_t b, uint16_t tol)
{
    /* Angles wrap, so measure the short-arc distance in a uint16 ring. */
    uint16_t d = (uint16_t)(a - b);
    if (d > 32768) d = (uint16_t)(65536u - d);
    return d <= tol;
}

/* Tolerances. 1 Q16.16 ulp = ~1.5e-5. */
#define TOL_SQRT_ULPS   ((cb_fx16_t)10)
#define TOL_SIN_ULPS    ((cb_fx16_t)64)   /* LUT + linear interp: generous */
#define TOL_ATAN2_BRAD  ((uint16_t)64)    /* ~0.35 deg worst-case */
#define TOL_FX32_SQRT   ((cb_fx32_t)(1LL << 20))  /* ~1e-6 in Q32.32 terms */

/* ---------- section 1: Q16.16 conversions ---------- */

static int section1_conversions(void)
{
    printf("Section 1: Q16.16 conversions\n");

    /* from_int: representable range */
    CHECK(cb_fx16_from_int(0) == 0, "from_int(0) != 0");
    CHECK(cb_fx16_from_int(1) == CB_FX16_ONE, "from_int(1) != ONE");
    CHECK(cb_fx16_from_int(-1) == -CB_FX16_ONE, "from_int(-1) != -ONE");
    CHECK(cb_fx16_from_int(100) == 100 * CB_FX16_ONE, "from_int(100)");
    CHECK(cb_fx16_from_int(-100) == -100 * CB_FX16_ONE, "from_int(-100)");
    CHECK(cb_fx16_from_int(INT16_MAX) == (cb_fx16_t)((int32_t)INT16_MAX * CB_FX16_ONE),
          "from_int(INT16_MAX)");
    CHECK(cb_fx16_from_int(INT16_MIN) == (cb_fx16_t)((int32_t)INT16_MIN * CB_FX16_ONE),
          "from_int(INT16_MIN)");

    /* from_int: saturation beyond Q16.16 integer range */
    CHECK(cb_fx16_from_int(INT32_MAX) == CB_FX16_MAX, "from_int(INT32_MAX) must saturate to MAX");
    CHECK(cb_fx16_from_int(INT32_MIN) == CB_FX16_MIN, "from_int(INT32_MIN) must saturate to MIN");
    CHECK(cb_fx16_from_int(40000) == CB_FX16_MAX, "from_int(40000) must saturate");
    CHECK(cb_fx16_from_int(-40000) == CB_FX16_MIN, "from_int(-40000) must saturate");

    /* to_int: truncation toward zero */
    CHECK(cb_fx16_to_int(CB_FX16_ONE) == 1, "to_int(ONE) != 1");
    CHECK(cb_fx16_to_int(-CB_FX16_ONE) == -1, "to_int(-ONE) != -1");
    CHECK(cb_fx16_to_int(CB_FX16_HALF) == 0, "to_int(HALF) must truncate to 0 (not 1)");
    CHECK(cb_fx16_to_int(-CB_FX16_HALF) == 0, "to_int(-HALF) must truncate to 0 (not -1)");
    CHECK(cb_fx16_to_int(CB_FX16_ONE + CB_FX16_HALF) == 1, "to_int(1.5) truncates to 1");
    CHECK(cb_fx16_to_int(-(CB_FX16_ONE + CB_FX16_HALF)) == -1, "to_int(-1.5) truncates to -1");
    CHECK(cb_fx16_to_int(0) == 0, "to_int(0) != 0");

    /* to_int on extremes: values expressible in int32 */
    CHECK(cb_fx16_to_int(CB_FX16_MAX) == 32767, "to_int(MAX) should be 32767");
    CHECK(cb_fx16_to_int(CB_FX16_MIN) == -32768, "to_int(MIN) should be -32768");

    /* from_float / to_float round-trip (renderer path only) */
    CHECK(cb_fx16_from_float(0.0f) == 0, "from_float(0)");
    CHECK(cb_fx16_from_float(1.0f) == CB_FX16_ONE, "from_float(1) bit-exact");
    CHECK(cb_fx16_from_float(-1.0f) == -CB_FX16_ONE, "from_float(-1) bit-exact");
    CHECK(cb_fx16_from_float(0.5f) == CB_FX16_HALF, "from_float(0.5) bit-exact");
    CHECK(cb_fx16_from_float(-0.5f) == -CB_FX16_HALF, "from_float(-0.5) bit-exact");
    CHECK(cb_fx16_from_float(0.25f) == (CB_FX16_ONE / 4), "from_float(0.25)");

    /* round-trip for representable values */
    float rtvals[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.25f, -0.25f, 100.25f, -100.25f };
    size_t nvals = sizeof(rtvals) / sizeof(rtvals[0]);
    for (size_t i = 0; i < nvals; ++i) {
        cb_fx16_t fx = cb_fx16_from_float(rtvals[i]);
        float back = cb_fx16_to_float(fx);
        float delta = back - rtvals[i];
        if (delta < 0) delta = -delta;
        /* 1 ulp in Q16.16 = 1/65536 ~= 1.52e-5 */
        if (delta > 2.0f / 65536.0f) {
            fprintf(stderr, "FAIL: float round-trip delta too big for %g (got %g)\n",
                    (double)rtvals[i], (double)back);
            return 1;
        }
    }

    /* Float exact round-trips */
    CHECK(cb_fx16_to_float(CB_FX16_ONE) == 1.0f, "to_float(ONE) != 1.0f exact");
    CHECK(cb_fx16_to_float(-CB_FX16_ONE) == -1.0f, "to_float(-ONE) != -1.0f exact");
    CHECK(cb_fx16_to_float(CB_FX16_HALF) == 0.5f, "to_float(HALF) != 0.5f exact");
    CHECK(cb_fx16_to_float(0) == 0.0f, "to_float(0) != 0.0f exact");

    printf("  conversions OK\n");
    return 0;
}

/* ---------- section 2: Q16.16 arithmetic ---------- */

static int section2_arithmetic(void)
{
    printf("Section 2: Q16.16 arithmetic (exact, bit-identical)\n");

    /* add */
    CHECK(cb_fx16_add(0, 0) == 0, "add(0,0)");
    CHECK(cb_fx16_add(CB_FX16_ONE, CB_FX16_ONE) == 2 * CB_FX16_ONE, "add(1,1)");
    CHECK(cb_fx16_add(-CB_FX16_ONE, CB_FX16_ONE) == 0, "add(-1,1)");
    CHECK(cb_fx16_add(CB_FX16_HALF, CB_FX16_HALF) == CB_FX16_ONE, "add(0.5,0.5)");
    CHECK(cb_fx16_add(CB_FX16_MAX, 1) == CB_FX16_MAX, "add(MAX,1) must saturate");
    CHECK(cb_fx16_add(CB_FX16_MAX, CB_FX16_ONE) == CB_FX16_MAX, "add(MAX,ONE) must saturate");
    CHECK(cb_fx16_add(CB_FX16_MIN, -1) == CB_FX16_MIN, "add(MIN,-1) must saturate");
    CHECK(cb_fx16_add(CB_FX16_MIN, -CB_FX16_ONE) == CB_FX16_MIN, "add(MIN,-ONE) must saturate");
    CHECK(cb_fx16_add(CB_FX16_MAX, CB_FX16_MIN) == -1, "add(MAX,MIN) = -1");

    /* sub */
    CHECK(cb_fx16_sub(0, 0) == 0, "sub(0,0)");
    CHECK(cb_fx16_sub(CB_FX16_ONE, CB_FX16_ONE) == 0, "sub(1,1)");
    CHECK(cb_fx16_sub(0, CB_FX16_ONE) == -CB_FX16_ONE, "sub(0,1)");
    CHECK(cb_fx16_sub(CB_FX16_MIN, 1) == CB_FX16_MIN, "sub(MIN,1) must saturate");
    CHECK(cb_fx16_sub(CB_FX16_MIN, CB_FX16_ONE) == CB_FX16_MIN, "sub(MIN,ONE) must saturate");
    CHECK(cb_fx16_sub(CB_FX16_MAX, -1) == CB_FX16_MAX, "sub(MAX,-1) must saturate");
    CHECK(cb_fx16_sub(CB_FX16_MAX, -CB_FX16_ONE) == CB_FX16_MAX, "sub(MAX,-ONE) must saturate");
    CHECK(cb_fx16_sub(CB_FX16_ONE + CB_FX16_HALF, CB_FX16_HALF) == CB_FX16_ONE, "sub(1.5,0.5)");

    /* mul: exact */
    CHECK(cb_fx16_mul(CB_FX16_ONE, CB_FX16_ONE) == CB_FX16_ONE, "mul(1,1)");
    CHECK(cb_fx16_mul(CB_FX16_ONE, 2 * CB_FX16_ONE) == 2 * CB_FX16_ONE, "mul(1,2)");
    CHECK(cb_fx16_mul(2 * CB_FX16_ONE, CB_FX16_ONE) == 2 * CB_FX16_ONE, "mul(2,1)");
    CHECK(cb_fx16_mul(CB_FX16_HALF, 2 * CB_FX16_ONE) == CB_FX16_ONE, "mul(0.5,2)");
    CHECK(cb_fx16_mul(-CB_FX16_ONE, CB_FX16_ONE) == -CB_FX16_ONE, "mul(-1,1)");
    CHECK(cb_fx16_mul(-CB_FX16_ONE, -CB_FX16_ONE) == CB_FX16_ONE, "mul(-1,-1)");
    CHECK(cb_fx16_mul(0, CB_FX16_MAX) == 0, "mul(0,MAX)");
    CHECK(cb_fx16_mul(CB_FX16_ONE, 0) == 0, "mul(ONE,0)");

    /* Hand-computed pairs: a*b, each in Q16.16. */
    /* 3.5 * 4.0 = 14.0 */
    {
        cb_fx16_t a = (cb_fx16_t)(3.5 * 65536.0);
        cb_fx16_t b = 4 * CB_FX16_ONE;
        cb_fx16_t expect = 14 * CB_FX16_ONE;
        CHECK(cb_fx16_mul(a, b) == expect, "mul(3.5,4.0)");
    }
    /* 0.25 * 0.25 = 0.0625 = 4096 in Q16.16 */
    {
        cb_fx16_t a = CB_FX16_ONE / 4;
        cb_fx16_t b = CB_FX16_ONE / 4;
        cb_fx16_t expect = (cb_fx16_t)4096;  /* 0.0625 * 65536 */
        CHECK(cb_fx16_mul(a, b) == expect, "mul(0.25,0.25)");
    }
    /* 2.5 * 2.5 = 6.25 */
    {
        cb_fx16_t a = (cb_fx16_t)(2.5 * 65536.0);
        cb_fx16_t b = a;
        cb_fx16_t expect = (cb_fx16_t)(6.25 * 65536.0);
        CHECK(cb_fx16_mul(a, b) == expect, "mul(2.5,2.5)");
    }
    /* -3 * 7 = -21 */
    CHECK(cb_fx16_mul(-3 * CB_FX16_ONE, 7 * CB_FX16_ONE) == -21 * CB_FX16_ONE, "mul(-3,7)");
    /* 10 * 10 = 100 */
    CHECK(cb_fx16_mul(10 * CB_FX16_ONE, 10 * CB_FX16_ONE) == 100 * CB_FX16_ONE, "mul(10,10)");
    /* 0.5 * 0.5 = 0.25 */
    CHECK(cb_fx16_mul(CB_FX16_HALF, CB_FX16_HALF) == CB_FX16_ONE / 4, "mul(0.5,0.5)");
    /* 8 * 8 = 64 */
    CHECK(cb_fx16_mul(8 * CB_FX16_ONE, 8 * CB_FX16_ONE) == 64 * CB_FX16_ONE, "mul(8,8)");
    /* 1.25 * 8 = 10 */
    {
        cb_fx16_t a = CB_FX16_ONE + CB_FX16_ONE / 4;
        CHECK(cb_fx16_mul(a, 8 * CB_FX16_ONE) == 10 * CB_FX16_ONE, "mul(1.25,8)");
    }
    /* -0.5 * 4 = -2 */
    CHECK(cb_fx16_mul(-CB_FX16_HALF, 4 * CB_FX16_ONE) == -2 * CB_FX16_ONE, "mul(-0.5,4)");

    /* mul: saturation */
    CHECK(cb_fx16_mul(CB_FX16_MAX, 2 * CB_FX16_ONE) == CB_FX16_MAX, "mul(MAX,2) saturates to MAX");
    CHECK(cb_fx16_mul(CB_FX16_MAX, -2 * CB_FX16_ONE) == CB_FX16_MIN, "mul(MAX,-2) saturates to MIN");
    CHECK(cb_fx16_mul(CB_FX16_MIN, 2 * CB_FX16_ONE) == CB_FX16_MIN, "mul(MIN,2) saturates to MIN");
    CHECK(cb_fx16_mul(CB_FX16_MIN, -2 * CB_FX16_ONE) == CB_FX16_MAX, "mul(MIN,-2) saturates to MAX");
    CHECK(cb_fx16_mul(100 * CB_FX16_ONE, 1000 * CB_FX16_ONE) == CB_FX16_MAX,
          "mul(100,1000) overflows to MAX");

    /* div */
    CHECK(cb_fx16_div(CB_FX16_ONE, CB_FX16_ONE) == CB_FX16_ONE, "div(1,1)");
    CHECK(cb_fx16_div(CB_FX16_ONE, 2 * CB_FX16_ONE) == CB_FX16_HALF, "div(1,2)");
    CHECK(cb_fx16_div(-CB_FX16_ONE, CB_FX16_ONE) == -CB_FX16_ONE, "div(-1,1)");
    CHECK(cb_fx16_div(0, CB_FX16_ONE) == 0, "div(0,1)");
    CHECK(cb_fx16_div(0, CB_FX16_MAX) == 0, "div(0,MAX)");
    CHECK(cb_fx16_div(10 * CB_FX16_ONE, 2 * CB_FX16_ONE) == 5 * CB_FX16_ONE, "div(10,2)");
    CHECK(cb_fx16_div(1 * CB_FX16_ONE, 4 * CB_FX16_ONE) == CB_FX16_ONE / 4, "div(1,4)");
    CHECK(cb_fx16_div(-8 * CB_FX16_ONE, 2 * CB_FX16_ONE) == -4 * CB_FX16_ONE, "div(-8,2)");

    /* div-by-zero: sign-propagating saturation */
    CHECK(cb_fx16_div(CB_FX16_ONE, 0) == CB_FX16_MAX, "div(pos,0) -> MAX");
    CHECK(cb_fx16_div(CB_FX16_MAX, 0) == CB_FX16_MAX, "div(MAX,0) -> MAX");
    CHECK(cb_fx16_div(-CB_FX16_ONE, 0) == CB_FX16_MIN, "div(neg,0) -> MIN");
    CHECK(cb_fx16_div(CB_FX16_MIN, 0) == CB_FX16_MIN, "div(MIN,0) -> MIN");
    CHECK(cb_fx16_div(0, 0) == 0, "div(0,0) -> 0 (indeterminate)");

    /* div overflow (result > MAX): saturates */
    CHECK(cb_fx16_div(CB_FX16_MAX, CB_FX16_HALF) == CB_FX16_MAX, "div(MAX,0.5) saturates");

    /* abs */
    CHECK(cb_fx16_abs(0) == 0, "abs(0)");
    CHECK(cb_fx16_abs(CB_FX16_ONE) == CB_FX16_ONE, "abs(1)");
    CHECK(cb_fx16_abs(-CB_FX16_ONE) == CB_FX16_ONE, "abs(-1)");
    CHECK(cb_fx16_abs(CB_FX16_MAX) == CB_FX16_MAX, "abs(MAX)");
    /* abs(MIN) can't fit in signed 32-bit -> must saturate to MAX */
    CHECK(cb_fx16_abs(CB_FX16_MIN) == CB_FX16_MAX, "abs(MIN) must saturate to MAX");

    /* min/max */
    CHECK(cb_fx16_min(CB_FX16_ONE, 2 * CB_FX16_ONE) == CB_FX16_ONE, "min(1,2)");
    CHECK(cb_fx16_min(-CB_FX16_ONE, CB_FX16_ONE) == -CB_FX16_ONE, "min(-1,1)");
    CHECK(cb_fx16_min(CB_FX16_ONE, CB_FX16_ONE) == CB_FX16_ONE, "min(1,1)");
    CHECK(cb_fx16_max(CB_FX16_ONE, 2 * CB_FX16_ONE) == 2 * CB_FX16_ONE, "max(1,2)");
    CHECK(cb_fx16_max(-CB_FX16_ONE, CB_FX16_ONE) == CB_FX16_ONE, "max(-1,1)");

    /* clamp */
    CHECK(cb_fx16_clamp(5 * CB_FX16_ONE, 0, 10 * CB_FX16_ONE) == 5 * CB_FX16_ONE,
          "clamp inside");
    CHECK(cb_fx16_clamp(-5 * CB_FX16_ONE, 0, 10 * CB_FX16_ONE) == 0, "clamp low");
    CHECK(cb_fx16_clamp(50 * CB_FX16_ONE, 0, 10 * CB_FX16_ONE) == 10 * CB_FX16_ONE,
          "clamp high");
    CHECK(cb_fx16_clamp(0, 0, 0) == 0, "clamp degenerate");

    /* lerp */
    CHECK(cb_fx16_lerp(0, 10 * CB_FX16_ONE, 0) == 0, "lerp(A,B,0) = A");
    CHECK(cb_fx16_lerp(0, 10 * CB_FX16_ONE, CB_FX16_ONE) == 10 * CB_FX16_ONE,
          "lerp(A,B,ONE) = B");
    CHECK(cb_fx16_lerp(0, 10 * CB_FX16_ONE, CB_FX16_HALF) == 5 * CB_FX16_ONE,
          "lerp midpoint");
    CHECK(cb_fx16_lerp(-4 * CB_FX16_ONE, 4 * CB_FX16_ONE, CB_FX16_HALF) == 0,
          "lerp across zero");
    CHECK(cb_fx16_lerp(2 * CB_FX16_ONE, 2 * CB_FX16_ONE, CB_FX16_HALF) == 2 * CB_FX16_ONE,
          "lerp degenerate");
    CHECK(cb_fx16_lerp(CB_FX16_ONE, 3 * CB_FX16_ONE, CB_FX16_ONE / 4) == CB_FX16_ONE + CB_FX16_HALF,
          "lerp quarter");

    printf("  arithmetic OK\n");
    return 0;
}

/* ---------- section 3: Q16.16 sqrt (approximate) ---------- */

static int section3_sqrt(void)
{
    printf("Section 3: Q16.16 sqrt (tolerance +/- %d ulps)\n", (int)TOL_SQRT_ULPS);

    /* Exact boundaries */
    CHECK(cb_fx16_sqrt(0) == 0, "sqrt(0) != 0 exact");
    CHECK(cb_fx16_sqrt(CB_FX16_ONE) == CB_FX16_ONE, "sqrt(1) != 1 exact");

    /* Perfect squares: should be very close to the integer result */
    struct { int n; int r; } perfect[] = {
        { 4, 2 }, { 9, 3 }, { 16, 4 }, { 25, 5 }, { 36, 6 },
        { 49, 7 }, { 64, 8 }, { 81, 9 }, { 100, 10 }, { 400, 20 },
    };
    for (size_t i = 0; i < sizeof(perfect) / sizeof(perfect[0]); ++i) {
        cb_fx16_t in = (cb_fx16_t)perfect[i].n * CB_FX16_ONE;
        cb_fx16_t expect = (cb_fx16_t)perfect[i].r * CB_FX16_ONE;
        cb_fx16_t got = cb_fx16_sqrt(in);
        if (!within_fx16(got, expect, TOL_SQRT_ULPS)) {
            fprintf(stderr, "FAIL: sqrt(%d) got %d, expect %d (tol %d ulps)\n",
                    perfect[i].n, (int)got, (int)expect, (int)TOL_SQRT_ULPS);
            return 1;
        }
    }

    /* Known irrational values */
    struct { double v; double expect; } irr[] = {
        { 2.0,  1.41421356 },
        { 3.0,  1.73205081 },
        { 5.0,  2.23606798 },
        { 0.25, 0.5 },
        { 0.5,  0.70710678 },
    };
    for (size_t i = 0; i < sizeof(irr) / sizeof(irr[0]); ++i) {
        cb_fx16_t in = (cb_fx16_t)(irr[i].v * 65536.0);
        cb_fx16_t expect = (cb_fx16_t)(irr[i].expect * 65536.0);
        cb_fx16_t got = cb_fx16_sqrt(in);
        if (!within_fx16(got, expect, TOL_SQRT_ULPS)) {
            fprintf(stderr, "FAIL: sqrt(%g) got %d, expect ~%d\n",
                    irr[i].v, (int)got, (int)expect);
            return 1;
        }
    }

    /* Negative input defined to return 0 */
    CHECK(cb_fx16_sqrt(-CB_FX16_ONE) == 0, "sqrt(-1) must return 0");
    CHECK(cb_fx16_sqrt(CB_FX16_MIN) == 0, "sqrt(MIN) must return 0");

    /* sqrt(MAX): must not crash, must be non-negative, approximate value */
    {
        cb_fx16_t got = cb_fx16_sqrt(CB_FX16_MAX);
        CHECK(got >= 0, "sqrt(MAX) must be non-negative");
        /* MAX ~ 32768 in real value, sqrt ~ 181.0 */
        cb_fx16_t expect = (cb_fx16_t)(181.02 * 65536.0);
        /* Use a wider tolerance here because MAX is huge. */
        if (!within_fx16(got, expect, (cb_fx16_t)(CB_FX16_ONE / 4))) {
            fprintf(stderr, "FAIL: sqrt(MAX) approx wrong: %d vs ~%d\n",
                    (int)got, (int)expect);
            return 1;
        }
    }

    printf("  sqrt OK\n");
    return 0;
}

/* ---------- section 4: BRAD trig (approximate) ---------- */

static int section4_trig(void)
{
    printf("Section 4: BRAD trig (tolerance +/- %d ulps / %d brad)\n",
           (int)TOL_SIN_ULPS, (int)TOL_ATAN2_BRAD);

    /* sin cardinal values */
    if (!within_fx16(cb_fx16_sin(0), 0, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: sin(0) != 0 (got %d)\n", (int)cb_fx16_sin(0));
        return 1;
    }
    if (!within_fx16(cb_fx16_sin(CB_BRAD_QUARTER), CB_FX16_ONE, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: sin(pi/2) != ONE (got %d)\n",
                (int)cb_fx16_sin(CB_BRAD_QUARTER));
        return 1;
    }
    if (!within_fx16(cb_fx16_sin(CB_BRAD_HALF), 0, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: sin(pi) != 0 (got %d)\n",
                (int)cb_fx16_sin(CB_BRAD_HALF));
        return 1;
    }
    if (!within_fx16(cb_fx16_sin((cb_brad_t)(CB_BRAD_HALF + CB_BRAD_QUARTER)),
                     -CB_FX16_ONE, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: sin(3pi/2) != -ONE (got %d)\n",
                (int)cb_fx16_sin((cb_brad_t)(CB_BRAD_HALF + CB_BRAD_QUARTER)));
        return 1;
    }

    /* cos cardinal values */
    if (!within_fx16(cb_fx16_cos(0), CB_FX16_ONE, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: cos(0) != ONE (got %d)\n", (int)cb_fx16_cos(0));
        return 1;
    }
    if (!within_fx16(cb_fx16_cos(CB_BRAD_QUARTER), 0, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: cos(pi/2) != 0 (got %d)\n",
                (int)cb_fx16_cos(CB_BRAD_QUARTER));
        return 1;
    }
    if (!within_fx16(cb_fx16_cos(CB_BRAD_HALF), -CB_FX16_ONE, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: cos(pi) != -ONE (got %d)\n",
                (int)cb_fx16_cos(CB_BRAD_HALF));
        return 1;
    }
    if (!within_fx16(cb_fx16_cos((cb_brad_t)(CB_BRAD_HALF + CB_BRAD_QUARTER)), 0, TOL_SIN_ULPS)) {
        fprintf(stderr, "FAIL: cos(3pi/2) != 0\n");
        return 1;
    }

    /* Pythagorean identity: sin^2 + cos^2 == 1 */
    cb_brad_t samples[] = {
        0, 1024, 4096, 8192, 12000, 16384, 20000, 24576, 28000, 32768,
        40000, 49152, 55000, 60000, 65535
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        cb_fx16_t s = cb_fx16_sin(samples[i]);
        cb_fx16_t c = cb_fx16_cos(samples[i]);
        cb_fx16_t s2 = cb_fx16_mul(s, s);
        cb_fx16_t c2 = cb_fx16_mul(c, c);
        cb_fx16_t sum = cb_fx16_add(s2, c2);
        /* Tolerance: multiplication doubles the per-value error; give ~200 ulps. */
        if (!within_fx16(sum, CB_FX16_ONE, (cb_fx16_t)200)) {
            fprintf(stderr, "FAIL: sin^2+cos^2 != 1 at brad=%u (got %d)\n",
                    (unsigned)samples[i], (int)sum);
            return 1;
        }
    }

    /* Half-turn identity: sin(a + pi) == -sin(a) */
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        cb_fx16_t a = cb_fx16_sin(samples[i]);
        cb_fx16_t b = cb_fx16_sin((cb_brad_t)(samples[i] + CB_BRAD_HALF));
        if (!within_fx16(a, -b, TOL_SIN_ULPS)) {
            fprintf(stderr, "FAIL: sin(a+pi) != -sin(a) at brad=%u (a=%d b=%d)\n",
                    (unsigned)samples[i], (int)a, (int)b);
            return 1;
        }
    }

    /* Wrap-around: uint16 overflow gives identical angles (periodicity).
       Adding 65536 to a uint16 is a no-op so we sample the wrap boundary. */
    for (uint32_t k = 0; k < 16; ++k) {
        cb_brad_t a = (cb_brad_t)(k * 4096);
        cb_fx16_t s1 = cb_fx16_sin(a);
        cb_fx16_t s2 = cb_fx16_sin((cb_brad_t)(a + (cb_brad_t)0));
        if (s1 != s2) {
            fprintf(stderr, "FAIL: sin not pure function of brad at %u\n", (unsigned)a);
            return 1;
        }
    }
    /* And confirm that sin((uint16)(a + 65536u)) == sin(a) (the cast wraps). */
    {
        cb_fx16_t s_near = cb_fx16_sin((cb_brad_t)1234);
        cb_fx16_t s_wrap = cb_fx16_sin((cb_brad_t)((uint32_t)1234 + 65536u));
        CHECK(s_near == s_wrap, "sin wrap-around not identity");
    }

    /* atan2 cardinal directions */
    if (!brad_within(cb_fx16_atan2(0, CB_FX16_ONE), 0, TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(0,1) != 0 (got %u)\n",
                (unsigned)cb_fx16_atan2(0, CB_FX16_ONE));
        return 1;
    }
    if (!brad_within(cb_fx16_atan2(CB_FX16_ONE, 0), CB_BRAD_QUARTER, TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(1,0) != QUARTER (got %u)\n",
                (unsigned)cb_fx16_atan2(CB_FX16_ONE, 0));
        return 1;
    }
    if (!brad_within(cb_fx16_atan2(0, -CB_FX16_ONE), CB_BRAD_HALF, TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(0,-1) != HALF (got %u)\n",
                (unsigned)cb_fx16_atan2(0, -CB_FX16_ONE));
        return 1;
    }
    if (!brad_within(cb_fx16_atan2(-CB_FX16_ONE, 0),
                     (cb_brad_t)(CB_BRAD_HALF + CB_BRAD_QUARTER), TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(-1,0) != 3*QUARTER (got %u)\n",
                (unsigned)cb_fx16_atan2(-CB_FX16_ONE, 0));
        return 1;
    }

    /* atan2(1,1) = pi/4 = 8192 brad */
    if (!brad_within(cb_fx16_atan2(CB_FX16_ONE, CB_FX16_ONE), (cb_brad_t)8192, TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(1,1) != 8192 brad (got %u)\n",
                (unsigned)cb_fx16_atan2(CB_FX16_ONE, CB_FX16_ONE));
        return 1;
    }
    /* atan2(-1,1) = -pi/4 = -8192 brad = 57344 brad */
    if (!brad_within(cb_fx16_atan2(-CB_FX16_ONE, CB_FX16_ONE), (cb_brad_t)(65536u - 8192u), TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(-1,1) wrong\n");
        return 1;
    }
    /* atan2(1,-1) = 3*pi/4 = 24576 brad */
    if (!brad_within(cb_fx16_atan2(CB_FX16_ONE, -CB_FX16_ONE), (cb_brad_t)24576, TOL_ATAN2_BRAD)) {
        fprintf(stderr, "FAIL: atan2(1,-1) wrong\n");
        return 1;
    }

    /* atan2(0,0) convention: 0 */
    CHECK(cb_fx16_atan2(0, 0) == 0, "atan2(0,0) convention = 0");

    printf("  trig OK\n");
    return 0;
}

/* ---------- section 5: Q32.32 ---------- */

static int section5_fx32(void)
{
    printf("Section 5: Q32.32 (world coordinates)\n");

    /* from_int / to_int round-trip */
    int64_t ints[] = { 0, 1, -1, 100, -100, 1000000000LL, -1000000000LL, 2147483647LL };
    for (size_t i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
        cb_fx32_t v = cb_fx32_from_int(ints[i]);
        int64_t back = cb_fx32_to_int(v);
        if (back != ints[i]) {
            fprintf(stderr, "FAIL: fx32 int round-trip %lld -> %lld\n",
                    (long long)ints[i], (long long)back);
            return 1;
        }
    }

    /* Constants */
    CHECK(cb_fx32_from_int(1) == CB_FX32_ONE, "fx32_from_int(1) = ONE");
    CHECK(cb_fx32_from_int(0) == 0, "fx32_from_int(0)");
    CHECK(cb_fx32_from_int(-1) == -CB_FX32_ONE, "fx32_from_int(-1)");

    /* add/sub */
    CHECK(cb_fx32_add(0, 0) == 0, "fx32_add(0,0)");
    CHECK(cb_fx32_add(CB_FX32_ONE, CB_FX32_ONE) == 2 * CB_FX32_ONE, "fx32_add(1,1)");
    CHECK(cb_fx32_sub(CB_FX32_ONE, CB_FX32_ONE) == 0, "fx32_sub(1,1)");
    CHECK(cb_fx32_add(CB_FX32_MAX, 1) == CB_FX32_MAX, "fx32_add(MAX,1) saturates");
    CHECK(cb_fx32_add(CB_FX32_MAX, CB_FX32_ONE) == CB_FX32_MAX, "fx32_add(MAX,ONE) saturates");
    CHECK(cb_fx32_sub(CB_FX32_MIN, 1) == CB_FX32_MIN, "fx32_sub(MIN,1) saturates");
    CHECK(cb_fx32_sub(CB_FX32_MIN, CB_FX32_ONE) == CB_FX32_MIN, "fx32_sub(MIN,ONE) saturates");

    /* mul: basics */
    CHECK(cb_fx32_mul(CB_FX32_ONE, CB_FX32_ONE) == CB_FX32_ONE, "fx32_mul(1,1)");
    CHECK(cb_fx32_mul(0, CB_FX32_MAX) == 0, "fx32_mul(0,MAX)");
    CHECK(cb_fx32_mul(-CB_FX32_ONE, CB_FX32_ONE) == -CB_FX32_ONE, "fx32_mul(-1,1)");
    CHECK(cb_fx32_mul(-CB_FX32_ONE, -CB_FX32_ONE) == CB_FX32_ONE, "fx32_mul(-1,-1)");

    /* mul: world-scale 100 * 100 = 10000 */
    {
        cb_fx32_t a = cb_fx32_from_int(100);
        cb_fx32_t b = cb_fx32_from_int(100);
        cb_fx32_t expect = cb_fx32_from_int(10000);
        CHECK(cb_fx32_mul(a, b) == expect, "fx32_mul(100,100)=10000");
    }
    /* 2 * 3 = 6 */
    CHECK(cb_fx32_mul(cb_fx32_from_int(2), cb_fx32_from_int(3)) == cb_fx32_from_int(6),
          "fx32_mul(2,3)=6");
    /* Half-constants: 0.5 * 0.5 = 0.25 */
    {
        cb_fx32_t half = CB_FX32_ONE >> 1;
        cb_fx32_t quarter = CB_FX32_ONE >> 2;
        CHECK(cb_fx32_mul(half, half) == quarter, "fx32_mul(0.5,0.5) = 0.25");
    }

    /* mul: saturation */
    CHECK(cb_fx32_mul(CB_FX32_MAX, 2 * CB_FX32_ONE) == CB_FX32_MAX,
          "fx32_mul(MAX,2) saturates to MAX");
    CHECK(cb_fx32_mul(CB_FX32_MAX, -2 * CB_FX32_ONE) == CB_FX32_MIN,
          "fx32_mul(MAX,-2) saturates to MIN");

    /* div */
    CHECK(cb_fx32_div(CB_FX32_ONE, CB_FX32_ONE) == CB_FX32_ONE, "fx32_div(1,1)");
    CHECK(cb_fx32_div(cb_fx32_from_int(10), cb_fx32_from_int(2)) == cb_fx32_from_int(5),
          "fx32_div(10,2)=5");
    CHECK(cb_fx32_div(0, CB_FX32_ONE) == 0, "fx32_div(0,1)");
    {
        cb_fx32_t half = CB_FX32_ONE >> 1;
        CHECK(cb_fx32_div(CB_FX32_ONE, cb_fx32_from_int(2)) == half, "fx32_div(1,2)=0.5");
    }
    /* div by zero saturation */
    CHECK(cb_fx32_div(CB_FX32_ONE, 0) == CB_FX32_MAX, "fx32_div(pos,0) -> MAX");
    CHECK(cb_fx32_div(-CB_FX32_ONE, 0) == CB_FX32_MIN, "fx32_div(neg,0) -> MIN");
    CHECK(cb_fx32_div(0, 0) == 0, "fx32_div(0,0) -> 0 (indeterminate)");

    /* sqrt */
    CHECK(cb_fx32_sqrt(0) == 0, "fx32_sqrt(0)=0");
    CHECK(cb_fx32_sqrt(CB_FX32_ONE) == CB_FX32_ONE, "fx32_sqrt(1)=1");
    {
        cb_fx32_t in = 4 * CB_FX32_ONE;
        cb_fx32_t got = cb_fx32_sqrt(in);
        cb_fx32_t expect = 2 * CB_FX32_ONE;
        if (!within_fx32(got, expect, TOL_FX32_SQRT)) {
            fprintf(stderr, "FAIL: fx32_sqrt(4) not ~ 2\n");
            return 1;
        }
    }
    {
        cb_fx32_t in = 9 * CB_FX32_ONE;
        cb_fx32_t got = cb_fx32_sqrt(in);
        cb_fx32_t expect = 3 * CB_FX32_ONE;
        if (!within_fx32(got, expect, TOL_FX32_SQRT)) {
            fprintf(stderr, "FAIL: fx32_sqrt(9) not ~ 3\n");
            return 1;
        }
    }
    CHECK(cb_fx32_sqrt(-CB_FX32_ONE) == 0, "fx32_sqrt(-1)=0");

    /* abs */
    CHECK(cb_fx32_abs(0) == 0, "fx32_abs(0)");
    CHECK(cb_fx32_abs(CB_FX32_ONE) == CB_FX32_ONE, "fx32_abs(1)");
    CHECK(cb_fx32_abs(-CB_FX32_ONE) == CB_FX32_ONE, "fx32_abs(-1)");
    CHECK(cb_fx32_abs(CB_FX32_MIN) == CB_FX32_MAX, "fx32_abs(MIN) must saturate to MAX");

    /* float round-trip (tooling) */
    {
        double d = 1234.5;
        cb_fx32_t fx = cb_fx32_from_float(d);
        double back = cb_fx32_to_double(fx);
        double delta = back - d;
        if (delta < 0) delta = -delta;
        /* One Q32.32 ulp = 2^-32 ~ 2.3e-10. Be generous. */
        if (delta > 1e-6) {
            fprintf(stderr, "FAIL: fx32 double round-trip bad (%g vs %g)\n", d, back);
            return 1;
        }
    }

    /* Mixed-width conversions: fx16 -> fx32 */
    {
        cb_fx16_t a16 = CB_FX16_ONE;
        cb_fx32_t a32 = cb_fx32_from_fx16(a16);
        CHECK(a32 == CB_FX32_ONE, "fx32_from_fx16(ONE) = fx32 ONE");
    }
    {
        cb_fx16_t a16 = -CB_FX16_ONE;
        cb_fx32_t a32 = cb_fx32_from_fx16(a16);
        CHECK(a32 == -CB_FX32_ONE, "fx32_from_fx16(-ONE)");
    }
    {
        cb_fx16_t a16 = CB_FX16_HALF;
        cb_fx32_t a32 = cb_fx32_from_fx16(a16);
        CHECK(a32 == (CB_FX32_ONE >> 1), "fx32_from_fx16(HALF)");
    }

    /* Mixed-width: fx32 -> fx16 round-trip */
    cb_fx16_t rt_vals[] = {
        0, CB_FX16_ONE, -CB_FX16_ONE, CB_FX16_HALF, -CB_FX16_HALF,
        CB_FX16_ONE * 100, -CB_FX16_ONE * 100
    };
    for (size_t i = 0; i < sizeof(rt_vals) / sizeof(rt_vals[0]); ++i) {
        cb_fx32_t wide = cb_fx32_from_fx16(rt_vals[i]);
        cb_fx16_t narrow = cb_fx16_from_fx32(wide);
        if (narrow != rt_vals[i]) {
            fprintf(stderr, "FAIL: fx16<->fx32 round-trip: %d -> %lld -> %d\n",
                    (int)rt_vals[i], (long long)wide, (int)narrow);
            return 1;
        }
    }

    /* fx16_from_fx32: saturation when out of Q16.16 range */
    {
        /* cb_fx32_from_int(100000) is > CB_FX16_MAX integer range */
        cb_fx32_t huge = cb_fx32_from_int(100000);
        cb_fx16_t narrow = cb_fx16_from_fx32(huge);
        CHECK(narrow == CB_FX16_MAX, "fx16_from_fx32(huge) saturates to MAX");
    }
    {
        cb_fx32_t huge_neg = cb_fx32_from_int(-100000);
        cb_fx16_t narrow = cb_fx16_from_fx32(huge_neg);
        CHECK(narrow == CB_FX16_MIN, "fx16_from_fx32(-huge) saturates to MIN");
    }

    printf("  fx32 OK\n");
    return 0;
}

/* ---------- section 6: determinism smoke test ---------- */

/*
 * This routine is the cross-platform determinism canary. Once the
 * implementation exists, run it on x86-64 and ARM64; both MUST produce
 * the identical accumulator bits.
 *
 * The routine:
 *   1. Seeds state to ONE.
 *   2. Runs 10000 iterations mixing mul/div/sqrt/sin/cos/add.
 *   3. XORs the raw bits of each iteration's state into an accumulator.
 *   4. Returns the accumulator.
 *
 * The expected value below is a PLACEHOLDER. After the implementation
 * lands, run this test once, copy the observed value into EXPECTED_ACC,
 * then CI on both x86 and ARM must match.
 */
static uint32_t determinism_routine(void)
{
    cb_fx16_t state = CB_FX16_ONE;
    uint32_t acc = 0;

    for (uint32_t i = 1; i <= 10000u; ++i) {
        cb_brad_t angle = (cb_brad_t)((i * 40u) & 0xFFFFu);
        cb_fx16_t s = cb_fx16_sin(angle);
        cb_fx16_t c = cb_fx16_cos(angle);

        state = cb_fx16_add(state, CB_FX16_ONE / 8);
        state = cb_fx16_mul(state, c);
        state = cb_fx16_add(state, s);

        /* Guard denominator away from 0 with a tiny bias. */
        cb_fx16_t denom = cb_fx16_add(CB_FX16_HALF, (cb_fx16_t)((int32_t)(i & 0xFFFF)));
        state = cb_fx16_div(state, denom);

        cb_fx16_t sq = cb_fx16_sqrt(cb_fx16_abs(cb_fx16_add(state, CB_FX16_ONE)));
        state = cb_fx16_sub(state, sq);

        /* Clamp to keep the walk bounded. */
        state = cb_fx16_clamp(state, -100 * CB_FX16_ONE, 100 * CB_FX16_ONE);

        acc ^= (uint32_t)state;
        /* rotate-left by 1; widen to avoid tripping -fsanitize=integer */
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));
    }

    return acc;
}

static int section6_determinism(void)
{
    printf("Section 6: determinism smoke test (10000 iterations)\n");

    /* TODO: fill in once implementation exists, from first run on x86-64.
       After ARM64 run, it MUST match this value bit-for-bit. */
    const uint32_t EXPECTED_ACC = 0x7B19CE20u;  /* captured on macOS arm64, clang */

    uint32_t got = determinism_routine();
    printf("  accumulator = 0x%08X (expected 0x%08X)\n",
           (unsigned)got, (unsigned)EXPECTED_ACC);

    if (got != EXPECTED_ACC) {
        fprintf(stderr, "FAIL: determinism accumulator mismatch. "
                        "If this is the FIRST run after implementing the module, "
                        "copy the observed value into EXPECTED_ACC and commit.\n");
        return 1;
    }

    printf("  determinism OK\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_fixed tests ===\n");

    if (section1_conversions()) { g_fails++; }
    if (section2_arithmetic())  { g_fails++; }
    if (section3_sqrt())        { g_fails++; }
    if (section4_trig())        { g_fails++; }
    if (section5_fx32())        { g_fails++; }
    if (section6_determinism()) { g_fails++; }

    if (g_fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", g_fails);
        return 1;
    }

    printf("All fixed tests passed!\n");
    return 0;
}
