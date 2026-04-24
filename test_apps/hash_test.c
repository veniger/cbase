/*
 * hash_test.c - Tests for the cb_sha256 module.
 *
 * Covers:
 *   1. Empty-input KAT ("" and NULL/0).
 *   2. "abc" KAT.
 *   3. 448-bit multi-block KAT.
 *   4. Million 'a' KAT with varied-size streaming chunks.
 *   5. Incremental (3-chunk) vs one-shot equivalence.
 *   6. Single-byte updates vs one-shot.
 *   7. Cross-block boundary with odd chunk sizes (63, 2, 135).
 *   8. Ctx re-init after final.
 *   9. (documentation-only) Repeated final not tested.
 *  10. Byte-order independence sanity via {0x01,0x02,0x03,0x04}.
 *  11. Fuzz round-trip: 200 iterations, random lengths, random splits.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../cbase.h"

/* ---------- helpers ---------- */

static int digest_eq(const uint8_t a[CB_SHA256_DIGEST_LEN],
                     const uint8_t b[CB_SHA256_DIGEST_LEN])
{
    return memcmp(a, b, CB_SHA256_DIGEST_LEN) == 0;
}

static void print_digest(const uint8_t d[CB_SHA256_DIGEST_LEN])
{
    for (int i = 0; i < CB_SHA256_DIGEST_LEN; ++i) {
        fprintf(stderr, "%02x", d[i]);
    }
}

static void fail_digest_mismatch(const char *name,
                                 const uint8_t got[CB_SHA256_DIGEST_LEN],
                                 const uint8_t want[CB_SHA256_DIGEST_LEN])
{
    fprintf(stderr, "FAIL: %s\n  got:  ", name);
    print_digest(got);
    fprintf(stderr, "\n  want: ");
    print_digest(want);
    fprintf(stderr, "\n");
}

/* ---------- section 1: empty input ---------- */

static int section1_empty(void)
{
    printf("--- Section 1: empty input KAT\n");

    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0xe3,0xb0,0xc4,0x42, 0x98,0xfc,0x1c,0x14,
        0x9a,0xfb,0xf4,0xc8, 0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4, 0x64,0x9b,0x93,0x4c,
        0xa4,0x95,0x99,0x1b, 0x78,0x52,0xb8,0x55
    };

    uint8_t out[CB_SHA256_DIGEST_LEN];

    cb_sha256("", 0, out);
    if (!digest_eq(out, want)) {
        fail_digest_mismatch("empty-string empty input", out, want);
        printf("FAIL: section 1 empty-string input\n");
        return 1;
    }

    memset(out, 0xAA, sizeof(out));
    cb_sha256(NULL, 0, out);
    if (!digest_eq(out, want)) {
        fail_digest_mismatch("NULL/0 empty input", out, want);
        printf("FAIL: section 1 NULL input\n");
        return 1;
    }

    printf("PASS: section 1 empty input\n");
    return 0;
}

/* ---------- section 2: "abc" KAT ---------- */

static int section2_abc(void)
{
    printf("--- Section 2: \"abc\" KAT\n");

    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0xba,0x78,0x16,0xbf, 0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde, 0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3, 0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61, 0xf2,0x00,0x15,0xad
    };

    uint8_t out[CB_SHA256_DIGEST_LEN];
    cb_sha256("abc", 3, out);
    if (!digest_eq(out, want)) {
        fail_digest_mismatch("abc", out, want);
        printf("FAIL: section 2 abc\n");
        return 1;
    }

    printf("PASS: section 2 abc\n");
    return 0;
}

/* ---------- section 3: 448-bit multi-block KAT ---------- */

static int section3_multi_block(void)
{
    printf("--- Section 3: 448-bit multi-block KAT\n");

    const char *msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0x24,0x8d,0x6a,0x61, 0xd2,0x06,0x38,0xb8,
        0xe5,0xc0,0x26,0x93, 0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59, 0x64,0xff,0x21,0x67,
        0xf6,0xec,0xed,0xd4, 0x19,0xdb,0x06,0xc1
    };

    uint8_t out[CB_SHA256_DIGEST_LEN];
    cb_sha256(msg, strlen(msg), out);
    if (!digest_eq(out, want)) {
        fail_digest_mismatch("448-bit KAT", out, want);
        printf("FAIL: section 3 multi-block\n");
        return 1;
    }

    printf("PASS: section 3 multi-block\n");
    return 0;
}

/* ---------- section 4: million 'a' KAT ---------- */

static int section4_million_a(void)
{
    printf("--- Section 4: 1,000,000 'a' KAT\n");

    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0xcd,0xc7,0x6e,0x5c, 0x99,0x14,0xfb,0x92,
        0x81,0xa1,0xc7,0xe2, 0x84,0xd7,0x3e,0x67,
        0xf1,0x80,0x9a,0x48, 0xa4,0x97,0x20,0x0e,
        0x04,0x6d,0x39,0xcc, 0xc7,0x11,0x2c,0xd0
    };

    /* Build 1,000,000 'a' bytes via many updates of varied size to exercise
       partial-block carryover across block boundaries repeatedly. */
    static uint8_t chunk[4096];
    memset(chunk, 'a', sizeof(chunk));

    cb_sha256_ctx_t ctx;
    cb_sha256_init(&ctx);

    size_t remaining = 1000000;
    /* Rotate through a few chunk sizes to hit different carryover paths:
       4, 1000, 4096, 37. */
    const size_t sizes[] = { 4, 1000, 4096, 37 };
    const size_t n_sizes = sizeof(sizes) / sizeof(sizes[0]);
    size_t idx = 0;
    while (remaining > 0) {
        size_t want_n = sizes[idx % n_sizes];
        idx++;
        size_t n = want_n < remaining ? want_n : remaining;
        cb_sha256_update(&ctx, chunk, n);
        remaining -= n;
    }

    uint8_t out[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, out);

    if (!digest_eq(out, want)) {
        fail_digest_mismatch("million 'a'", out, want);
        printf("FAIL: section 4 million 'a'\n");
        return 1;
    }

    printf("PASS: section 4 million 'a'\n");
    return 0;
}

/* ---------- section 5: incremental (3 chunks) vs one-shot ---------- */

static int section5_incremental(void)
{
    printf("--- Section 5: incremental == one-shot (\"quick brown fox\")\n");

    const char *msg = "The quick brown fox jumps over the lazy dog";
    const size_t len = 43; /* strlen */
    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0xd7,0xa8,0xfb,0xb3, 0x07,0xd7,0x80,0x94,
        0x69,0xca,0x9a,0xbc, 0xb0,0x08,0x2e,0x4f,
        0x8d,0x56,0x51,0xe4, 0x6d,0x3c,0xdb,0x76,
        0x2d,0x02,0xd0,0xbf, 0x37,0xc9,0xe5,0x92
    };

    uint8_t one_shot[CB_SHA256_DIGEST_LEN];
    cb_sha256(msg, len, one_shot);
    if (!digest_eq(one_shot, want)) {
        fail_digest_mismatch("fox one-shot", one_shot, want);
        printf("FAIL: section 5 one-shot mismatch\n");
        return 1;
    }

    /* Three random-sized splits. Sizes chosen to be odd/uneven and sum to
       the total length. */
    const size_t split_a = 7;
    const size_t split_b = 19;
    const size_t split_c = len - split_a - split_b; /* = 17 */

    cb_sha256_ctx_t ctx;
    cb_sha256_init(&ctx);
    cb_sha256_update(&ctx, msg, split_a);
    cb_sha256_update(&ctx, msg + split_a, split_b);
    cb_sha256_update(&ctx, msg + split_a + split_b, split_c);
    uint8_t streamed[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, streamed);

    if (!digest_eq(streamed, want)) {
        fail_digest_mismatch("fox streamed", streamed, want);
        printf("FAIL: section 5 streamed mismatch\n");
        return 1;
    }

    if (!digest_eq(streamed, one_shot)) {
        printf("FAIL: section 5 streamed != one-shot\n");
        return 1;
    }

    printf("PASS: section 5 incremental\n");
    return 0;
}

/* ---------- section 6: single-byte updates ---------- */

static int section6_single_byte(void)
{
    printf("--- Section 6: single-byte updates\n");

    const char *msg = "Hello, cbase!";
    const size_t len = strlen(msg);

    uint8_t one_shot[CB_SHA256_DIGEST_LEN];
    cb_sha256(msg, len, one_shot);

    cb_sha256_ctx_t ctx;
    cb_sha256_init(&ctx);
    for (size_t i = 0; i < len; ++i) {
        cb_sha256_update(&ctx, msg + i, 1);
    }
    uint8_t streamed[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, streamed);

    if (!digest_eq(streamed, one_shot)) {
        fail_digest_mismatch("single-byte updates", streamed, one_shot);
        printf("FAIL: section 6 single-byte\n");
        return 1;
    }

    printf("PASS: section 6 single-byte\n");
    return 0;
}

/* ---------- section 7: cross-block boundary ---------- */

static int section7_cross_block(void)
{
    printf("--- Section 7: cross-block boundary (63, 2, 135)\n");

    /* 200 bytes total. Sizes 63 + 2 + 135 = 200; 63+2=65 crosses one block
       boundary (with 1 byte of carryover), and 65+135=200 crosses the next. */
    uint8_t data[200];
    for (size_t i = 0; i < sizeof(data); ++i) {
        data[i] = (uint8_t)((i * 17u + 3u) & 0xFFu);
    }

    uint8_t one_shot[CB_SHA256_DIGEST_LEN];
    cb_sha256(data, sizeof(data), one_shot);

    cb_sha256_ctx_t ctx;
    cb_sha256_init(&ctx);
    cb_sha256_update(&ctx, data,            63);
    cb_sha256_update(&ctx, data + 63,        2);
    cb_sha256_update(&ctx, data + 65,      135);
    uint8_t streamed[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, streamed);

    if (!digest_eq(streamed, one_shot)) {
        fail_digest_mismatch("cross-block", streamed, one_shot);
        printf("FAIL: section 7 cross-block\n");
        return 1;
    }

    printf("PASS: section 7 cross-block\n");
    return 0;
}

/* ---------- section 8: ctx re-init after final ---------- */

static int section8_reinit(void)
{
    printf("--- Section 8: ctx re-init after final\n");

    const char *msg_a = "first message";
    const char *msg_b = "a completely different second message";

    uint8_t want_a[CB_SHA256_DIGEST_LEN];
    uint8_t want_b[CB_SHA256_DIGEST_LEN];
    cb_sha256(msg_a, strlen(msg_a), want_a);
    cb_sha256(msg_b, strlen(msg_b), want_b);

    cb_sha256_ctx_t ctx;

    cb_sha256_init(&ctx);
    cb_sha256_update(&ctx, msg_a, strlen(msg_a));
    uint8_t got_a[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, got_a);
    if (!digest_eq(got_a, want_a)) {
        fail_digest_mismatch("reinit first", got_a, want_a);
        printf("FAIL: section 8 first hash\n");
        return 1;
    }

    /* Re-init the SAME ctx and hash a different message. */
    cb_sha256_init(&ctx);
    cb_sha256_update(&ctx, msg_b, strlen(msg_b));
    uint8_t got_b[CB_SHA256_DIGEST_LEN];
    cb_sha256_final(&ctx, got_b);
    if (!digest_eq(got_b, want_b)) {
        fail_digest_mismatch("reinit second", got_b, want_b);
        printf("FAIL: section 8 second hash\n");
        return 1;
    }

    printf("PASS: section 8 reinit\n");
    return 0;
}

/* ---------- section 10: byte-order independence ---------- */

static int section10_byte_order(void)
{
    printf("--- Section 10: byte-order independence\n");

    const uint8_t input[4] = { 0x01u, 0x02u, 0x03u, 0x04u };
    const uint8_t want[CB_SHA256_DIGEST_LEN] = {
        0x9f,0x64,0xa7,0x47, 0xe1,0xb9,0x7f,0x13,
        0x1f,0xab,0xb6,0xb4, 0x47,0x29,0x6c,0x9b,
        0x6f,0x02,0x01,0xe7, 0x9f,0xb3,0xc5,0x35,
        0x6e,0x6c,0x77,0xe8, 0x9b,0x6a,0x80,0x6a
    };

    uint8_t out[CB_SHA256_DIGEST_LEN];
    cb_sha256(input, sizeof(input), out);
    if (!digest_eq(out, want)) {
        fail_digest_mismatch("byte-order {01,02,03,04}", out, want);
        printf("FAIL: section 10 byte-order\n");
        return 1;
    }

    /* Sanity: the literal C-string "\x01\x02\x03\x04" of length 4 must
       yield the same digest. */
    uint8_t out2[CB_SHA256_DIGEST_LEN];
    cb_sha256("\x01\x02\x03\x04", 4, out2);
    if (!digest_eq(out, out2)) {
        printf("FAIL: section 10 uint8_t vs C-string disagree\n");
        return 1;
    }

    printf("PASS: section 10 byte-order\n");
    return 0;
}

/* ---------- section 11: fuzz round-trip ---------- */

static int section11_fuzz(void)
{
    printf("--- Section 11: fuzz round-trip (200 iters)\n");

    cb_rng_t rng = cb_rng_seed(0xC0FFEEull, 0x1u);

    static uint8_t buf[4096];

    for (int iter = 0; iter < 200; ++iter) {
        /* Length in [0, 4096]. */
        uint32_t len32 = cb_rng_u32_below(&rng, 4097u);
        size_t len = (size_t)len32;

        /* Fill with random bytes. */
        for (size_t i = 0; i < len; ++i) {
            buf[i] = (uint8_t)(cb_rng_u32(&rng) & 0xFFu);
        }

        /* One-shot. */
        uint8_t want[CB_SHA256_DIGEST_LEN];
        cb_sha256(buf, len, want);

        /* Streamed: pick between 1 and 8 chunks, each a random slice. */
        uint32_t n_chunks = 1u + cb_rng_u32_below(&rng, 8u);
        cb_sha256_ctx_t ctx;
        cb_sha256_init(&ctx);

        size_t consumed = 0;
        for (uint32_t c = 0; c < n_chunks; ++c) {
            size_t remaining = len - consumed;
            size_t chunk;
            if (c + 1 == n_chunks) {
                chunk = remaining;   /* last chunk flushes the rest */
            } else {
                /* Random split in [0, remaining]. */
                if (remaining == 0) {
                    chunk = 0;
                } else {
                    uint32_t pick = cb_rng_u32_below(&rng, (uint32_t)remaining + 1u);
                    chunk = (size_t)pick;
                }
            }
            cb_sha256_update(&ctx, buf + consumed, chunk);
            consumed += chunk;
        }

        uint8_t got[CB_SHA256_DIGEST_LEN];
        cb_sha256_final(&ctx, got);

        if (!digest_eq(got, want)) {
            fprintf(stderr, "FAIL: fuzz iter=%d len=%zu chunks=%u\n",
                    iter, len, (unsigned)n_chunks);
            fail_digest_mismatch("fuzz", got, want);
            printf("FAIL: section 11 fuzz\n");
            return 1;
        }
    }

    printf("PASS: section 11 fuzz\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_sha256 tests ===\n");

    int fails = 0;

    if (section1_empty())         fails++;
    if (section2_abc())           fails++;
    if (section3_multi_block())   fails++;
    if (section4_million_a())     fails++;
    if (section5_incremental())   fails++;
    if (section6_single_byte())   fails++;
    if (section7_cross_block())   fails++;
    if (section8_reinit())        fails++;
    /* section 9: documentation-only, no test. */
    if (section10_byte_order())   fails++;
    if (section11_fuzz())         fails++;

    if (fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", fails);
        return 1;
    }

    printf("All hash tests passed!\n");
    return 0;
}
