/*
 * transport_test.c — exercises cb_transport_t through both adapters.
 *
 * The point of cb_transport is that a caller holding only `cb_transport_t *`
 * cannot tell whether the wire is real UDP or cb_netsim. The same probe
 * function `do_round_trip` is therefore called against both adapters and
 * MUST succeed identically.
 *
 * Sections:
 *   1. UDP adapter — bind two real UDP sockets on 127.0.0.1, send/recv,
 *                    verify round-trip + WOULD_BLOCK on empty.
 *   2. NetSim adapter — bind two virtual endpoints, send/recv, verify
 *                       round-trip + WOULD_BLOCK on empty (mapped from
 *                       CB_INFO_NETSIM_EMPTY).
 *   3. Polymorphic — drive the same probe with a `cb_transport_t *` of
 *                    each backend in succession; both pass.
 *   4. Bad-args — NULL / zero-out factories yield non-OK info; dispatchers
 *                 on a NULL transport return GENERIC_ERROR.
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

/* ---------- manual clock for netsim ---------- */
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

/* ---------- backend-agnostic probe ----------
 *
 * Drives one transport (`tx_send`) -> the matching backend wire -> the other
 * transport (`tx_recv`) and verifies the payload survives intact. For UDP
 * this is a real loopback datagram; for netsim we step the sim between
 * send and recv. The optional `step_fn` lets the caller advance the netsim
 * clock; pass NULL for the udp path. */
typedef void (*step_fn_t)(void *user);

static int do_round_trip(cb_transport_t *tx_send,
                         cb_transport_t *tx_recv,
                         cb_net_addr_t   dest,
                         step_fn_t       step_fn,
                         void           *step_user)
{
    const char     payload[] = "hello transport";
    const size_t   n         = sizeof(payload);

    cb_net_io_result_t sr = cb_transport_send(tx_send, dest, payload, n);
    CHECK(sr.info == CB_INFO_OK, "send ok");
    CHECK(sr.bytes == n, "send bytes match");

    if (step_fn) step_fn(step_user);

    /* Drain — UDP loopback may need a few polls before the kernel hands us
     * back the datagram. NetSim hands it back immediately after step. */
    uint8_t              buf[256];
    cb_udp_recv_result_t rr;
    rr.info = CB_INFO_NET_WOULD_BLOCK;
    for (int tries = 0; tries < 1000; ++tries) {
        rr = cb_transport_recv(tx_recv, buf, sizeof(buf));
        if (rr.info == CB_INFO_OK) break;
        if (rr.info != CB_INFO_NET_WOULD_BLOCK) break;
    }
    CHECK(rr.info == CB_INFO_OK, "recv ok");
    CHECK(rr.size == n, "recv size match");
    CHECK(memcmp(buf, payload, n) == 0, "payload bytes match");

    /* Empty queue should now be WOULD_BLOCK on both backends. */
    if (step_fn) step_fn(step_user);
    cb_udp_recv_result_t rr2 = cb_transport_recv(tx_recv, buf, sizeof(buf));
    CHECK(rr2.info == CB_INFO_NET_WOULD_BLOCK, "empty recv -> WOULD_BLOCK");
    return 0;
}

/* ---------- section 1: UDP adapter ---------- */

static int section1_udp(void)
{
    printf("--- Section 1: UDP adapter\n");
    CHECK(cb_net_init() == CB_INFO_OK, "net_init");

    /* Use ephemeral ports and read them back so we don't race CI port use.
     * cb_udp_open binds; we then need the actual bound port. The header
     * doesn't expose getsockname, so use known free ports for this test
     * (loopback only, sanitizers handle re-bind via SO_REUSEADDR). */
    cb_udp_socket_t sa = cb_udp_open(0u);
    CHECK(sa.info == CB_INFO_OK, "open A");
    cb_udp_socket_t sb = cb_udp_open(0u);
    CHECK(sb.info == CB_INFO_OK, "open B");

    /* Discover the port B was bound to via getsockname directly, since cb
     * doesn't expose it. */
#ifdef CB_PLATFORM_POSIX
    struct sockaddr_in name;
    socklen_t          nlen = sizeof(name);
    memset(&name, 0, sizeof(name));
    CHECK(getsockname(sb.handle, (struct sockaddr *)&name, &nlen) == 0,
          "getsockname B");
    uint16_t b_port = ntohs(name.sin_port);
#else
    uint16_t b_port = 0u; /* test only meaningful on POSIX hosts; CI is darwin */
    CHECK(0, "POSIX-only path in this test");
#endif

    cb_transport_t txa = cb_transport_udp(&sa);
    cb_transport_t txb = cb_transport_udp(&sb);
    CHECK(txa.info == CB_INFO_OK, "factory A");
    CHECK(txb.info == CB_INFO_OK, "factory B");

    cb_net_addr_t to_b = make_addr(0x7F000001u, b_port);
    int rc = do_round_trip(&txa, &txb, to_b, NULL, NULL);
    if (rc != 0) {
        cb_transport_close(&txa);
        cb_transport_close(&txb);
        cb_net_shutdown();
        return rc;
    }

    cb_transport_close(&txa);
    cb_transport_close(&txb);
    /* Closing twice via cb_udp_close is a no-op (handle == INVALID); the
     * transport's close just forwards. The next line proves no double-free. */
    CHECK(cb_transport_close(&txa) == CB_INFO_OK, "close idempotent");

    cb_net_shutdown();
    printf("  PASS\n");
    return 0;
}

/* ---------- section 2: NetSim adapter ---------- */

static void netsim_step(void *user)
{
    cb_netsim_t *net = (cb_netsim_t *)user;
    cb_netsim_step(net);
}

static int section2_netsim(void)
{
    printf("--- Section 2: NetSim adapter\n");
    g_clock_ns = 0;
    cb_netsim_t net = cb_netsim_create(NULL, 0xC0FFEEull, zero_params());
    CHECK(net.info == CB_INFO_OK, "create");
    cb_netsim_set_clock(&net, manual_clock, NULL);

    cb_net_addr_t aaddr = make_addr(0x7F000001u, 1100u);
    cb_net_addr_t baddr = make_addr(0x7F000001u, 1101u);
    cb_netsim_endpoint_t ea = cb_netsim_bind(&net, aaddr);
    cb_netsim_endpoint_t eb = cb_netsim_bind(&net, baddr);
    CHECK(ea.info == CB_INFO_OK, "bind A");
    CHECK(eb.info == CB_INFO_OK, "bind B");

    cb_transport_t txa = cb_transport_netsim(&ea);
    cb_transport_t txb = cb_transport_netsim(&eb);
    CHECK(txa.info == CB_INFO_OK, "factory A");
    CHECK(txb.info == CB_INFO_OK, "factory B");

    int rc = do_round_trip(&txa, &txb, baddr, netsim_step, &net);
    if (rc != 0) {
        cb_netsim_destroy(&net);
        return rc;
    }

    /* close on netsim adapter is a no-op; the endpoint lives on. Verify by
     * sending one more packet AFTER cb_transport_close and confirming it
     * still flows. */
    cb_transport_close(&txa);
    cb_transport_close(&txb);

    const char    again[] = "after-close";
    cb_net_io_result_t sr = cb_transport_send(&txa, baddr, again, sizeof(again));
    CHECK(sr.info == CB_INFO_OK, "post-close send still works (netsim)");
    cb_netsim_step(&net);
    uint8_t buf[64];
    cb_udp_recv_result_t rr = cb_transport_recv(&txb, buf, sizeof(buf));
    CHECK(rr.info == CB_INFO_OK, "post-close recv still works (netsim)");
    CHECK(rr.size == sizeof(again), "post-close recv size");

    cb_netsim_close(&ea);
    cb_netsim_close(&eb);
    cb_netsim_destroy(&net);
    printf("  PASS\n");
    return 0;
}

/* ---------- section 3: polymorphic dispatch ---------- */

/* Callee that doesn't know which backend it's holding — proves the seam. */
static int echo_through(cb_transport_t *send, cb_transport_t *recv,
                        cb_net_addr_t dest, step_fn_t step, void *step_user)
{
    return do_round_trip(send, recv, dest, step, step_user);
}

static int section3_polymorphic(void)
{
    printf("--- Section 3: same callee, two backends\n");

    /* netsim leg */
    {
        g_clock_ns = 0;
        cb_netsim_t net = cb_netsim_create(NULL, 0xABCDull, zero_params());
        cb_netsim_set_clock(&net, manual_clock, NULL);
        cb_net_addr_t a = make_addr(0x7F000001u, 2000u);
        cb_net_addr_t b = make_addr(0x7F000001u, 2001u);
        cb_netsim_endpoint_t ea = cb_netsim_bind(&net, a);
        cb_netsim_endpoint_t eb = cb_netsim_bind(&net, b);
        cb_transport_t txa = cb_transport_netsim(&ea);
        cb_transport_t txb = cb_transport_netsim(&eb);

        int rc = echo_through(&txa, &txb, b, netsim_step, &net);
        cb_netsim_close(&ea);
        cb_netsim_close(&eb);
        cb_netsim_destroy(&net);
        if (rc != 0) return rc;
    }

    /* udp leg */
    {
        CHECK(cb_net_init() == CB_INFO_OK, "net_init");
        cb_udp_socket_t sa = cb_udp_open(0u);
        cb_udp_socket_t sb = cb_udp_open(0u);
#ifdef CB_PLATFORM_POSIX
        struct sockaddr_in name;
        socklen_t          nlen = sizeof(name);
        memset(&name, 0, sizeof(name));
        CHECK(getsockname(sb.handle, (struct sockaddr *)&name, &nlen) == 0,
              "getsockname");
        uint16_t b_port = ntohs(name.sin_port);
#else
        uint16_t b_port = 0u;
        CHECK(0, "POSIX-only");
#endif
        cb_transport_t txa = cb_transport_udp(&sa);
        cb_transport_t txb = cb_transport_udp(&sb);
        cb_net_addr_t  to_b = make_addr(0x7F000001u, b_port);

        int rc = echo_through(&txa, &txb, to_b, NULL, NULL);
        cb_transport_close(&txa);
        cb_transport_close(&txb);
        cb_net_shutdown();
        if (rc != 0) return rc;
    }

    printf("  PASS\n");
    return 0;
}

/* ---------- section 4: bad-args ---------- */

static int section4_badargs(void)
{
    printf("--- Section 4: NULL / bad-args paths\n");

    cb_transport_t bad = cb_transport_udp(NULL);
    CHECK(bad.info != CB_INFO_OK, "udp factory rejects NULL");

    cb_transport_t bad2 = cb_transport_netsim(NULL);
    CHECK(bad2.info != CB_INFO_OK, "netsim factory rejects NULL");

    cb_net_addr_t  to = make_addr(0x7F000001u, 1u);
    uint8_t        buf[8];
    cb_net_io_result_t sr = cb_transport_send(NULL, to, buf, sizeof(buf));
    CHECK(sr.info == CB_INFO_GENERIC_ERROR, "send NULL transport -> error");
    cb_udp_recv_result_t rr = cb_transport_recv(NULL, buf, sizeof(buf));
    CHECK(rr.info == CB_INFO_GENERIC_ERROR, "recv NULL transport -> error");
    CHECK(cb_transport_close(NULL) == CB_INFO_GENERIC_ERROR, "close NULL -> error");

    printf("  PASS\n");
    return 0;
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== transport tests ===\n");
    if (section1_udp())          return 1;
    if (section2_netsim())       return 1;
    if (section3_polymorphic())  return 1;
    if (section4_badargs())      return 1;
    printf("All transport tests passed!\n");
    return 0;
}
