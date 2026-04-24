/*
    cbase_fixed.c — deterministic fixed-point math.

    Sim-path operations are bit-exact across platforms: no floats, no libm,
    LUT-backed trig. Overflow saturates. Rounding is truncate-toward-zero.

    Float helpers (cb_fx*_from_float / to_float / to_double) exist for the
    renderer and offline tooling. Never call them from authoritative sim code.
*/

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
    #define CB__HAS_INT128 1
    typedef          __int128 cb__i128;
    typedef unsigned __int128 cb__u128;
#elif defined(_MSC_VER)
    #define CB__HAS_INT128 0
    #include <intrin.h>
#else
    #define CB__HAS_INT128 0
#endif

/* ================================================================ */
/*  LUTs — generated from sin / atan, endpoints pinned to exacts.   */
/* ================================================================ */

/* sin over [0, pi/2], 1025 entries (index 1024 = ONE exactly). */
static const cb_fx16_t cb__sin_lut[1025] = {
         0,    101,    201,    302,    402,    503,    603,    704,
       804,    905,   1005,   1106,   1206,   1307,   1407,   1508,
      1608,   1709,   1809,   1910,   2010,   2111,   2211,   2312,
      2412,   2513,   2613,   2714,   2814,   2914,   3015,   3115,
      3216,   3316,   3417,   3517,   3617,   3718,   3818,   3918,
      4019,   4119,   4219,   4320,   4420,   4520,   4621,   4721,
      4821,   4921,   5022,   5122,   5222,   5322,   5422,   5523,
      5623,   5723,   5823,   5923,   6023,   6123,   6224,   6324,
      6424,   6524,   6624,   6724,   6824,   6924,   7024,   7124,
      7224,   7323,   7423,   7523,   7623,   7723,   7823,   7923,
      8022,   8122,   8222,   8322,   8421,   8521,   8621,   8720,
      8820,   8919,   9019,   9119,   9218,   9318,   9417,   9517,
      9616,   9716,   9815,   9914,  10014,  10113,  10212,  10312,
     10411,  10510,  10609,  10709,  10808,  10907,  11006,  11105,
     11204,  11303,  11402,  11501,  11600,  11699,  11798,  11897,
     11996,  12095,  12193,  12292,  12391,  12490,  12588,  12687,
     12785,  12884,  12983,  13081,  13180,  13278,  13376,  13475,
     13573,  13672,  13770,  13868,  13966,  14065,  14163,  14261,
     14359,  14457,  14555,  14653,  14751,  14849,  14947,  15045,
     15143,  15240,  15338,  15436,  15534,  15631,  15729,  15826,
     15924,  16021,  16119,  16216,  16314,  16411,  16508,  16606,
     16703,  16800,  16897,  16994,  17091,  17188,  17285,  17382,
     17479,  17576,  17673,  17770,  17867,  17963,  18060,  18156,
     18253,  18350,  18446,  18543,  18639,  18735,  18832,  18928,
     19024,  19120,  19216,  19313,  19409,  19505,  19600,  19696,
     19792,  19888,  19984,  20080,  20175,  20271,  20366,  20462,
     20557,  20653,  20748,  20844,  20939,  21034,  21129,  21224,
     21320,  21415,  21510,  21604,  21699,  21794,  21889,  21984,
     22078,  22173,  22268,  22362,  22457,  22551,  22645,  22740,
     22834,  22928,  23022,  23116,  23210,  23304,  23398,  23492,
     23586,  23680,  23774,  23867,  23961,  24054,  24148,  24241,
     24335,  24428,  24521,  24614,  24708,  24801,  24894,  24987,
     25080,  25172,  25265,  25358,  25451,  25543,  25636,  25728,
     25821,  25913,  26005,  26098,  26190,  26282,  26374,  26466,
     26558,  26650,  26742,  26833,  26925,  27017,  27108,  27200,
     27291,  27382,  27474,  27565,  27656,  27747,  27838,  27929,
     28020,  28111,  28202,  28293,  28383,  28474,  28564,  28655,
     28745,  28835,  28926,  29016,  29106,  29196,  29286,  29376,
     29466,  29555,  29645,  29735,  29824,  29914,  30003,  30093,
     30182,  30271,  30360,  30449,  30538,  30627,  30716,  30805,
     30893,  30982,  31071,  31159,  31248,  31336,  31424,  31512,
     31600,  31688,  31776,  31864,  31952,  32040,  32127,  32215,
     32303,  32390,  32477,  32565,  32652,  32739,  32826,  32913,
     33000,  33087,  33173,  33260,  33347,  33433,  33520,  33606,
     33692,  33778,  33865,  33951,  34037,  34122,  34208,  34294,
     34380,  34465,  34551,  34636,  34721,  34806,  34892,  34977,
     35062,  35146,  35231,  35316,  35401,  35485,  35570,  35654,
     35738,  35823,  35907,  35991,  36075,  36159,  36243,  36326,
     36410,  36493,  36577,  36660,  36744,  36827,  36910,  36993,
     37076,  37159,  37241,  37324,  37407,  37489,  37572,  37654,
     37736,  37818,  37900,  37982,  38064,  38146,  38228,  38309,
     38391,  38472,  38554,  38635,  38716,  38797,  38878,  38959,
     39040,  39120,  39201,  39282,  39362,  39442,  39523,  39603,
     39683,  39763,  39843,  39922,  40002,  40082,  40161,  40241,
     40320,  40399,  40478,  40557,  40636,  40715,  40794,  40872,
     40951,  41029,  41108,  41186,  41264,  41342,  41420,  41498,
     41576,  41653,  41731,  41808,  41886,  41963,  42040,  42117,
     42194,  42271,  42348,  42424,  42501,  42578,  42654,  42730,
     42806,  42882,  42958,  43034,  43110,  43186,  43261,  43337,
     43412,  43487,  43562,  43638,  43713,  43787,  43862,  43937,
     44011,  44086,  44160,  44234,  44308,  44382,  44456,  44530,
     44604,  44677,  44751,  44824,  44898,  44971,  45044,  45117,
     45190,  45262,  45335,  45408,  45480,  45552,  45625,  45697,
     45769,  45841,  45912,  45984,  46056,  46127,  46199,  46270,
     46341,  46412,  46483,  46554,  46624,  46695,  46765,  46836,
     46906,  46976,  47046,  47116,  47186,  47256,  47325,  47395,
     47464,  47534,  47603,  47672,  47741,  47809,  47878,  47947,
     48015,  48084,  48152,  48220,  48288,  48356,  48424,  48491,
     48559,  48626,  48694,  48761,  48828,  48895,  48962,  49029,
     49095,  49162,  49228,  49295,  49361,  49427,  49493,  49559,
     49624,  49690,  49756,  49821,  49886,  49951,  50016,  50081,
     50146,  50211,  50275,  50340,  50404,  50468,  50532,  50596,
     50660,  50724,  50787,  50851,  50914,  50977,  51041,  51104,
     51166,  51229,  51292,  51354,  51417,  51479,  51541,  51603,
     51665,  51727,  51789,  51850,  51911,  51973,  52034,  52095,
     52156,  52217,  52277,  52338,  52398,  52459,  52519,  52579,
     52639,  52699,  52759,  52818,  52878,  52937,  52996,  53055,
     53114,  53173,  53232,  53290,  53349,  53407,  53465,  53523,
     53581,  53639,  53697,  53754,  53812,  53869,  53926,  53983,
     54040,  54097,  54154,  54210,  54267,  54323,  54379,  54435,
     54491,  54547,  54603,  54658,  54714,  54769,  54824,  54879,
     54934,  54989,  55043,  55098,  55152,  55206,  55260,  55314,
     55368,  55422,  55476,  55529,  55582,  55636,  55689,  55742,
     55794,  55847,  55900,  55952,  56004,  56056,  56108,  56160,
     56212,  56264,  56315,  56367,  56418,  56469,  56520,  56571,
     56621,  56672,  56722,  56773,  56823,  56873,  56923,  56972,
     57022,  57072,  57121,  57170,  57219,  57268,  57317,  57366,
     57414,  57463,  57511,  57559,  57607,  57655,  57703,  57750,
     57798,  57845,  57892,  57939,  57986,  58033,  58079,  58126,
     58172,  58219,  58265,  58311,  58356,  58402,  58448,  58493,
     58538,  58583,  58628,  58673,  58718,  58763,  58807,  58851,
     58896,  58940,  58983,  59027,  59071,  59114,  59158,  59201,
     59244,  59287,  59330,  59372,  59415,  59457,  59499,  59541,
     59583,  59625,  59667,  59708,  59750,  59791,  59832,  59873,
     59914,  59954,  59995,  60035,  60075,  60116,  60156,  60195,
     60235,  60275,  60314,  60353,  60392,  60431,  60470,  60509,
     60547,  60586,  60624,  60662,  60700,  60738,  60776,  60813,
     60851,  60888,  60925,  60962,  60999,  61035,  61072,  61108,
     61145,  61181,  61217,  61253,  61288,  61324,  61359,  61394,
     61429,  61464,  61499,  61534,  61568,  61603,  61637,  61671,
     61705,  61739,  61772,  61806,  61839,  61873,  61906,  61939,
     61971,  62004,  62036,  62069,  62101,  62133,  62165,  62197,
     62228,  62260,  62291,  62322,  62353,  62384,  62415,  62445,
     62476,  62506,  62536,  62566,  62596,  62626,  62655,  62685,
     62714,  62743,  62772,  62801,  62830,  62858,  62886,  62915,
     62943,  62971,  62998,  63026,  63054,  63081,  63108,  63135,
     63162,  63189,  63215,  63242,  63268,  63294,  63320,  63346,
     63372,  63397,  63423,  63448,  63473,  63498,  63523,  63547,
     63572,  63596,  63621,  63645,  63668,  63692,  63716,  63739,
     63763,  63786,  63809,  63832,  63854,  63877,  63899,  63922,
     63944,  63966,  63987,  64009,  64031,  64052,  64073,  64094,
     64115,  64136,  64156,  64177,  64197,  64217,  64237,  64257,
     64277,  64296,  64316,  64335,  64354,  64373,  64392,  64410,
     64429,  64447,  64465,  64483,  64501,  64519,  64536,  64554,
     64571,  64588,  64605,  64622,  64639,  64655,  64672,  64688,
     64704,  64720,  64735,  64751,  64766,  64782,  64797,  64812,
     64827,  64841,  64856,  64870,  64884,  64899,  64912,  64926,
     64940,  64953,  64967,  64980,  64993,  65006,  65018,  65031,
     65043,  65055,  65067,  65079,  65091,  65103,  65114,  65126,
     65137,  65148,  65159,  65169,  65180,  65190,  65200,  65210,
     65220,  65230,  65240,  65249,  65259,  65268,  65277,  65286,
     65294,  65303,  65311,  65320,  65328,  65336,  65343,  65351,
     65358,  65366,  65373,  65380,  65387,  65393,  65400,  65406,
     65413,  65419,  65425,  65430,  65436,  65442,  65447,  65452,
     65457,  65462,  65467,  65471,  65476,  65480,  65484,  65488,
     65492,  65495,  65499,  65502,  65505,  65508,  65511,  65514,
     65516,  65519,  65521,  65523,  65525,  65527,  65528,  65530,
     65531,  65532,  65533,  65534,  65535,  65535,  65536,  65536,
     65536
};

/* atan over [0, 1], 257 entries (index 256 = QUARTER/2 = 8192 brad exactly). */
static const cb_brad_t cb__atan_lut[257] = {
        0,    41,    81,   122,   163,   204,   244,   285,
      326,   367,   407,   448,   489,   529,   570,   610,
      651,   692,   732,   773,   813,   854,   894,   935,
      975,  1015,  1056,  1096,  1136,  1177,  1217,  1257,
     1297,  1337,  1377,  1417,  1457,  1497,  1537,  1577,
     1617,  1656,  1696,  1736,  1775,  1815,  1854,  1894,
     1933,  1973,  2012,  2051,  2090,  2129,  2168,  2207,
     2246,  2285,  2324,  2363,  2401,  2440,  2478,  2517,
     2555,  2594,  2632,  2670,  2708,  2746,  2784,  2822,
     2860,  2897,  2935,  2973,  3010,  3047,  3085,  3122,
     3159,  3196,  3233,  3270,  3307,  3344,  3380,  3417,
     3453,  3490,  3526,  3562,  3599,  3635,  3670,  3706,
     3742,  3778,  3813,  3849,  3884,  3920,  3955,  3990,
     4025,  4060,  4095,  4129,  4164,  4199,  4233,  4267,
     4302,  4336,  4370,  4404,  4438,  4471,  4505,  4539,
     4572,  4605,  4639,  4672,  4705,  4738,  4771,  4803,
     4836,  4869,  4901,  4933,  4966,  4998,  5030,  5062,
     5094,  5125,  5157,  5188,  5220,  5251,  5282,  5313,
     5344,  5375,  5406,  5437,  5467,  5498,  5528,  5559,
     5589,  5619,  5649,  5679,  5708,  5738,  5768,  5797,
     5826,  5856,  5885,  5914,  5943,  5972,  6000,  6029,
     6058,  6086,  6114,  6142,  6171,  6199,  6227,  6254,
     6282,  6310,  6337,  6365,  6392,  6419,  6446,  6473,
     6500,  6527,  6554,  6580,  6607,  6633,  6660,  6686,
     6712,  6738,  6764,  6790,  6815,  6841,  6867,  6892,
     6917,  6943,  6968,  6993,  7018,  7043,  7068,  7092,
     7117,  7141,  7166,  7190,  7214,  7238,  7262,  7286,
     7310,  7334,  7358,  7381,  7405,  7428,  7451,  7475,
     7498,  7521,  7544,  7566,  7589,  7612,  7635,  7657,
     7679,  7702,  7724,  7746,  7768,  7790,  7812,  7834,
     7856,  7877,  7899,  7920,  7942,  7963,  7984,  8005,
     8026,  8047,  8068,  8089,  8110,  8131,  8151,  8172,
     8192
};

/* ================================================================ */
/*  Q16.16 conversions                                              */
/* ================================================================ */

cb_fx16_t cb_fx16_from_int(int32_t v)
{
    if (v > 32767)  return CB_FX16_MAX;
    if (v < -32768) return CB_FX16_MIN;
    return (cb_fx16_t)((int64_t)v * (int64_t)CB_FX16_ONE);
}

int32_t cb_fx16_to_int(cb_fx16_t v)
{
    /* Divide (not shift) for truncate-toward-zero on negatives. */
    return (int32_t)(v / CB_FX16_ONE);
}

cb_fx16_t cb_fx16_from_float(float v)
{
    /* Tooling-only. Caller's responsibility to avoid NaN/Inf. */
    float scaled = v * 65536.0f;
    if (scaled >= 2147483520.0f) return CB_FX16_MAX;  /* largest float <= INT32_MAX */
    if (scaled <= -2147483648.0f) return CB_FX16_MIN;
    return (cb_fx16_t)scaled;
}

float cb_fx16_to_float(cb_fx16_t v)
{
    return (float)v / 65536.0f;
}

/* ================================================================ */
/*  Q16.16 arithmetic                                               */
/* ================================================================ */

cb_fx16_t cb_fx16_add(cb_fx16_t a, cb_fx16_t b)
{
    int64_t r = (int64_t)a + (int64_t)b;
    if (r > (int64_t)CB_FX16_MAX) return CB_FX16_MAX;
    if (r < (int64_t)CB_FX16_MIN) return CB_FX16_MIN;
    return (cb_fx16_t)r;
}

cb_fx16_t cb_fx16_sub(cb_fx16_t a, cb_fx16_t b)
{
    int64_t r = (int64_t)a - (int64_t)b;
    if (r > (int64_t)CB_FX16_MAX) return CB_FX16_MAX;
    if (r < (int64_t)CB_FX16_MIN) return CB_FX16_MIN;
    return (cb_fx16_t)r;
}

cb_fx16_t cb_fx16_mul(cb_fx16_t a, cb_fx16_t b)
{
    int64_t prod = (int64_t)a * (int64_t)b;
    /* Divide, not shift: shift of negative signed is arithmetic (floor)
       while we want truncate-toward-zero to match the tests. */
    int64_t scaled = prod / (int64_t)CB_FX16_ONE;
    if (scaled > (int64_t)CB_FX16_MAX) return CB_FX16_MAX;
    if (scaled < (int64_t)CB_FX16_MIN) return CB_FX16_MIN;
    return (cb_fx16_t)scaled;
}

cb_fx16_t cb_fx16_div(cb_fx16_t a, cb_fx16_t b)
{
    if (b == 0)
    {
        if (a == 0) return 0;                       /* indeterminate -> 0 */
        return (a > 0) ? CB_FX16_MAX : CB_FX16_MIN;
    }
    /* Multiply (not <<) to avoid UB on negative shift. */
    int64_t num = (int64_t)a * (int64_t)CB_FX16_ONE;
    int64_t q   = num / b;
    if (q > (int64_t)CB_FX16_MAX) return CB_FX16_MAX;
    if (q < (int64_t)CB_FX16_MIN) return CB_FX16_MIN;
    return (cb_fx16_t)q;
}

cb_fx16_t cb_fx16_abs(cb_fx16_t v)
{
    if (v == CB_FX16_MIN) return CB_FX16_MAX;    /* -INT32_MIN doesn't fit; saturate */
    return (v < 0) ? -v : v;
}

cb_fx16_t cb_fx16_min(cb_fx16_t a, cb_fx16_t b)   { return (a < b) ? a : b; }
cb_fx16_t cb_fx16_max(cb_fx16_t a, cb_fx16_t b)   { return (a > b) ? a : b; }

cb_fx16_t cb_fx16_clamp(cb_fx16_t v, cb_fx16_t lo, cb_fx16_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

cb_fx16_t cb_fx16_lerp(cb_fx16_t a, cb_fx16_t b, cb_fx16_t t)
{
    cb_fx16_t diff   = cb_fx16_sub(b, a);
    cb_fx16_t scaled = cb_fx16_mul(diff, t);
    return cb_fx16_add(a, scaled);
}

/* ================================================================ */
/*  Q16.16 sqrt — digit-by-digit integer, bit-exact                 */
/* ================================================================ */

cb_fx16_t cb_fx16_sqrt(cb_fx16_t v)
{
    if (v <= 0) return 0;
    /* Q16.16 sqrt: sqrt(v) = isqrt(v * 2^16).  v <= 2^31-1, so v<<16 <= 2^47. */
    uint64_t n   = (uint64_t)(uint32_t)v << 16;
    uint64_t res = 0;
    uint64_t bit = (uint64_t)1 << 46;
    while (bit > n) bit >>= 2;
    while (bit != 0)
    {
        if (n >= res + bit)
        {
            n  -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (cb_fx16_t)res;
}

/* ================================================================ */
/*  Q16.16 trig — LUT-backed                                        */
/* ================================================================ */

/* sin over first-quadrant position (0..16384) using the LUT. */
static cb_fx16_t cb__sin_q1(uint32_t pos)
{
    if (pos >= 16384) return CB_FX16_ONE;       /* boundary: sin(pi/2) */
    uint32_t idx  = pos >> 4;                   /* 0..1023 */
    uint32_t frac = pos & 0xF;                  /* 0..15 */
    int32_t a = cb__sin_lut[idx];
    int32_t b = cb__sin_lut[idx + 1];
    int32_t diff = b - a;
    return (cb_fx16_t)(a + (diff * (int32_t)frac) / 16);
}

cb_fx16_t cb_fx16_sin(cb_brad_t angle)
{
    uint32_t a        = (uint32_t)angle;
    uint32_t quadrant = a >> 14;                /* 0..3 */
    uint32_t pos      = a & 0x3FFF;             /* 0..16383 */
    cb_fx16_t v;
    if ((quadrant & 1u) == 0u)
    {
        v = cb__sin_q1(pos);                    /* Q0 or Q2: direct */
    }
    else
    {
        v = cb__sin_q1(16384u - pos);           /* Q1 or Q3: mirror */
    }
    if (quadrant >= 2u) v = -v;
    return v;
}

cb_fx16_t cb_fx16_cos(cb_brad_t angle)
{
    return cb_fx16_sin((cb_brad_t)((uint32_t)angle + CB_BRAD_QUARTER));
}

cb_brad_t cb_fx16_atan2(cb_fx16_t y, cb_fx16_t x)
{
    if (x == 0 && y == 0) return 0;
    if (x == 0) return (y > 0) ? CB_BRAD_QUARTER
                               : (cb_brad_t)(CB_BRAD_HALF + CB_BRAD_QUARTER);
    if (y == 0) return (x > 0) ? (cb_brad_t)0
                               : CB_BRAD_HALF;

    cb_fx16_t ax = cb_fx16_abs(x);
    cb_fx16_t ay = cb_fx16_abs(y);

    int complement;
    cb_fx16_t small, large;
    if (ay > ax) { small = ax; large = ay; complement = 1; }
    else         { small = ay; large = ax; complement = 0; }

    /* t in [0, ONE]. small <= large so division is safe. */
    cb_fx16_t t = cb_fx16_div(small, large);

    uint32_t tu  = (uint32_t)t;
    uint32_t idx = tu >> 8;                     /* 0..256 */
    uint32_t frac = tu & 0xFF;
    int32_t a_val, b_val;
    if (idx >= 256u) { a_val = cb__atan_lut[256]; b_val = cb__atan_lut[256]; frac = 0; }
    else             { a_val = cb__atan_lut[idx]; b_val = cb__atan_lut[idx + 1]; }
    int32_t diff  = b_val - a_val;
    int32_t res_i = a_val + (diff * (int32_t)frac) / 256;
    uint32_t res = (uint32_t)res_i;

    if (complement) res = (uint32_t)CB_BRAD_QUARTER - res;
    if (x < 0)      res = (uint32_t)CB_BRAD_HALF - res;
    if (y < 0)      res = (res == 0u) ? 0u : (65536u - res);
    return (cb_brad_t)(res & 0xFFFFu);
}

/* ================================================================ */
/*  Q32.32 conversions                                              */
/* ================================================================ */

cb_fx32_t cb_fx32_from_int(int64_t v)
{
    if (v > (int64_t)2147483647LL)        return CB_FX32_MAX;
    if (v < (int64_t)(-2147483647LL - 1)) return CB_FX32_MIN;
    return (cb_fx32_t)v * CB_FX32_ONE;
}

int64_t cb_fx32_to_int(cb_fx32_t v)
{
    return (int64_t)(v / CB_FX32_ONE);
}

cb_fx32_t cb_fx32_from_float(double v)
{
    double scaled = v * 4294967296.0;           /* 2^32 */
    if (scaled >= 9.2233720368547748e18) return CB_FX32_MAX;   /* ~INT64_MAX */
    if (scaled <= -9.2233720368547758e18) return CB_FX32_MIN;  /* INT64_MIN */
    return (cb_fx32_t)scaled;
}

double cb_fx32_to_double(cb_fx32_t v)
{
    return (double)v / 4294967296.0;
}

/* ================================================================ */
/*  Q32.32 arithmetic                                               */
/* ================================================================ */

cb_fx32_t cb_fx32_add(cb_fx32_t a, cb_fx32_t b)
{
#if CB__HAS_INT128
    cb__i128 r = (cb__i128)a + (cb__i128)b;
    if (r > (cb__i128)CB_FX32_MAX) return CB_FX32_MAX;
    if (r < (cb__i128)CB_FX32_MIN) return CB_FX32_MIN;
    return (cb_fx32_t)r;
#else
    int64_t r = (int64_t)((uint64_t)a + (uint64_t)b);
    /* Overflow iff operands same sign but result opposite sign. */
    if (((a ^ r) & (b ^ r)) < 0) return (a < 0) ? CB_FX32_MIN : CB_FX32_MAX;
    return r;
#endif
}

cb_fx32_t cb_fx32_sub(cb_fx32_t a, cb_fx32_t b)
{
#if CB__HAS_INT128
    cb__i128 r = (cb__i128)a - (cb__i128)b;
    if (r > (cb__i128)CB_FX32_MAX) return CB_FX32_MAX;
    if (r < (cb__i128)CB_FX32_MIN) return CB_FX32_MIN;
    return (cb_fx32_t)r;
#else
    int64_t r = (int64_t)((uint64_t)a - (uint64_t)b);
    if (((a ^ b) & (a ^ r)) < 0) return (a < 0) ? CB_FX32_MIN : CB_FX32_MAX;
    return r;
#endif
}

cb_fx32_t cb_fx32_mul(cb_fx32_t a, cb_fx32_t b)
{
#if CB__HAS_INT128
    cb__i128 prod   = (cb__i128)a * (cb__i128)b;
    cb__i128 scaled = prod / (cb__i128)CB_FX32_ONE;
    if (scaled > (cb__i128)CB_FX32_MAX) return CB_FX32_MAX;
    if (scaled < (cb__i128)CB_FX32_MIN) return CB_FX32_MIN;
    return (cb_fx32_t)scaled;
#else
    /* MSVC 64-bit path. Produces a 128-bit product, then shifts by 32. */
    int64_t hi;
    int64_t lo = _mul128(a, b, &hi);
    /* Compose shifted = (hi : lo) >> 32 (arithmetic). */
    uint64_t ulo = (uint64_t)lo;
    int64_t shifted_lo = (int64_t)((ulo >> 32) | ((uint64_t)hi << 32));
    int64_t shifted_hi = hi >> 32;
    if (shifted_hi != (shifted_lo >> 63))
    {
        return (shifted_hi < 0) ? CB_FX32_MIN : CB_FX32_MAX;
    }
    return (cb_fx32_t)shifted_lo;
#endif
}

cb_fx32_t cb_fx32_div(cb_fx32_t a, cb_fx32_t b)
{
    if (b == 0)
    {
        if (a == 0) return 0;
        return (a > 0) ? CB_FX32_MAX : CB_FX32_MIN;
    }
#if CB__HAS_INT128
    cb__i128 num = (cb__i128)a * (cb__i128)CB_FX32_ONE;
    cb__i128 q   = num / b;
    if (q > (cb__i128)CB_FX32_MAX) return CB_FX32_MAX;
    if (q < (cb__i128)CB_FX32_MIN) return CB_FX32_MIN;
    return (cb_fx32_t)q;
#else
    /* MSVC sign-magnitude path using _udiv128 for the unsigned core. */
    int neg = ((a < 0) ^ (b < 0));
    uint64_t ua = (a < 0) ? (uint64_t)(-(a + 1)) + 1u : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-(b + 1)) + 1u : (uint64_t)b;
    /* numerator = ua << 32, 128-bit unsigned. */
    uint64_t num_hi = ua >> 32;
    uint64_t num_lo = ua << 32;
    uint64_t rem;
    /* _udiv128 will raise SEH on overflow; guard first. */
    if (num_hi >= ub)
    {
        return neg ? CB_FX32_MIN : CB_FX32_MAX;
    }
    uint64_t q = _udiv128(num_hi, num_lo, ub, &rem);
    if (neg)
    {
        if (q > (uint64_t)CB_FX32_MAX + 1u) return CB_FX32_MIN;
        return (q == (uint64_t)CB_FX32_MAX + 1u) ? CB_FX32_MIN : -(cb_fx32_t)q;
    }
    else
    {
        if (q > (uint64_t)CB_FX32_MAX) return CB_FX32_MAX;
        return (cb_fx32_t)q;
    }
#endif
}

cb_fx32_t cb_fx32_abs(cb_fx32_t v)
{
    if (v == CB_FX32_MIN) return CB_FX32_MAX;
    return (v < 0) ? -v : v;
}

cb_fx32_t cb_fx32_sqrt(cb_fx32_t v)
{
    if (v <= 0) return 0;
#if CB__HAS_INT128
    cb__u128 n   = (cb__u128)(uint64_t)v << 32;
    cb__u128 res = 0;
    cb__u128 bit = (cb__u128)1 << 94;
    while (bit > n) bit >>= 2;
    while (bit != 0)
    {
        if (n >= res + bit)
        {
            n  -= res + bit;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (cb_fx32_t)res;
#else
    /* MSVC fallback: operate on a {hi, lo} uint64 pair. Same digit-by-digit
       algorithm, but manual 96-bit arithmetic. bit starts at 2^94 = (1<<30) in hi. */
    uint64_t n_lo = (uint64_t)v << 32;
    uint64_t n_hi = (uint64_t)v >> 32;
    uint64_t res_lo = 0, res_hi = 0;
    uint64_t bit_lo = 0, bit_hi = (uint64_t)1 << 30;
    /* shrink bit until bit <= n */
    while (bit_hi > n_hi || (bit_hi == n_hi && bit_lo > n_lo))
    {
        /* bit >>= 2 as 128-bit */
        uint64_t new_hi = bit_hi >> 2;
        uint64_t new_lo = (bit_lo >> 2) | (bit_hi << 62);
        bit_hi = new_hi; bit_lo = new_lo;
    }
    while (bit_hi != 0 || bit_lo != 0)
    {
        /* cand = res + bit (128-bit add) */
        uint64_t cand_lo = res_lo + bit_lo;
        uint64_t carry   = (cand_lo < res_lo) ? 1u : 0u;
        uint64_t cand_hi = res_hi + bit_hi + carry;
        int ge = (n_hi > cand_hi) || (n_hi == cand_hi && n_lo >= cand_lo);
        if (ge)
        {
            /* n -= cand */
            uint64_t new_lo = n_lo - cand_lo;
            uint64_t borrow = (n_lo < cand_lo) ? 1u : 0u;
            uint64_t new_hi = n_hi - cand_hi - borrow;
            n_lo = new_lo; n_hi = new_hi;
            /* res = (res >> 1) + bit */
            uint64_t rs_lo = (res_lo >> 1) | (res_hi << 63);
            uint64_t rs_hi = res_hi >> 1;
            uint64_t nr_lo = rs_lo + bit_lo;
            uint64_t c2    = (nr_lo < rs_lo) ? 1u : 0u;
            uint64_t nr_hi = rs_hi + bit_hi + c2;
            res_lo = nr_lo; res_hi = nr_hi;
        }
        else
        {
            uint64_t rs_lo = (res_lo >> 1) | (res_hi << 63);
            uint64_t rs_hi = res_hi >> 1;
            res_lo = rs_lo; res_hi = rs_hi;
        }
        /* bit >>= 2 */
        uint64_t new_hi = bit_hi >> 2;
        uint64_t new_lo = (bit_lo >> 2) | (bit_hi << 62);
        bit_hi = new_hi; bit_lo = new_lo;
    }
    return (cb_fx32_t)res_lo;
#endif
}

/* ================================================================ */
/*  Mixed-width conversions                                         */
/* ================================================================ */

cb_fx32_t cb_fx32_from_fx16(cb_fx16_t v)
{
    /* Every Q16.16 fits in Q32.32 with room: shift by 16 as a multiply. */
    return (cb_fx32_t)v * ((cb_fx32_t)1 << 16);
}

cb_fx16_t cb_fx16_from_fx32(cb_fx32_t v)
{
    /* Divide (not >>) for truncate-toward-zero. */
    int64_t narrowed = v / ((cb_fx32_t)1 << 16);
    if (narrowed > (int64_t)CB_FX16_MAX) return CB_FX16_MAX;
    if (narrowed < (int64_t)CB_FX16_MIN) return CB_FX16_MIN;
    return (cb_fx16_t)narrowed;
}
