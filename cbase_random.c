/*
 * cbase_random.c - PCG32 deterministic PRNG.
 *
 * Algorithm: PCG32 (O'Neill, 2014). 64-bit LCG + XSH-RR output
 * permutation. Canonical pcg-c "srandom" seeding so the reference vector
 * for (seed=42, stream=54) matches the published demo output.
 *
 * All math is integer. No libm, no floats, no global state.
 * Sanitizer regime: -fsanitize=integer,undefined,address — see trap notes
 * inline (unsigned negation, signed span subtraction, rotation mask).
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const uint64_t CB__PCG_MULT = 6364136223846793005ULL;

/* The LCG step is mod-2^64 wrap by definition; UBSan's unsigned-overflow
   check would flag every tick. Narrow the opt-out to just this helper. */
#if defined(__clang__)
    #define CB__NO_SANITIZE_INT __attribute__((no_sanitize("unsigned-integer-overflow")))
#else
    #define CB__NO_SANITIZE_INT
#endif

CB__NO_SANITIZE_INT
static inline void cb__pcg_step(cb_rng_t *rng)
{
    rng->state = rng->state * CB__PCG_MULT + rng->inc;
}

static inline uint32_t cb__pcg_output(uint64_t state)
{
    uint32_t xs  = (uint32_t)(((state >> 18u) ^ state) >> 27u);
    uint32_t rot = (uint32_t)(state >> 59u);
    /* Use (32 - rot) & 31 rather than (-rot) & 31: -fsanitize=integer
       flags unary-minus on unsigned. Equivalent for rot in [0, 31]. */
    uint32_t shr = rot;
    uint32_t shl = (32u - rot) & 31u;
    /* Widen the left shift so UBSan's shift-result-overflow check stays
       quiet; narrowing cast preserves the same low 32 bits. */
    return (uint32_t)((xs >> shr) | ((uint64_t)xs << shl));
}

/* --- Core --- */

cb_rng_t cb_rng_seed(uint64_t seed, uint64_t stream)
{
    cb_rng_t rng;
    rng.info  = CB_INFO_OK;
    rng.state = 0ULL;
    rng.inc   = (stream << 1u) | 1u;
    cb__pcg_step(&rng);
    rng.state += seed;
    cb__pcg_step(&rng);
    return rng;
}

uint32_t cb_rng_u32(cb_rng_t *rng)
{
    uint64_t old = rng->state;
    cb__pcg_step(rng);
    return cb__pcg_output(old);
}

uint64_t cb_rng_u64(cb_rng_t *rng)
{
    uint32_t high = cb_rng_u32(rng);
    uint32_t low  = cb_rng_u32(rng);
    return ((uint64_t)high << 32) | (uint64_t)low;
}

/* --- Bounded integers (unbiased via Lemire's nearly-divisionless) --- */

uint32_t cb_rng_u32_below(cb_rng_t *rng, uint32_t bound)
{
    if (bound == 0u) return 0u;             /* no state advance */

    uint32_t x = cb_rng_u32(rng);
    uint64_t m = (uint64_t)x * (uint64_t)bound;
    uint32_t l = (uint32_t)m;
    if (l < bound) {
        /* Threshold = 2^32 mod bound, computed in u64 so the subtraction
           does not trip -fsanitize=integer. */
        uint32_t t = (uint32_t)(((uint64_t)1 << 32) % (uint64_t)bound);
        while (l < t) {
            x = cb_rng_u32(rng);
            m = (uint64_t)x * (uint64_t)bound;
            l = (uint32_t)m;
        }
    }
    return (uint32_t)(m >> 32);
}

int32_t cb_rng_i32_range(cb_rng_t *rng, int32_t lo, int32_t hi_inclusive)
{
    if (hi_inclusive < lo) return lo;       /* malformed -> lo, no advance */

    /* Compute span in signed i64 — subtracting two i32s never overflows
       i64, so no unsigned-wrap trap. span64 ∈ [1, 2^32]. */
    int64_t span64 = (int64_t)hi_inclusive - (int64_t)lo + 1;
    uint32_t r;
    if (span64 >= ((int64_t)1 << 32)) {
        /* Full 2^32 range (lo=INT32_MIN, hi=INT32_MAX). */
        r = cb_rng_u32(rng);
    } else {
        r = cb_rng_u32_below(rng, (uint32_t)span64);
    }
    /* Result = lo + r is always inside [lo, hi], so the i32 narrow is safe. */
    return (int32_t)((int64_t)lo + (int64_t)r);
}

/* --- Fixed-point & angles --- */

cb_fx16_t cb_rng_fx16_unit(cb_rng_t *rng)
{
    /* Top 16 bits of a u32 gives [0, 65536) == [0, CB_FX16_ONE). */
    uint32_t u = cb_rng_u32(rng);
    return (cb_fx16_t)(u >> 16);
}

cb_fx16_t cb_rng_fx16_range(cb_rng_t *rng, cb_fx16_t lo, cb_fx16_t hi)
{
    if (hi <= lo) return lo;                /* degenerate / malformed */

    /* cb_fx16_t is int32_t; span fits in int64_t. Max span is ~2^32, so
       it always fits in uint32_t after the cap below. */
    int64_t span = (int64_t)hi - (int64_t)lo;
    if (span > (int64_t)UINT32_MAX) span = (int64_t)UINT32_MAX;
    uint32_t r = cb_rng_u32_below(rng, (uint32_t)span);
    return (cb_fx16_t)((int64_t)lo + (int64_t)r);
}

cb_brad_t cb_rng_brad(cb_rng_t *rng)
{
    uint32_t u = cb_rng_u32(rng);
    return (cb_brad_t)(u >> 16);
}

/* --- Bool / chance --- */

bool cb_rng_bool(cb_rng_t *rng)
{
    return (bool)(cb_rng_u32(rng) >> 31);
}

bool cb_rng_chance_fx16(cb_rng_t *rng, cb_fx16_t probability)
{
    if (probability <= 0)            return false;   /* no advance */
    if (probability >= CB_FX16_ONE)  return true;    /* no advance */
    return cb_rng_fx16_unit(rng) < probability;
}

/* --- Shuffle (Fisher-Yates, in place, deterministic) --- */

void cb_rng_shuffle(cb_rng_t *rng, void *base, size_t n, size_t stride)
{
    if (n < 2) return;                      /* info untouched */
    if (stride > CB_RNG_SHUFFLE_MAX_STRIDE) {
        rng->info = CB_INFO_RNG_BAD_STRIDE;
        return;                             /* array untouched */
    }

    uint8_t  tmp[CB_RNG_SHUFFLE_MAX_STRIDE];
    uint8_t *bytes = (uint8_t *)base;

    for (size_t i = n - 1; i > 0; --i) {
        uint32_t j = cb_rng_u32_below(rng, (uint32_t)(i + 1u));
        if ((size_t)j == i) continue;
        uint8_t *pi = bytes + i * stride;
        uint8_t *pj = bytes + (size_t)j * stride;
        memcpy(tmp, pi,  stride);
        memcpy(pi,  pj,  stride);
        memcpy(pj,  tmp, stride);
    }
    /* Do NOT touch rng->info on success — preserves prior errors. */
}
