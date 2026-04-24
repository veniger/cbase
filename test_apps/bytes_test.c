/*
 * bytes_test.c - Tests for the cb_bytes module.
 *
 * Covers:
 *   1. Round-trip of every primitive (u8..u64 and i8..i64).
 *   2. Little-endian wire format.
 *   3. Writer bounds error + sticky info.
 *   4. Reader bounds error + sticky info.
 *   5. Frame round-trip with mixed content.
 *   6. Frame-too-large failure path.
 *   7. Empty frame.
 *   8. Fuzz-ish smoke: 100 random frames through a 1 KB buffer.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../cbase.h"

/* ---------- helpers ---------- */

static int g_fails = 0;

#define CHECK(cond, msg) do {                                              \
    if (!(cond)) {                                                         \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__); \
        return 1;                                                          \
    }                                                                      \
} while (0)

/* ---------- section 1: primitive round-trips ---------- */

static int section1_primitives(void)
{
    printf("--- Section 1: primitive round-trips\n");

    uint8_t buf[256];
    cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));

    const uint8_t  u8s[]  = { 0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu };
    const uint16_t u16s[] = { 0x0000u, 0x0001u, 0x7FFFu, 0x8000u, 0xFFFFu };
    const uint32_t u32s[] = { 0x00000000u, 0x00000001u, 0x7FFFFFFFu,
                              0x80000000u, 0xFFFFFFFFu };
    const uint64_t u64s[] = { 0x0000000000000000ull, 0x0000000000000001ull,
                              0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull,
                              0xFFFFFFFFFFFFFFFFull };

    const int8_t  i8s[]  = { 0, 1, -1, INT8_MIN,  INT8_MAX  };
    const int16_t i16s[] = { 0, 1, -1, INT16_MIN, INT16_MAX };
    const int32_t i32s[] = { 0, 1, -1, INT32_MIN, INT32_MAX };
    const int64_t i64s[] = { 0, 1, -1, INT64_MIN, INT64_MAX };

    for (size_t i = 0; i < sizeof(u8s)  / sizeof(u8s[0]);  ++i)
        CHECK(cb_bytes_write_u8 (&w, u8s[i])  == CB_INFO_OK, "write u8");
    for (size_t i = 0; i < sizeof(u16s) / sizeof(u16s[0]); ++i)
        CHECK(cb_bytes_write_u16_le(&w, u16s[i]) == CB_INFO_OK, "write u16");
    for (size_t i = 0; i < sizeof(u32s) / sizeof(u32s[0]); ++i)
        CHECK(cb_bytes_write_u32_le(&w, u32s[i]) == CB_INFO_OK, "write u32");
    for (size_t i = 0; i < sizeof(u64s) / sizeof(u64s[0]); ++i)
        CHECK(cb_bytes_write_u64_le(&w, u64s[i]) == CB_INFO_OK, "write u64");

    for (size_t i = 0; i < sizeof(i8s)  / sizeof(i8s[0]);  ++i)
        CHECK(cb_bytes_write_i8 (&w, i8s[i])  == CB_INFO_OK, "write i8");
    for (size_t i = 0; i < sizeof(i16s) / sizeof(i16s[0]); ++i)
        CHECK(cb_bytes_write_i16_le(&w, i16s[i]) == CB_INFO_OK, "write i16");
    for (size_t i = 0; i < sizeof(i32s) / sizeof(i32s[0]); ++i)
        CHECK(cb_bytes_write_i32_le(&w, i32s[i]) == CB_INFO_OK, "write i32");
    for (size_t i = 0; i < sizeof(i64s) / sizeof(i64s[0]); ++i)
        CHECK(cb_bytes_write_i64_le(&w, i64s[i]) == CB_INFO_OK, "write i64");

    CHECK(w.info == CB_INFO_OK, "writer info must be OK after all writes");

    cb_bytes_reader_t r = cb_bytes_reader_init(buf, cb_bytes_writer_len(&w));

    for (size_t i = 0; i < sizeof(u8s)  / sizeof(u8s[0]);  ++i) {
        uint8_t v;
        CHECK(cb_bytes_read_u8(&r, &v) == CB_INFO_OK, "read u8");
        CHECK(v == u8s[i], "u8 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(u16s) / sizeof(u16s[0]); ++i) {
        uint16_t v;
        CHECK(cb_bytes_read_u16_le(&r, &v) == CB_INFO_OK, "read u16");
        CHECK(v == u16s[i], "u16 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(u32s) / sizeof(u32s[0]); ++i) {
        uint32_t v;
        CHECK(cb_bytes_read_u32_le(&r, &v) == CB_INFO_OK, "read u32");
        CHECK(v == u32s[i], "u32 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(u64s) / sizeof(u64s[0]); ++i) {
        uint64_t v;
        CHECK(cb_bytes_read_u64_le(&r, &v) == CB_INFO_OK, "read u64");
        CHECK(v == u64s[i], "u64 round-trip mismatch");
    }

    for (size_t i = 0; i < sizeof(i8s)  / sizeof(i8s[0]);  ++i) {
        int8_t v;
        CHECK(cb_bytes_read_i8(&r, &v) == CB_INFO_OK, "read i8");
        CHECK(v == i8s[i], "i8 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(i16s) / sizeof(i16s[0]); ++i) {
        int16_t v;
        CHECK(cb_bytes_read_i16_le(&r, &v) == CB_INFO_OK, "read i16");
        CHECK(v == i16s[i], "i16 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(i32s) / sizeof(i32s[0]); ++i) {
        int32_t v;
        CHECK(cb_bytes_read_i32_le(&r, &v) == CB_INFO_OK, "read i32");
        CHECK(v == i32s[i], "i32 round-trip mismatch");
    }
    for (size_t i = 0; i < sizeof(i64s) / sizeof(i64s[0]); ++i) {
        int64_t v;
        CHECK(cb_bytes_read_i64_le(&r, &v) == CB_INFO_OK, "read i64");
        CHECK(v == i64s[i], "i64 round-trip mismatch");
    }

    CHECK(cb_bytes_reader_remaining(&r) == 0, "reader should be fully consumed");
    CHECK(r.info == CB_INFO_OK, "reader info must be OK after consumption");

    printf("  primitives OK\n");
    return 0;
}

/* ---------- section 2: endianness ---------- */

static int section2_endianness(void)
{
    printf("--- Section 2: little-endian wire format\n");

    uint8_t buf[16];

    {
        cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));
        CHECK(cb_bytes_write_u16_le(&w, 0x1234u) == CB_INFO_OK, "write u16");
        CHECK(buf[0] == 0x34u, "u16 byte0 low");
        CHECK(buf[1] == 0x12u, "u16 byte1 high");
    }

    {
        cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));
        CHECK(cb_bytes_write_u32_le(&w, 0xDEADBEEFu) == CB_INFO_OK, "write u32");
        CHECK(buf[0] == 0xEFu, "u32 byte0");
        CHECK(buf[1] == 0xBEu, "u32 byte1");
        CHECK(buf[2] == 0xADu, "u32 byte2");
        CHECK(buf[3] == 0xDEu, "u32 byte3");
    }

    {
        cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));
        CHECK(cb_bytes_write_u64_le(&w, 0x0123456789ABCDEFull) == CB_INFO_OK, "write u64");
        CHECK(buf[0] == 0xEFu, "u64 byte0");
        CHECK(buf[1] == 0xCDu, "u64 byte1");
        CHECK(buf[2] == 0xABu, "u64 byte2");
        CHECK(buf[3] == 0x89u, "u64 byte3");
        CHECK(buf[4] == 0x67u, "u64 byte4");
        CHECK(buf[5] == 0x45u, "u64 byte5");
        CHECK(buf[6] == 0x23u, "u64 byte6");
        CHECK(buf[7] == 0x01u, "u64 byte7");
    }

    printf("  endianness OK\n");
    return 0;
}

/* ---------- section 3: writer bounds + sticky info ---------- */

static int section3_writer_bounds(void)
{
    printf("--- Section 3: writer out-of-bounds + sticky info\n");

    uint8_t buf[4];
    cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));

    CHECK(cb_bytes_write_u32_le(&w, 0xAABBCCDDu) == CB_INFO_OK, "u32 should fit");
    CHECK(w.info == CB_INFO_OK, "info still OK after exact-fit write");
    CHECK(cb_bytes_writer_len(&w) == 4, "writer len == 4 after u32");

    size_t pos_before = w.pos;
    cb_info_t info = cb_bytes_write_u8(&w, 0x11u);
    CHECK(info == CB_INFO_BYTES_OUT_OF_BOUNDS, "write past cap must be OOB");
    CHECK(w.info == CB_INFO_BYTES_OUT_OF_BOUNDS, "info must stick OOB");
    CHECK(w.pos == pos_before, "pos must not advance on OOB");

    /* Sticky: a subsequent write that would otherwise succeed still short-
       circuits and returns the stuck info. Use a big enough buffer-relative
       no-op shape to confirm — we reset pos to 0 mentally, but sticky info
       overrides. Trying a 0-byte write_bytes still returns the sticky info. */
    info = cb_bytes_write_bytes(&w, buf, 0);
    CHECK(info == CB_INFO_BYTES_OUT_OF_BOUNDS, "sticky info short-circuits 0-byte write");
    CHECK(w.pos == pos_before, "pos still unchanged after sticky short-circuit");

    printf("  writer bounds OK\n");
    return 0;
}

/* ---------- section 4: reader bounds + sticky info ---------- */

static int section4_reader_bounds(void)
{
    printf("--- Section 4: reader out-of-bounds + sticky info\n");

    const uint8_t buf[3] = { 0x11u, 0x22u, 0x33u };
    cb_bytes_reader_t r = cb_bytes_reader_init(buf, sizeof(buf));

    uint32_t v;
    cb_info_t info = cb_bytes_read_u32_le(&r, &v);
    CHECK(info == CB_INFO_BYTES_OUT_OF_BOUNDS, "u32 read on 3-byte buffer must OOB");
    CHECK(r.info == CB_INFO_BYTES_OUT_OF_BOUNDS, "reader info must stick OOB");
    CHECK(r.pos == 0, "pos must not advance on OOB");

    /* Sticky: a later read of a byte that does fit still returns the stuck
       info because the reader is poisoned. */
    uint8_t b;
    info = cb_bytes_read_u8(&r, &b);
    CHECK(info == CB_INFO_BYTES_OUT_OF_BOUNDS, "sticky info short-circuits later u8");
    CHECK(r.pos == 0, "pos unchanged after sticky short-circuit");

    printf("  reader bounds OK\n");
    return 0;
}

/* ---------- section 5: frame round-trip ---------- */

static int section5_frame_roundtrip(void)
{
    printf("--- Section 5: frame round-trip\n");

    uint8_t buf[64];
    cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));

    CHECK(cb_bytes_write_u8(&w, 0xAAu) == CB_INFO_OK, "pre-frame sentinel");

    size_t mark = 0;
    CHECK(cb_bytes_begin_frame_u16(&w, &mark) == CB_INFO_OK, "begin frame");
    CHECK(cb_bytes_write_u8(&w, 0x01u) == CB_INFO_OK, "frame body: u8");
    CHECK(cb_bytes_write_u16_le(&w, 0x0203u) == CB_INFO_OK, "frame body: u16");
    CHECK(cb_bytes_write_bytes(&w, "hello", 5) == CB_INFO_OK, "frame body: bytes");
    CHECK(cb_bytes_end_frame_u16(&w, mark) == CB_INFO_OK, "end frame");

    CHECK(cb_bytes_write_u8(&w, 0xBBu) == CB_INFO_OK, "post-frame sentinel");
    CHECK(w.info == CB_INFO_OK, "writer info OK after frame");

    /* Expected frame body length = 1 (u8) + 2 (u16) + 5 (bytes) = 8. */
    CHECK(buf[mark + 0] == 0x08u, "length prefix lo byte");
    CHECK(buf[mark + 1] == 0x00u, "length prefix hi byte");

    cb_bytes_reader_t r = cb_bytes_reader_init(buf, cb_bytes_writer_len(&w));

    uint8_t pre;
    CHECK(cb_bytes_read_u8(&r, &pre) == CB_INFO_OK, "read pre sentinel");
    CHECK(pre == 0xAAu, "pre sentinel value");

    cb_bytes_reader_t sub;
    CHECK(cb_bytes_read_frame_u16(&r, &sub) == CB_INFO_OK, "read frame");
    CHECK(sub.len == 8, "sub-reader length");

    uint8_t  b1;
    uint16_t s1;
    uint8_t  payload[5];
    CHECK(cb_bytes_read_u8(&sub, &b1) == CB_INFO_OK, "sub u8");
    CHECK(b1 == 0x01u, "sub u8 value");
    CHECK(cb_bytes_read_u16_le(&sub, &s1) == CB_INFO_OK, "sub u16");
    CHECK(s1 == 0x0203u, "sub u16 value");
    CHECK(cb_bytes_read_bytes(&sub, payload, 5) == CB_INFO_OK, "sub bytes");
    CHECK(memcmp(payload, "hello", 5) == 0, "sub bytes value");
    CHECK(cb_bytes_reader_remaining(&sub) == 0, "sub fully consumed");

    uint8_t post;
    CHECK(cb_bytes_read_u8(&r, &post) == CB_INFO_OK, "read post sentinel");
    CHECK(post == 0xBBu, "post sentinel value");
    CHECK(cb_bytes_reader_remaining(&r) == 0, "outer fully consumed");

    printf("  frame round-trip OK\n");
    return 0;
}

/* ---------- section 6: frame too large ---------- */

static int section6_frame_too_large(void)
{
    printf("--- Section 6: frame too large\n");

    size_t cap = 70000;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL, "malloc 70000 bytes");

    cb_bytes_writer_t w = cb_bytes_writer_init(buf, cap);
    size_t mark = 0;
    CHECK(cb_bytes_begin_frame_u16(&w, &mark) == CB_INFO_OK, "begin frame");

    /* Write 65536 zero bytes — one past UINT16_MAX. */
    uint8_t chunk[1024];
    memset(chunk, 0, sizeof(chunk));
    size_t remaining = 65536;
    while (remaining > 0) {
        size_t n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        CHECK(cb_bytes_write_bytes(&w, chunk, n) == CB_INFO_OK, "write zero chunk");
        remaining -= n;
    }
    CHECK(w.info == CB_INFO_OK, "writer info before end_frame");

    cb_info_t info = cb_bytes_end_frame_u16(&w, mark);
    CHECK(info == CB_INFO_BYTES_FRAME_TOO_LARGE, "end_frame must error");
    CHECK(w.info == CB_INFO_BYTES_FRAME_TOO_LARGE, "info must stick FRAME_TOO_LARGE");

    free(buf);
    printf("  frame too large OK\n");
    return 0;
}

/* ---------- section 7: empty frame ---------- */

static int section7_empty_frame(void)
{
    printf("--- Section 7: empty frame\n");

    uint8_t buf[16];
    cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));

    size_t mark = 0;
    CHECK(cb_bytes_begin_frame_u16(&w, &mark) == CB_INFO_OK, "begin");
    CHECK(cb_bytes_end_frame_u16(&w, mark)   == CB_INFO_OK, "end empty");
    CHECK(cb_bytes_writer_len(&w) == 2, "empty frame writes only 2 bytes");
    CHECK(buf[0] == 0x00u && buf[1] == 0x00u, "empty frame length bytes are zero");

    cb_bytes_reader_t r = cb_bytes_reader_init(buf, cb_bytes_writer_len(&w));
    cb_bytes_reader_t sub;
    CHECK(cb_bytes_read_frame_u16(&r, &sub) == CB_INFO_OK, "read empty frame");
    CHECK(cb_bytes_reader_remaining(&sub) == 0, "empty sub-reader remaining == 0");
    CHECK(sub.info == CB_INFO_OK, "empty sub info OK");
    CHECK(cb_bytes_reader_remaining(&r) == 0, "outer fully consumed");

    printf("  empty frame OK\n");
    return 0;
}

/* ---------- section 8: fuzz-ish smoke test ---------- */

#define FUZZ_BUF_SIZE   1024
#define FUZZ_FRAMES     100
#define FUZZ_MAX_PAYLOAD 16

static int section8_fuzz_smoke(void)
{
    printf("--- Section 8: fuzz-ish smoke (100 random frames)\n");

    uint8_t buf[FUZZ_BUF_SIZE];

    /* Generate frames with cb_random, record them, then write them through
       the writer, and read them back through the reader. */
    cb_rng_t gen = cb_rng_seed(0xB17E51DEu, 0xF00Du);

    /* Record expected payloads. */
    uint8_t  payloads[FUZZ_FRAMES][FUZZ_MAX_PAYLOAD];
    uint16_t lens[FUZZ_FRAMES];
    size_t   frames_written = 0;

    cb_bytes_writer_t w = cb_bytes_writer_init(buf, sizeof(buf));

    for (size_t i = 0; i < FUZZ_FRAMES; ++i) {
        uint32_t len32 = cb_rng_u32_below(&gen, FUZZ_MAX_PAYLOAD + 1u);
        uint16_t len = (uint16_t)len32;
        for (uint16_t k = 0; k < len; ++k) {
            payloads[i][k] = (uint8_t)(cb_rng_u32(&gen) & 0xFFu);
        }

        /* Need 2 (length prefix) + len bytes in the buffer. */
        if (w.pos + 2 + (size_t)len > w.cap) break;

        size_t mark;
        CHECK(cb_bytes_begin_frame_u16(&w, &mark) == CB_INFO_OK, "fuzz begin");
        if (len > 0) {
            CHECK(cb_bytes_write_bytes(&w, payloads[i], len) == CB_INFO_OK, "fuzz body");
        }
        CHECK(cb_bytes_end_frame_u16(&w, mark) == CB_INFO_OK, "fuzz end");

        lens[i] = len;
        frames_written++;
    }

    CHECK(w.info == CB_INFO_OK, "fuzz writer info OK");
    CHECK(frames_written > 0, "fuzz wrote at least one frame");

    cb_bytes_reader_t r = cb_bytes_reader_init(buf, cb_bytes_writer_len(&w));
    for (size_t i = 0; i < frames_written; ++i) {
        cb_bytes_reader_t sub;
        CHECK(cb_bytes_read_frame_u16(&r, &sub) == CB_INFO_OK, "fuzz read frame");
        CHECK(sub.len == (size_t)lens[i], "fuzz frame length mismatch");

        uint8_t got[FUZZ_MAX_PAYLOAD];
        if (lens[i] > 0) {
            CHECK(cb_bytes_read_bytes(&sub, got, lens[i]) == CB_INFO_OK, "fuzz read body");
            CHECK(memcmp(got, payloads[i], lens[i]) == 0, "fuzz payload mismatch");
        }
        CHECK(cb_bytes_reader_remaining(&sub) == 0, "fuzz sub fully consumed");
    }
    CHECK(cb_bytes_reader_remaining(&r) == 0, "fuzz outer fully consumed");
    CHECK(r.info == CB_INFO_OK, "fuzz reader info OK");

    printf("  fuzz smoke OK (%zu frames)\n", frames_written);
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_bytes tests ===\n");

    if (section1_primitives())       { g_fails++; }
    if (section2_endianness())       { g_fails++; }
    if (section3_writer_bounds())    { g_fails++; }
    if (section4_reader_bounds())    { g_fails++; }
    if (section5_frame_roundtrip())  { g_fails++; }
    if (section6_frame_too_large())  { g_fails++; }
    if (section7_empty_frame())      { g_fails++; }
    if (section8_fuzz_smoke())       { g_fails++; }

    if (g_fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", g_fails);
        return 1;
    }

    printf("All bytes tests passed!\n");
    return 0;
}
