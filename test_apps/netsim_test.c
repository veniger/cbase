/*
 * netsim_test.c - Tests for the cb_netsim module.
 *
 * Uses a manual clock so scheduling is pinned — no real wall-clock time
 * involved. Each section prints PASS/FAIL and the suite returns 0 iff all
 * pass.
 *
 * Sections:
 *   1.  Create/destroy — zero impairments, bind two endpoints, teardown.
 *   2.  Duplicate bind → CB_INFO_NETSIM_DUPLICATE_ADDR.
 *   3.  Zero-impairment round-trip of 3 datagrams.
 *   4.  Unbound dest is a silent drop (counted in stat_dropped).
 *   5.  100% drop → stat_dropped == 100, stat_delivered == 0.
 *   6.  100% duplicate → 1 send yields 2 recv'd payloads.
 *   7.  Constant latency (50ms) — not ready at 49_999_999, ready at 50_000_000.
 *   8.  Random latency in [10, 20] ms — all 100 delivered by 20ms clock.
 *   9.  100% corruption flips exactly one bit per packet.
 *  10.  100% reorder produces at least one out-of-order pair.
 *  11.  Determinism: same seed + same calls → bit-identical recv stream.
 *  12.  max_queue_bytes caps enqueues.
 *  13.  cb_netsim_set_params rejects bad params and leaves prior params intact.
 *  14.  cb_netsim_flush drops pending + ready; stats preserved.
 *  15.  Arena-backed create + destroy; double destroy is idempotent.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../cbase.h"

#define CHECK(cond, msg) do {                                                 \
    if (!(cond)) {                                                            \
        fprintf(stderr, "FAIL: %s  (at %s:%d)\n", (msg), __FILE__, __LINE__); \
        return 1;                                                             \
    }                                                                         \
} while (0)

/* ---------- manual clock ---------- */

static uint64_t g_clock_ns = 0;
static uint64_t manual_clock(void *u) { (void)u; return g_clock_ns; }

static cb_net_addr_t make_addr(uint32_t ip, uint16_t port)
{
    cb_net_addr_t a;
    a.info = CB_INFO_OK;
    a.ip   = ip;
    a.port = port;
    return a;
}

static cb_netsim_params_t zero_params(void)
{
    cb_netsim_params_t p;
    memset(&p, 0, sizeof(p));
    return p;
}

/* ---------- section 1 ---------- */

static int section1_create_destroy(void)
{
    printf("--- Section 1: create/destroy with two endpoints\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 0x1234ull, zero_params());
    CHECK(net.info == CB_INFO_OK, "create ok");
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_netsim_endpoint_t a = cb_netsim_bind(&net, make_addr(0x7F000001, 1000));
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, make_addr(0x7F000001, 1001));
    CHECK(a.info == CB_INFO_OK, "bind A");
    CHECK(b.info == CB_INFO_OK, "bind B");

    cb_netsim_close(&a);
    cb_netsim_close(&b);
    cb_netsim_destroy(&net);

    printf("  PASS\n");
    return 0;
}

/* ---------- section 2 ---------- */

static int section2_duplicate_bind(void)
{
    printf("--- Section 2: duplicate bind\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 1ull, zero_params());
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_netsim_endpoint_t a  = cb_netsim_bind(&net, make_addr(0x7F000001, 2000));
    cb_netsim_endpoint_t a2 = cb_netsim_bind(&net, make_addr(0x7F000001, 2000));
    CHECK(a.info == CB_INFO_OK, "first bind ok");
    CHECK(a2.info == CB_INFO_NETSIM_DUPLICATE_ADDR, "second bind duplicate");

    cb_netsim_close(&a);
    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 3 ---------- */

static int section3_zero_impairment(void)
{
    printf("--- Section 3: zero-impairment round-trip\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 2ull, zero_params());
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 3000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 3001);

    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    const uint8_t payloads[3][4] = {
        {0xAA, 0xBB, 0xCC, 0xDD},
        {0x11, 0x22, 0x33, 0x44},
        {0xDE, 0xAD, 0xBE, 0xEF},
    };

    for (int i = 0; i < 3; ++i) {
        cb_info_t info = cb_netsim_send_to(&a, baddr, payloads[i], 4);
        CHECK(info == CB_INFO_OK, "send ok");
    }

    uint32_t moved = cb_netsim_step(&net);
    CHECK(moved == 3, "step moved 3");

    for (int i = 0; i < 3; ++i) {
        uint8_t buf[16];
        size_t  got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_OK, "recv ok");
        CHECK(got == 4, "recv len");
        CHECK(memcmp(buf, payloads[i], 4) == 0, "payload match");
        CHECK(src.ip == aaddr.ip && src.port == aaddr.port, "src matches A");
    }

    uint8_t buf[16];
    size_t  got = 0;
    cb_net_addr_t src;
    cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "4th recv EMPTY");

    CHECK(net.stat_enqueued == 3, "enqueued 3");
    CHECK(net.stat_delivered == 3, "delivered 3");
    CHECK(net.stat_dropped == 0, "no drops");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 4 ---------- */

static int section4_unbound_dest(void)
{
    printf("--- Section 4: unbound dest is silent drop\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 3ull, zero_params());
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 4000);
    cb_net_addr_t ghost = make_addr(0x7F000001, 4999);

    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    CHECK(a.info == CB_INFO_OK, "bind A");

    uint8_t p = 0x55;
    cb_info_t info = cb_netsim_send_to(&a, ghost, &p, 1);
    CHECK(info == CB_INFO_OK, "send to ghost returns OK");
    CHECK(net.stat_dropped == 1, "counted as drop");

    cb_netsim_step(&net);

    uint8_t buf[8];
    size_t got = 0;
    cb_net_addr_t src;
    info = cb_netsim_recv_from(&a, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "A recv EMPTY");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 5 ---------- */

static int section5_full_drop(void)
{
    printf("--- Section 5: 100%% drop\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.drop_prob = CB_FX16_ONE;
    cb_netsim_t net = cb_netsim_create(NULL, 4ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x0A000001, 5000);
    cb_net_addr_t baddr = make_addr(0x0A000001, 5001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    for (int i = 0; i < 100; ++i) {
        cb_info_t info = cb_netsim_send_to(&a, baddr, payload, sizeof(payload));
        CHECK(info == CB_INFO_OK, "send ok");
    }

    cb_netsim_step(&net);

    uint8_t buf[32];
    size_t got = 0;
    cb_net_addr_t src;
    cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "recv EMPTY after 100% drop");
    CHECK(net.stat_dropped == 100, "stat_dropped == 100");
    CHECK(net.stat_delivered == 0, "stat_delivered == 0");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 6 ---------- */

static int section6_full_dup(void)
{
    printf("--- Section 6: 100%% duplicate\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.dup_prob = CB_FX16_ONE;
    cb_netsim_t net = cb_netsim_create(NULL, 5ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 6000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 6001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t payload[5] = { 'H','e','l','l','o' };
    CHECK(cb_netsim_send_to(&a, baddr, payload, 5) == CB_INFO_OK, "send");

    cb_netsim_step(&net);

    for (int i = 0; i < 2; ++i) {
        uint8_t buf[16];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_OK, "recv copy");
        CHECK(got == 5, "copy len");
        CHECK(memcmp(buf, payload, 5) == 0, "copy payload");
    }

    uint8_t buf[16];
    size_t got = 0;
    cb_net_addr_t src;
    cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "third recv EMPTY");

    CHECK(net.stat_duplicated == 1, "stat_duplicated == 1");
    CHECK(net.stat_delivered  == 2, "stat_delivered == 2");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 7 ---------- */

static int section7_constant_latency(void)
{
    printf("--- Section 7: constant 50ms latency\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.latency_ms_min = 50;
    p.latency_ms_max = 50;
    cb_netsim_t net = cb_netsim_create(NULL, 6ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 7000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 7001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t payload[3] = {0xAA, 0xBB, 0xCC};
    CHECK(cb_netsim_send_to(&a, baddr, payload, 3) == CB_INFO_OK, "send");

    /* At 49_999_999 ns: not yet ready. */
    g_clock_ns = 49999999ull;
    cb_netsim_step(&net);
    {
        uint8_t buf[8];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_NETSIM_EMPTY, "not ready at 49_999_999 ns");
    }

    /* At 50_000_000 ns: ready. */
    g_clock_ns = 50000000ull;
    cb_netsim_step(&net);
    {
        uint8_t buf[8];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_OK, "ready at 50_000_000 ns");
        CHECK(got == 3, "len");
        CHECK(memcmp(buf, payload, 3) == 0, "payload");
    }

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 8 ---------- */

static int section8_random_latency_range(void)
{
    printf("--- Section 8: random latency in [10, 20] ms\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.latency_ms_min = 10;
    p.latency_ms_max = 20;
    cb_netsim_t net = cb_netsim_create(NULL, 0xBEEFull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 8000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 8001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t p1 = 0x77;
    for (int i = 0; i < 100; ++i) {
        CHECK(cb_netsim_send_to(&a, baddr, &p1, 1) == CB_INFO_OK, "send");
    }

    /* Before 10ms nothing should be ready. */
    g_clock_ns = 9999999ull;
    cb_netsim_step(&net);
    {
        uint8_t buf[4];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_NETSIM_EMPTY, "nothing ready before 10ms");
    }

    /* At 30ms all should be ready. */
    g_clock_ns = 30000000ull;
    cb_netsim_step(&net);

    uint32_t recv_count = 0;
    for (;;) {
        uint8_t buf[4];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        if (info == CB_INFO_NETSIM_EMPTY) break;
        CHECK(info == CB_INFO_OK, "recv ok");
        CHECK(got == 1, "len 1");
        recv_count++;
    }
    CHECK(recv_count == 100, "all 100 delivered by 30ms");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 9 ---------- */

static int section9_corruption_one_bit(void)
{
    printf("--- Section 9: 100%% corruption flips exactly one bit\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.corrupt_prob = CB_FX16_ONE;
    cb_netsim_t net = cb_netsim_create(NULL, 0xC0FFEEull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 9000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 9001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t pattern[32];
    for (int i = 0; i < 32; ++i) pattern[i] = (uint8_t)(0xA5 ^ (uint8_t)i);

    for (int i = 0; i < 100; ++i) {
        CHECK(cb_netsim_send_to(&a, baddr, pattern, 32) == CB_INFO_OK, "send");
    }

    cb_netsim_step(&net);

    for (int i = 0; i < 100; ++i) {
        uint8_t buf[32];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_OK, "recv ok");
        CHECK(got == 32, "len");

        int diff_bits = 0;
        for (int k = 0; k < 32; ++k) {
            uint8_t x = (uint8_t)(buf[k] ^ pattern[k]);
            while (x) { diff_bits += (x & 1); x >>= 1; }
        }
        CHECK(diff_bits == 1, "exactly one bit flipped per packet");
    }

    CHECK(net.stat_corrupted == 100, "stat_corrupted == 100");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 10 ---------- */

static int section10_reorder_inverts(void)
{
    printf("--- Section 10: 100%% reorder produces out-of-order delivery\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.reorder_prob        = CB_FX16_ONE;
    p.reorder_swap_ms_min = 1;
    p.reorder_swap_ms_max = 20;
    cb_netsim_t net = cb_netsim_create(NULL, 0xAA55ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 10000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 10001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    for (uint8_t seq = 0; seq < 10; ++seq) {
        CHECK(cb_netsim_send_to(&a, baddr, &seq, 1) == CB_INFO_OK, "send");
    }

    g_clock_ns = 100000000ull;
    cb_netsim_step(&net);

    uint8_t order[10];
    int n = 0;
    for (; n < 10; ++n) {
        uint8_t buf[4];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
        CHECK(info == CB_INFO_OK, "recv ok");
        CHECK(got == 1, "len");
        order[n] = buf[0];
    }
    CHECK(n == 10, "received 10");

    int monotonic = 1;
    for (int i = 1; i < 10; ++i) {
        if (order[i] < order[i-1]) { monotonic = 0; break; }
    }
    CHECK(!monotonic, "received order must not be monotonic");
    CHECK(net.stat_reordered == 10, "stat_reordered counts rolls (10)");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 11 ---------- */

/* Drive a fixed sequence of sends/steps/recvs on `net` and append every
 * received byte to `out` (up to out_cap bytes). Returns total bytes
 * collected. Used for bit-identical determinism comparison. */
static size_t drive_scenario(cb_netsim_t *net,
                             cb_netsim_endpoint_t *a,
                             cb_netsim_endpoint_t *b,
                             cb_net_addr_t baddr,
                             uint8_t *out, size_t out_cap)
{
    /* Mixed impairments: drop 10%, dup 20%, reorder 30% with 2..5ms swap,
     * latency 1..5ms, corrupt 15%. Same params will be set by caller. */
    uint8_t payload[16];
    for (int i = 0; i < 16; ++i) payload[i] = (uint8_t)(i * 17 + 3);
    size_t wrote = 0;
    for (int round = 0; round < 20; ++round) {
        for (int k = 0; k < 5; ++k) {
            uint8_t buf[16];
            for (int i = 0; i < 16; ++i) buf[i] = (uint8_t)(payload[i] ^ (uint8_t)round ^ (uint8_t)k);
            cb_netsim_send_to(a, baddr, buf, 16);
        }
        g_clock_ns += 3000000ull; /* advance 3ms per round */
        cb_netsim_step(net);
        for (;;) {
            uint8_t rbuf[32];
            size_t got = 0;
            cb_net_addr_t src;
            cb_info_t info = cb_netsim_recv_from(b, &src, rbuf, sizeof(rbuf), &got);
            if (info != CB_INFO_OK) break;
            for (size_t i = 0; i < got && wrote < out_cap; ++i) {
                out[wrote++] = rbuf[i];
            }
        }
    }
    /* Final drain: advance far past any pending. */
    g_clock_ns += 1000000000ull;
    cb_netsim_step(net);
    for (;;) {
        uint8_t rbuf[32];
        size_t got = 0;
        cb_net_addr_t src;
        cb_info_t info = cb_netsim_recv_from(b, &src, rbuf, sizeof(rbuf), &got);
        if (info != CB_INFO_OK) break;
        for (size_t i = 0; i < got && wrote < out_cap; ++i) {
            out[wrote++] = rbuf[i];
        }
    }
    return wrote;
}

static cb_netsim_params_t mixed_params(void)
{
    cb_netsim_params_t p = zero_params();
    p.drop_prob           = (cb_fx16_t)(CB_FX16_ONE / 10);
    p.dup_prob            = (cb_fx16_t)(CB_FX16_ONE / 5);
    p.corrupt_prob        = (cb_fx16_t)((CB_FX16_ONE * 15) / 100);
    p.latency_ms_min      = 1;
    p.latency_ms_max      = 5;
    p.reorder_prob        = (cb_fx16_t)((CB_FX16_ONE * 30) / 100);
    p.reorder_swap_ms_min = 2;
    p.reorder_swap_ms_max = 5;
    return p;
}

static int section11_determinism(void)
{
    printf("--- Section 11: determinism (two nets with same seed)\n");

    const uint64_t seed = 0xDEADBEEFCAFEBABEull;
    uint8_t buf1[8192];
    uint8_t buf2[8192];
    size_t  n1 = 0, n2 = 0;

    {
        g_clock_ns = 0;
        cb_netsim_t net = cb_netsim_create(NULL, seed, mixed_params());
        cb_netsim_set_clock(&net, manual_clock, NULL);
        cb_net_addr_t aaddr = make_addr(0x7F000001, 11000);
        cb_net_addr_t baddr = make_addr(0x7F000001, 11001);
        cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
        cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
        CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds (net1)");
        n1 = drive_scenario(&net, &a, &b, baddr, buf1, sizeof(buf1));
        cb_netsim_destroy(&net);
    }
    {
        g_clock_ns = 0;
        cb_netsim_t net = cb_netsim_create(NULL, seed, mixed_params());
        cb_netsim_set_clock(&net, manual_clock, NULL);
        cb_net_addr_t aaddr = make_addr(0x7F000001, 11000);
        cb_net_addr_t baddr = make_addr(0x7F000001, 11001);
        cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
        cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
        CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds (net2)");
        n2 = drive_scenario(&net, &a, &b, baddr, buf2, sizeof(buf2));
        cb_netsim_destroy(&net);
    }

    CHECK(n1 > 0, "drove at least one byte");
    CHECK(n1 == n2, "deterministic byte-count match");
    CHECK(memcmp(buf1, buf2, n1) == 0, "deterministic byte-for-byte match");

    printf("  PASS (%zu bytes identical across two runs)\n", n1);
    return 0;
}

/* ---------- section 12 ---------- */

static int section12_queue_cap(void)
{
    printf("--- Section 12: max_queue_bytes cap\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.max_queue_bytes = 100;
    cb_netsim_t net = cb_netsim_create(NULL, 7ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 12000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 12001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t chunk[40];
    memset(chunk, 0xAB, sizeof(chunk));
    for (int i = 0; i < 5; ++i) {
        cb_info_t info = cb_netsim_send_to(&a, baddr, chunk, sizeof(chunk));
        CHECK(info == CB_INFO_OK, "send ok");
    }

    CHECK(net.stat_enqueued == 5, "enqueued 5");
    CHECK(net.stat_queue_full == 3, "3 dropped at enqueue (40+40=80 ok, +40=120>100)");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 13 ---------- */

static int section13_bad_params(void)
{
    printf("--- Section 13: set_params rejects bad params\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.latency_ms_min = 5;
    p.latency_ms_max = 10;
    cb_netsim_t net = cb_netsim_create(NULL, 8ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);
    CHECK(net.info == CB_INFO_OK, "create ok");

    cb_netsim_params_t bad = p;
    bad.latency_ms_min = 100;
    bad.latency_ms_max = 10;

    cb_info_t info = cb_netsim_set_params(&net, bad);
    CHECK(info == CB_INFO_NETSIM_BAD_PARAMS, "bad params rejected");
    CHECK(net.params.latency_ms_min == 5, "original min preserved");
    CHECK(net.params.latency_ms_max == 10, "original max preserved");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 14 ---------- */

static int section14_flush(void)
{
    printf("--- Section 14: flush clears queues; stats preserved\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.latency_ms_min = 50;
    p.latency_ms_max = 50;
    cb_netsim_t net = cb_netsim_create(NULL, 9ull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 14000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 14001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    for (int i = 0; i < 10; ++i) {
        uint8_t x = (uint8_t)i;
        CHECK(cb_netsim_send_to(&a, baddr, &x, 1) == CB_INFO_OK, "send");
    }
    CHECK(net.stat_enqueued == 10, "enqueued 10");

    cb_netsim_flush(&net);

    /* Advance past original delivery time and step; should still be empty. */
    g_clock_ns = 1000000000ull;
    uint32_t moved = cb_netsim_step(&net);
    CHECK(moved == 0, "step after flush moves nothing");

    uint8_t buf[4];
    size_t got = 0;
    cb_net_addr_t src;
    cb_info_t info = cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "recv EMPTY after flush");

    /* Stats preserved (enqueue counter stays). */
    CHECK(net.stat_enqueued == 10, "stat_enqueued preserved");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 15 ---------- */

static int section15_arena_backed(void)
{
    printf("--- Section 15: arena-backed create + destroy (idempotent)\n");
    g_clock_ns = 0;

    cb_arena_t arena = cb_arena_create(64 * 1024, CB_ARENA_LINEAR);
    CHECK(arena.info == CB_INFO_OK, "arena create");

    cb_netsim_t net = cb_netsim_create(&arena, 10ull, zero_params());
    CHECK(net.info == CB_INFO_OK, "netsim create");
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 15000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 15001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t p[4] = {1,2,3,4};
    CHECK(cb_netsim_send_to(&a, baddr, p, 4) == CB_INFO_OK, "send");
    cb_netsim_step(&net);

    uint8_t buf[8];
    size_t got = 0;
    cb_net_addr_t src;
    CHECK(cb_netsim_recv_from(&b, &src, buf, sizeof(buf), &got) == CB_INFO_OK, "recv");
    CHECK(got == 4 && memcmp(buf, p, 4) == 0, "payload");

    cb_netsim_destroy(&net);
    /* Second destroy is idempotent — fields are nulled on first destroy. */
    cb_netsim_destroy(&net);

    CHECK(cb_arena_check_health(&arena), "arena guards intact");
    cb_arena_destroy(&arena);

    printf("  PASS\n");
    return 0;
}

/* ---------- section 16 ---------- */

/* Regression: closing an endpoint must scrub pending datagrams addressed to
 * it and hard-delete the endpoint node. Otherwise a later bind on the same
 * addr receives packets that belonged to the old endpoint. */
static int section16_close_scrubs_pending_and_rebind(void)
{
    printf("--- Section 16: close scrubs pending; rebind does not inherit\n");
    g_clock_ns = 0;
    cb_netsim_params_t p = zero_params();
    p.latency_ms_min = 50;
    p.latency_ms_max = 50;
    cb_netsim_t net = cb_netsim_create(NULL, 0xABCDull, p);
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 16000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 16001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
    CHECK(cb_netsim_send_to(&a, baddr, payload, 4) == CB_INFO_OK, "send");
    CHECK(net.stat_enqueued == 1, "enqueued 1");
    CHECK(net.stat_dropped  == 0, "no drops yet");

    /* Close B while the datagram is still pending. */
    cb_netsim_close(&b);
    CHECK(net.stat_dropped == 1, "close scrubbed the pending datagram");
    CHECK(b.net == NULL,         "close cleared handle net ptr");

    /* Rebind B at the same addr while clock has not yet reached delivery time. */
    g_clock_ns = 10000000ull;
    cb_netsim_endpoint_t b2 = cb_netsim_bind(&net, baddr);
    CHECK(b2.info == CB_INFO_OK, "rebind B");

    /* Advance past original delivery time. The scrubbed packet must NOT
     * appear on the new endpoint's FIFO. */
    g_clock_ns = 100000000ull;
    uint32_t moved = cb_netsim_step(&net);
    CHECK(moved == 0, "step after rebind moves nothing");

    uint8_t buf[16];
    size_t got = 0;
    cb_net_addr_t src;
    cb_info_t info = cb_netsim_recv_from(&b2, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "new B sees no orphan datagram");

    /* And a fresh round-trip on the new endpoint works as normal. */
    CHECK(cb_netsim_send_to(&a, baddr, payload, 4) == CB_INFO_OK, "fresh send");
    g_clock_ns = 100000000ull + 50000000ull;
    cb_netsim_step(&net);
    info = cb_netsim_recv_from(&b2, &src, buf, sizeof(buf), &got);
    CHECK(info == CB_INFO_OK && got == 4, "fresh delivery works");
    CHECK(memcmp(buf, payload, 4) == 0, "fresh payload matches");

    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 17 ---------- */

/* send_to rejects len > CB_NETSIM_MAX_DATAGRAM_BYTES with no side effects,
 * and accepts len == cap. */
static int section17_payload_size_cap(void)
{
    printf("--- Section 17: payload size cap\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 0xF00Dull, zero_params());
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001, 17000);
    cb_net_addr_t baddr = make_addr(0x7F000001, 17001);
    cb_netsim_endpoint_t a = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t b = cb_netsim_bind(&net, baddr);
    CHECK(a.info == CB_INFO_OK && b.info == CB_INFO_OK, "binds");

    /* Legal: exactly CB_NETSIM_MAX_DATAGRAM_BYTES. */
    size_t legal_len = CB_NETSIM_MAX_DATAGRAM_BYTES;
    uint8_t *legal = (uint8_t *)malloc(legal_len);
    CHECK(legal != NULL, "alloc legal buf");
    for (size_t i = 0; i < legal_len; ++i) legal[i] = (uint8_t)(i & 0xFFu);
    cb_info_t info = cb_netsim_send_to(&a, baddr, legal, legal_len);
    CHECK(info == CB_INFO_OK, "send @cap ok");
    CHECK(net.stat_enqueued == 1, "enqueued");

    cb_netsim_step(&net);
    uint8_t *rbuf = (uint8_t *)malloc(legal_len);
    CHECK(rbuf != NULL, "alloc recv buf");
    size_t got = 0;
    cb_net_addr_t src;
    info = cb_netsim_recv_from(&b, &src, rbuf, legal_len, &got);
    CHECK(info == CB_INFO_OK, "recv @cap ok");
    CHECK(got == legal_len, "recv @cap len");
    CHECK(memcmp(rbuf, legal, legal_len) == 0, "payload @cap matches");

    /* Illegal: cap + 1. Must NOT touch stat_enqueued/stat_dropped. */
    uint32_t enq_before  = net.stat_enqueued;
    uint32_t drop_before = net.stat_dropped;
    size_t   over_len    = CB_NETSIM_MAX_DATAGRAM_BYTES + 1u;
    uint8_t *over        = (uint8_t *)malloc(over_len);
    CHECK(over != NULL, "alloc over buf");
    memset(over, 0x5Au, over_len);
    info = cb_netsim_send_to(&a, baddr, over, over_len);
    CHECK(info == CB_INFO_NETSIM_PAYLOAD_TOO_LARGE, "over-cap rejected");
    CHECK(net.stat_enqueued == enq_before,  "no enqueue on reject");
    CHECK(net.stat_dropped  == drop_before, "no drop on reject");

    cb_netsim_step(&net);
    info = cb_netsim_recv_from(&b, &src, rbuf, legal_len, &got);
    CHECK(info == CB_INFO_NETSIM_EMPTY, "nothing delivered from over-cap send");

    free(legal);
    free(rbuf);
    free(over);
    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== cb_netsim tests ===\n");
    int fails = 0;

    if (section1_create_destroy())       fails++;
    if (section2_duplicate_bind())       fails++;
    if (section3_zero_impairment())      fails++;
    if (section4_unbound_dest())         fails++;
    if (section5_full_drop())            fails++;
    if (section6_full_dup())             fails++;
    if (section7_constant_latency())     fails++;
    if (section8_random_latency_range()) fails++;
    if (section9_corruption_one_bit())   fails++;
    if (section10_reorder_inverts())     fails++;
    if (section11_determinism())         fails++;
    if (section12_queue_cap())           fails++;
    if (section13_bad_params())          fails++;
    if (section14_flush())               fails++;
    if (section15_arena_backed())        fails++;
    if (section16_close_scrubs_pending_and_rebind()) fails++;
    if (section17_payload_size_cap())    fails++;

    if (fails != 0) {
        fprintf(stderr, "FAIL: %d section(s) failed\n", fails);
        return 1;
    }

    printf("All netsim tests passed!\n");
    return 0;
}
