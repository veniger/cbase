/*
 * random_test.c - Tests for the cb_random module (PCG32).
 *
 * The module does NOT exist yet. This file is the specification-as-tests
 * that will drive the implementation. It will fail to compile until
 * cbase_random.c is added to cbase_union.c.
 *
 * Determinism contract:
 *   - For the same (seed, stream) pair, every function in this module
 *     returns a bit-identical sequence on every supported platform
 *     (Windows / Linux / macOS, x86_64 / ARM64).
 *   - All sim randomness MUST flow through cb_rng_*. Replays reseed and
 *     must reproduce.
 *   - Section 1 pins the canonical PCG32 reference vector (seed=42,
 *     stream=54). Section 7 is a 10000-iteration canary whose accumulator
 *     is pinned after the first implementation run.
 *
 * The expected PCG32 reference vector assumes the canonical pcg-c
 * "srandom" init pattern used in O'Neill's demo:
 *     state = 0;
 *     inc   = (stream << 1) | 1;
 *     state = state * 6364136223846793005 + inc;
 *     state += seed;
 *     state = state * 6364136223846793005 + inc;
 * If the implementation picks a different init ordering, the first-6
 * values below will need to be re-pinned (see the PIN ME stderr print).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../cbase.h"

/* ---------- helpers ---------- */

static int g_fails = 0;

#define TEST_ASSERT(cond, msg) do {                                        \
    if (!(cond)) {                                                         \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__); \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* ---------- section 1: seed & reproducibility ---------- */

/*
 * Canonical PCG32 reference vector for seed=42, stream=54. Taken from
 * O'Neill's pcg-c demo output. These six 32-bit literals are the
 * cross-platform anchor for the implementation. If they change, either
 * the algorithm or the seeding convention has drifted.
 *
 * TO BE PINNED / VERIFIED: confirmed against pcg-c demo on first run.
 * If the implementation agent picks a different seeding convention and
 * these drift, re-pin from the "PIN ME" stderr dump and document the
 * choice in a comment above.
 */
static const uint32_t PCG32_REF_42_54[6] = {
    0xa15c02b7u,
    0x7b47f409u,
    0xba1d3330u,
    0x83d2f293u,
    0xbfa4784bu,
    0xcbed606eu
};

static int section1_seed_reproducibility(void)
{
    printf("--- Section 1: seed & reproducibility\n");

    /* Same (seed, stream) -> identical first N outputs. */
    {
        cb_rng_t a = cb_rng_seed(0x1234567890ABCDEFull, 7u);
        cb_rng_t b = cb_rng_seed(0x1234567890ABCDEFull, 7u);
        TEST_ASSERT(a.info == CB_INFO_OK, "seed() info must be OK (a)");
        TEST_ASSERT(b.info == CB_INFO_OK, "seed() info must be OK (b)");
        for (int i = 0; i < 64; ++i) {
            uint32_t va = cb_rng_u32(&a);
            uint32_t vb = cb_rng_u32(&b);
            if (va != vb) {
                fprintf(stderr, "FAIL: same seed diverged at i=%d (%08x vs %08x)\n",
                        i, va, vb);
                return 1;
            }
        }
    }

    /* Different streams -> different sequences from the same seed. */
    {
        cb_rng_t a = cb_rng_seed(12345u, 1u);
        cb_rng_t b = cb_rng_seed(12345u, 2u);
        int differences = 0;
        for (int i = 0; i < 8; ++i) {
            uint32_t va = cb_rng_u32(&a);
            uint32_t vb = cb_rng_u32(&b);
            if (va != vb) differences++;
        }
        /* Two PCG streams on the same seed differ on every draw with
           overwhelming probability; require at least 6/8 differences. */
        if (differences < 6) {
            fprintf(stderr, "FAIL: different streams produced too-similar output (%d/8 differ)\n",
                    differences);
            return 1;
        }
    }

    /* Canonical PCG32 reference vector: seed=42, stream=54. */
    {
        cb_rng_t rng = cb_rng_seed(42u, 54u);
        uint32_t observed[6];
        for (int i = 0; i < 6; ++i) observed[i] = cb_rng_u32(&rng);

        int match = 1;
        for (int i = 0; i < 6; ++i) {
            if (observed[i] != PCG32_REF_42_54[i]) { match = 0; break; }
        }
        if (!match) {
            fprintf(stderr, "FAIL: canonical PCG32 (seed=42, stream=54) vector mismatch.\n");
            fprintf(stderr, "  PIN ME - observed values (commit these into PCG32_REF_42_54 if\n");
            fprintf(stderr, "  the seeding convention is intentionally different):\n");
            for (int i = 0; i < 6; ++i) {
                fprintf(stderr, "    0x%08xu,\n", observed[i]);
            }
            return 1;
        }
    }

    /* u64 concatenates two u32s in (high, low) draw order. */
    {
        cb_rng_t a = cb_rng_seed(99u, 99u);
        cb_rng_t b = cb_rng_seed(99u, 99u);
        uint32_t hi = cb_rng_u32(&a);
        uint32_t lo = cb_rng_u32(&a);
        uint64_t combined = cb_rng_u64(&b);
        uint64_t expect = ((uint64_t)hi << 32) | (uint64_t)lo;
        if (combined != expect) {
            fprintf(stderr, "FAIL: u64 != (high<<32)|low : %016llx vs %016llx\n",
                    (unsigned long long)combined, (unsigned long long)expect);
            return 1;
        }
    }

    printf("  seed & reproducibility OK\n");
    return 0;
}

/* ---------- section 2: distribution smoke tests ---------- */

static int section2_distribution(void)
{
    printf("--- Section 2: distribution smoke tests\n");

    /* Not a rigor test. Catches "always 0 / always same" bugs. */

    /* u32 high-byte mean over 10000 samples should be loosely near 127.5. */
    {
        cb_rng_t rng = cb_rng_seed(0xCAFEBABEu, 0x1111u);
        uint64_t sum = 0;
        for (int i = 0; i < 10000; ++i) {
            uint32_t v = cb_rng_u32(&rng);
            sum += (uint64_t)((v >> 24) & 0xFFu);
        }
        double mean = (double)sum / 10000.0;
        if (mean < 120.0 || mean > 135.0) {
            fprintf(stderr, "FAIL: u32 high-byte mean out of band: %g (expect 120..135)\n", mean);
            return 1;
        }
    }

    /* cb_rng_bool: ~50% true over 10000 samples. */
    {
        cb_rng_t rng = cb_rng_seed(0xDEADBEEFu, 0x2222u);
        int trues = 0;
        for (int i = 0; i < 10000; ++i) {
            if (cb_rng_bool(&rng)) trues++;
        }
        if (trues < 4800 || trues > 5200) {
            fprintf(stderr, "FAIL: bool trues=%d out of [4800, 5200]\n", trues);
            return 1;
        }
    }

    /* cb_rng_u32_below(7) buckets: each bucket 0..6 gets at least 1200 hits. */
    {
        cb_rng_t rng = cb_rng_seed(0xA5A5A5A5u, 0x3333u);
        int buckets[7] = {0};
        for (int i = 0; i < 10000; ++i) {
            uint32_t v = cb_rng_u32_below(&rng, 7u);
            if (v >= 7u) {
                fprintf(stderr, "FAIL: u32_below(7) returned %u (>=7)\n", v);
                return 1;
            }
            buckets[v]++;
        }
        for (int i = 0; i < 7; ++i) {
            if (buckets[i] < 1200) {
                fprintf(stderr, "FAIL: u32_below(7) bucket %d has only %d hits (<1200)\n",
                        i, buckets[i]);
                return 1;
            }
        }
    }

    printf("  distribution OK\n");
    return 0;
}

/* ---------- section 3: bounded correctness ---------- */

static int section3_bounded(void)
{
    printf("--- Section 3: bounded integer correctness\n");

    /* u32_below(0) == 0 and does NOT advance state. */
    {
        cb_rng_t rng  = cb_rng_seed(7u, 7u);
        cb_rng_t ref  = cb_rng_seed(7u, 7u);
        for (int i = 0; i < 32; ++i) {
            uint32_t r = cb_rng_u32_below(&rng, 0u);
            TEST_ASSERT(r == 0u, "u32_below(0) must return 0");
        }
        /* After 32 zero-bound calls, state must still match ref (never advanced). */
        for (int i = 0; i < 8; ++i) {
            uint32_t va = cb_rng_u32(&rng);
            uint32_t vb = cb_rng_u32(&ref);
            if (va != vb) {
                fprintf(stderr, "FAIL: u32_below(0) advanced state (div at i=%d)\n", i);
                return 1;
            }
        }
    }

    /* u32_below(1) always 0 and DOES advance state (single outcome per call). */
    {
        cb_rng_t rng  = cb_rng_seed(7u, 7u);
        cb_rng_t ref  = cb_rng_seed(7u, 7u);
        for (int i = 0; i < 32; ++i) {
            uint32_t r = cb_rng_u32_below(&rng, 1u);
            TEST_ASSERT(r == 0u, "u32_below(1) must return 0");
            /* Ref advances too, for byte-for-byte comparison. */
            (void)cb_rng_u32(&ref);
        }
        /* Now draws must match between rng (post-32 advances) and ref (post-32 advances). */
        for (int i = 0; i < 8; ++i) {
            uint32_t va = cb_rng_u32(&rng);
            uint32_t vb = cb_rng_u32(&ref);
            if (va != vb) {
                fprintf(stderr, "FAIL: u32_below(1) must advance state exactly like u32 (i=%d)\n", i);
                return 1;
            }
        }
    }

    /* i32_range(-5, 5) over 10000 iters: all 11 buckets hit, min=-5, max=5. */
    {
        cb_rng_t rng = cb_rng_seed(0xFACEFEEDu, 0x4444u);
        int buckets[11] = {0};
        int32_t min_seen = INT32_MAX, max_seen = INT32_MIN;
        for (int i = 0; i < 10000; ++i) {
            int32_t v = cb_rng_i32_range(&rng, -5, 5);
            if (v < -5 || v > 5) {
                fprintf(stderr, "FAIL: i32_range(-5,5) out of band: %d\n", (int)v);
                return 1;
            }
            buckets[v + 5]++;
            if (v < min_seen) min_seen = v;
            if (v > max_seen) max_seen = v;
        }
        for (int i = 0; i < 11; ++i) {
            if (buckets[i] == 0) {
                fprintf(stderr, "FAIL: i32_range(-5,5) bucket %d never hit\n", i - 5);
                return 1;
            }
        }
        TEST_ASSERT(min_seen == -5, "i32_range(-5,5) min observed must be -5");
        TEST_ASSERT(max_seen == 5,  "i32_range(-5,5) max observed must be +5");
    }

    /* i32_range(7, 7): always 7 (degenerate single-point range). */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        for (int i = 0; i < 64; ++i) {
            int32_t v = cb_rng_i32_range(&rng, 7, 7);
            TEST_ASSERT(v == 7, "i32_range(7,7) must be 7");
        }
    }

    /* i32_range(10, 3): malformed (hi < lo) -> lo. */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        for (int i = 0; i < 32; ++i) {
            int32_t v = cb_rng_i32_range(&rng, 10, 3);
            TEST_ASSERT(v == 10, "i32_range(hi<lo) must return lo");
        }
    }

    /* Rejection uniformity: u32_below(3) over 60000 iters, each bucket in [18000, 22000]. */
    {
        cb_rng_t rng = cb_rng_seed(0x01234567u, 0x5555u);
        int buckets[3] = {0};
        for (int i = 0; i < 60000; ++i) {
            uint32_t v = cb_rng_u32_below(&rng, 3u);
            if (v >= 3u) {
                fprintf(stderr, "FAIL: u32_below(3) out of band: %u\n", v);
                return 1;
            }
            buckets[v]++;
        }
        for (int i = 0; i < 3; ++i) {
            if (buckets[i] < 18000 || buckets[i] > 22000) {
                fprintf(stderr, "FAIL: u32_below(3) bucket %d=%d out of [18000, 22000]\n",
                        i, buckets[i]);
                return 1;
            }
        }
    }

    printf("  bounded OK\n");
    return 0;
}

/* ---------- section 4: fixed-point & BRAD integration ---------- */

static int section4_fixed_brad(void)
{
    printf("--- Section 4: fixed-point & BRAD\n");

    /* fx16_unit: 10000 samples, all in [0, ONE), spread across the range, mean near ONE/2. */
    {
        cb_rng_t rng = cb_rng_seed(0xBEEF0001u, 0x6666u);
        int64_t sum = 0;
        int low_hits = 0, high_hits = 0;
        for (int i = 0; i < 10000; ++i) {
            cb_fx16_t v = cb_rng_fx16_unit(&rng);
            if (v < 0 || v >= CB_FX16_ONE) {
                fprintf(stderr, "FAIL: fx16_unit out of [0, ONE): %d\n", (int)v);
                return 1;
            }
            if (v < CB_FX16_ONE / 4)         low_hits++;
            if (v > 3 * (CB_FX16_ONE / 4))   high_hits++;
            sum += (int64_t)v;
        }
        TEST_ASSERT(low_hits  > 0, "fx16_unit: at least one sample below ONE/4 expected");
        TEST_ASSERT(high_hits > 0, "fx16_unit: at least one sample above 3*ONE/4 expected");

        int64_t mean = sum / 10000;
        int64_t half = (int64_t)(CB_FX16_ONE / 2);
        /* Mean should be loosely near ONE/2 (32768). Allow +/- ONE/8 = 8192. */
        int64_t d = mean - half;
        if (d < 0) d = -d;
        if (d > (int64_t)(CB_FX16_ONE / 8)) {
            fprintf(stderr, "FAIL: fx16_unit mean = %lld, expected near %lld\n",
                    (long long)mean, (long long)half);
            return 1;
        }
    }

    /* fx16_range(-ONE, ONE): 10000 samples, all in [-ONE, ONE), stretches to both halves. */
    {
        cb_rng_t rng = cb_rng_seed(0xBEEF0002u, 0x7777u);
        cb_fx16_t lo = -CB_FX16_ONE;
        cb_fx16_t hi =  CB_FX16_ONE;
        cb_fx16_t min_seen = CB_FX16_MAX;
        cb_fx16_t max_seen = CB_FX16_MIN;
        for (int i = 0; i < 10000; ++i) {
            cb_fx16_t v = cb_rng_fx16_range(&rng, lo, hi);
            if (v < lo || v >= hi) {
                fprintf(stderr, "FAIL: fx16_range out of band: %d\n", (int)v);
                return 1;
            }
            if (v < min_seen) min_seen = v;
            if (v > max_seen) max_seen = v;
        }
        if (min_seen > -(CB_FX16_ONE / 2)) {
            fprintf(stderr, "FAIL: fx16_range min %d never dipped below -ONE/2\n",
                    (int)min_seen);
            return 1;
        }
        if (max_seen < (CB_FX16_ONE / 2)) {
            fprintf(stderr, "FAIL: fx16_range max %d never rose above ONE/2\n",
                    (int)max_seen);
            return 1;
        }
    }

    /* fx16_range: degenerate hi<=lo returns lo. */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        for (int i = 0; i < 16; ++i) {
            cb_fx16_t v = cb_rng_fx16_range(&rng, CB_FX16_ONE, CB_FX16_ONE);
            TEST_ASSERT(v == CB_FX16_ONE, "fx16_range(eq, eq) must be lo");
        }
        for (int i = 0; i < 16; ++i) {
            cb_fx16_t v = cb_rng_fx16_range(&rng, 5 * CB_FX16_ONE, 2 * CB_FX16_ONE);
            TEST_ASSERT(v == 5 * CB_FX16_ONE, "fx16_range(hi<lo) must be lo");
        }
    }

    /* brad: all four quadrants must be well populated. */
    {
        cb_rng_t rng = cb_rng_seed(0xBEEF0003u, 0x8888u);
        int quads[4] = {0};
        for (int i = 0; i < 10000; ++i) {
            cb_brad_t b = cb_rng_brad(&rng);
            quads[b >> 14]++;  /* top 2 bits of uint16 = quadrant */
        }
        for (int q = 0; q < 4; ++q) {
            if (quads[q] < 1500) {
                fprintf(stderr, "FAIL: brad quadrant %d only %d hits (<1500)\n",
                        q, quads[q]);
                return 1;
            }
        }
    }

    printf("  fixed & brad OK\n");
    return 0;
}

/* ---------- section 5: chance ---------- */

static int section5_chance(void)
{
    printf("--- Section 5: chance\n");

    /* p = 0: never true. */
    {
        cb_rng_t rng = cb_rng_seed(11u, 22u);
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, 0);
            TEST_ASSERT(!b, "chance(0) must never be true");
        }
    }

    /* p = ONE: always true. */
    {
        cb_rng_t rng = cb_rng_seed(11u, 22u);
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, CB_FX16_ONE);
            TEST_ASSERT(b, "chance(ONE) must always be true");
        }
    }

    /* p = ONE/4: ~25% over 10000 iters. */
    {
        cb_rng_t rng = cb_rng_seed(0x2468ACE0u, 0x9999u);
        int trues = 0;
        for (int i = 0; i < 10000; ++i) {
            if (cb_rng_chance_fx16(&rng, CB_FX16_ONE / 4)) trues++;
        }
        if (trues < 2200 || trues > 2800) {
            fprintf(stderr, "FAIL: chance(ONE/4) trues=%d out of [2200, 2800]\n", trues);
            return 1;
        }
    }

    /* Negative probability -> always false (clamped to 0). */
    {
        cb_rng_t rng = cb_rng_seed(11u, 22u);
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, -CB_FX16_ONE);
            TEST_ASSERT(!b, "chance(negative) must clamp to false");
        }
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, CB_FX16_MIN);
            TEST_ASSERT(!b, "chance(MIN) must clamp to false");
        }
    }

    /* Probability > ONE -> always true (clamped to ONE). */
    {
        cb_rng_t rng = cb_rng_seed(11u, 22u);
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, 2 * CB_FX16_ONE);
            TEST_ASSERT(b, "chance(2*ONE) must clamp to true");
        }
        for (int i = 0; i < 5000; ++i) {
            bool b = cb_rng_chance_fx16(&rng, CB_FX16_MAX);
            TEST_ASSERT(b, "chance(MAX) must clamp to true");
        }
    }

    printf("  chance OK\n");
    return 0;
}

/* ---------- section 6: shuffle ---------- */

typedef struct {
    int64_t a;
    int64_t b;
    int64_t c;
} s6_wide24_t;

static int section6_shuffle(void)
{
    printf("--- Section 6: shuffle\n");

    /* Shuffle identity [0..15]: must still be a permutation (each value once). */
    {
        cb_rng_t rng = cb_rng_seed(0x12340001u, 0xAAAAu);
        int32_t arr[16];
        for (int i = 0; i < 16; ++i) arr[i] = i;
        cb_rng_shuffle(&rng, arr, 16, sizeof(int32_t));
        TEST_ASSERT(rng.info == CB_INFO_OK, "shuffle(stride=4) info must stay OK");

        int seen[16] = {0};
        for (int i = 0; i < 16; ++i) {
            TEST_ASSERT(arr[i] >= 0 && arr[i] < 16, "shuffled value out of [0,15]");
            seen[arr[i]]++;
        }
        for (int i = 0; i < 16; ++i) {
            TEST_ASSERT(seen[i] == 1, "shuffle must produce a permutation (each value exactly once)");
        }
    }

    /* Same seed, same input -> identical byte-for-byte output. */
    {
        cb_rng_t r1 = cb_rng_seed(0xDEADBEEFu, 0xBBBBu);
        cb_rng_t r2 = cb_rng_seed(0xDEADBEEFu, 0xBBBBu);
        int32_t a[16], b[16];
        for (int i = 0; i < 16; ++i) { a[i] = i; b[i] = i; }
        cb_rng_shuffle(&r1, a, 16, sizeof(int32_t));
        cb_rng_shuffle(&r2, b, 16, sizeof(int32_t));
        if (memcmp(a, b, sizeof(a)) != 0) {
            fprintf(stderr, "FAIL: shuffle not deterministic under same seed\n");
            return 1;
        }
    }

    /* Different seed usually -> different output (loose: at least 4 differences in 16 elements). */
    {
        cb_rng_t r1 = cb_rng_seed(1u, 0xCCCCu);
        cb_rng_t r2 = cb_rng_seed(2u, 0xCCCCu);
        int32_t a[16], b[16];
        for (int i = 0; i < 16; ++i) { a[i] = i; b[i] = i; }
        cb_rng_shuffle(&r1, a, 16, sizeof(int32_t));
        cb_rng_shuffle(&r2, b, 16, sizeof(int32_t));
        int diff = 0;
        for (int i = 0; i < 16; ++i) if (a[i] != b[i]) diff++;
        if (diff < 4) {
            fprintf(stderr, "FAIL: shuffles with different seeds only differ in %d positions\n", diff);
            return 1;
        }
    }

    /* n = 0 no-op: array untouched, info OK. */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        int32_t arr[4] = { 11, 22, 33, 44 };
        int32_t copy[4] = { 11, 22, 33, 44 };
        cb_rng_shuffle(&rng, arr, 0, sizeof(int32_t));
        TEST_ASSERT(rng.info == CB_INFO_OK, "shuffle(n=0) info must be OK");
        TEST_ASSERT(memcmp(arr, copy, sizeof(arr)) == 0, "shuffle(n=0) must leave array untouched");
    }

    /* n = 1 no-op: single-element array unchanged. */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        int32_t arr[1] = { 77 };
        cb_rng_shuffle(&rng, arr, 1, sizeof(int32_t));
        TEST_ASSERT(rng.info == CB_INFO_OK, "shuffle(n=1) info must be OK");
        TEST_ASSERT(arr[0] == 77, "shuffle(n=1) must leave single element untouched");
    }

    /* Stride too large -> CB_INFO_RNG_BAD_STRIDE, array untouched. */
    {
        cb_rng_t rng = cb_rng_seed(1u, 1u);
        size_t bad_stride = CB_RNG_SHUFFLE_MAX_STRIDE + 1;
        unsigned char buf[3 * 512];  /* 3 elements of bad_stride bytes each, up to 1536 */
        for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = (unsigned char)(i & 0xFFu);
        unsigned char copy[3 * 512];
        memcpy(copy, buf, sizeof(buf));

        cb_rng_shuffle(&rng, buf, 3, bad_stride);
        TEST_ASSERT(rng.info == CB_INFO_RNG_BAD_STRIDE,
                    "shuffle(stride > cap) must set CB_INFO_RNG_BAD_STRIDE");
        /* Only the first 3 * bad_stride bytes are the live region; compare them. */
        if (memcmp(buf, copy, 3 * bad_stride) != 0) {
            fprintf(stderr, "FAIL: shuffle with oversized stride modified the array\n");
            return 1;
        }
    }

    /* Stride 24 (3x int64) works and preserves multiset identity. */
    {
        cb_rng_t rng = cb_rng_seed(0x76543210u, 0xDDDDu);
        s6_wide24_t arr[12];
        for (int i = 0; i < 12; ++i) {
            arr[i].a = (int64_t)i * 1000 + 1;
            arr[i].b = (int64_t)i * 1000 + 2;
            arr[i].c = (int64_t)i * 1000 + 3;
        }
        cb_rng_shuffle(&rng, arr, 12, sizeof(s6_wide24_t));
        TEST_ASSERT(rng.info == CB_INFO_OK, "shuffle(stride=24) info must stay OK");

        int seen[12] = {0};
        for (int i = 0; i < 12; ++i) {
            /* Recover original index from .a field. */
            int orig = (int)((arr[i].a - 1) / 1000);
            TEST_ASSERT(orig >= 0 && orig < 12, "shuffle(24) element corrupted");
            /* Elements must remain intact: b and c must match the recovered orig. */
            TEST_ASSERT(arr[i].b == (int64_t)orig * 1000 + 2,
                        "shuffle(24) corrupted struct field b");
            TEST_ASSERT(arr[i].c == (int64_t)orig * 1000 + 3,
                        "shuffle(24) corrupted struct field c");
            seen[orig]++;
        }
        for (int i = 0; i < 12; ++i) {
            TEST_ASSERT(seen[i] == 1, "shuffle(24) multiset identity broken");
        }
    }

    printf("  shuffle OK\n");
    return 0;
}

/* ---------- section 7: determinism canary ---------- */

/*
 * Cross-platform determinism canary. Once the implementation lands,
 * run this on macOS arm64 (clang) first, copy the observed value into
 * EXPECTED_ACC, then the same value MUST come back from x86_64 Linux,
 * x86_64 Windows, and every supported target.
 *
 * The routine mixes u32, fx16_unit, brad, chance, u32_below, and a
 * small in-place shuffle. Every output is folded into a 32-bit
 * accumulator via xor + rotate-left-1 (UBSan-safe widening). The
 * shuffle contribution is captured by XORing the post-shuffle array
 * sum every 100 iterations.
 */
static uint32_t determinism_routine(void)
{
    cb_rng_t rng = cb_rng_seed(0xD1E7C0DEu, 0x0F00Du);
    uint32_t acc = 0;
    int32_t deck[8];
    for (int i = 0; i < 8; ++i) deck[i] = i;

    for (uint32_t i = 1; i <= 10000u; ++i) {
        uint32_t  r  = cb_rng_u32(&rng);
        cb_fx16_t fu = cb_rng_fx16_unit(&rng);
        cb_brad_t br = cb_rng_brad(&rng);
        bool      ch = cb_rng_chance_fx16(&rng, CB_FX16_ONE / 3);
        uint32_t  b  = cb_rng_u32_below(&rng, 1000u);

        acc ^= r;
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));

        acc ^= (uint32_t)fu;
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));

        acc ^= (uint32_t)br;
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));

        acc ^= ch ? 0xA5A5A5A5u : 0x5A5A5A5Au;
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));

        acc ^= b;
        acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));

        if ((i % 100u) == 0u) {
            cb_rng_shuffle(&rng, deck, 8, sizeof(int32_t));
            uint32_t fold = 0;
            for (int k = 0; k < 8; ++k) {
                fold ^= (uint32_t)deck[k];
                fold = (uint32_t)(((uint64_t)fold << 1) | (fold >> 31));
            }
            acc ^= fold;
            acc = (uint32_t)(((uint64_t)acc << 1) | (acc >> 31));
        }
    }

    return acc;
}

static int section7_determinism(void)
{
    printf("--- Section 7: determinism canary (10000 iterations)\n");

    /* Pinned on macOS arm64, clang. If a new arch diverges, suspect the
       PCG32 step or the Lemire threshold before anything else. */
    const uint32_t EXPECTED_ACC = 0x5655FF65u;

    uint32_t got = determinism_routine();
    printf("  accumulator = 0x%08X (expected 0x%08X)\n",
           (unsigned)got, (unsigned)EXPECTED_ACC);

    if (got != EXPECTED_ACC) {
        fprintf(stderr, "FAIL: rng determinism accumulator mismatch.\n");
        fprintf(stderr, "  PIN ME - if this is the FIRST run after implementing\n");
        fprintf(stderr, "  cb_random, copy the observed value into EXPECTED_ACC and commit.\n");
        return 1;
    }

    printf("  determinism OK\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_random tests ===\n");

    if (section1_seed_reproducibility()) { g_fails++; }
    if (section2_distribution())         { g_fails++; }
    if (section3_bounded())              { g_fails++; }
    if (section4_fixed_brad())           { g_fails++; }
    if (section5_chance())               { g_fails++; }
    if (section6_shuffle())              { g_fails++; }
    if (section7_determinism())          { g_fails++; }

    if (g_fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", g_fails);
        return 1;
    }

    printf("All random tests passed!\n");
    return 0;
}
