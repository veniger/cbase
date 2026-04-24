#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../cbase.h"

#define UDP_PORT_A 17771
#define UDP_PORT_B 17772
#define TCP_PORT   17773

static int fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

/* Server side of the TCP test runs on a worker thread. */
typedef struct
{
    int ready;
    int ok;
    cb_mutex_t mtx;
    cb_cond_t  cv;
} tcp_server_sync_t;

static cb_thread_result_t tcp_server_fn(void *arg)
{
    tcp_server_sync_t *sync = (tcp_server_sync_t *)arg;
    cb_thread_result_t tr = { CB_INFO_OK, NULL };

    cb_tcp_listener_t lst = cb_tcp_listen(TCP_PORT, 4);
    if (lst.info != CB_INFO_OK)
    {
        tr.info = lst.info;
        return tr;
    }

    cb_mutex_lock(&sync->mtx);
    sync->ready = 1;
    cb_cond_signal(&sync->cv);
    cb_mutex_unlock(&sync->mtx);

    /* Poll the listener for an incoming connection. */
    cb_net_pollable_t p = { lst.handle, CB_NET_POLL_READ, 0 };
    if (cb_net_poll(&p, 1, 2000) != CB_INFO_OK) { sync->ok = 0; cb_tcp_listener_close(&lst); return tr; }
    if (!(p.revents & CB_NET_POLL_READ))        { sync->ok = 0; cb_tcp_listener_close(&lst); return tr; }

    cb_tcp_socket_t conn = cb_tcp_accept(&lst);
    if (conn.info != CB_INFO_OK) { sync->ok = 0; cb_tcp_listener_close(&lst); return tr; }

    /* Wait for bytes, echo them back. */
    cb_net_pollable_t rp = { conn.handle, CB_NET_POLL_READ, 0 };
    if (cb_net_poll(&rp, 1, 2000) != CB_INFO_OK) { sync->ok = 0; goto done; }

    char buf[256];
    cb_net_io_result_t rr = cb_tcp_recv(&conn, buf, sizeof(buf));
    if (rr.info != CB_INFO_OK || rr.bytes == 0) { sync->ok = 0; goto done; }

    cb_net_io_result_t sr = cb_tcp_send(&conn, buf, rr.bytes);
    if (sr.info != CB_INFO_OK || sr.bytes != rr.bytes) { sync->ok = 0; goto done; }

    sync->ok = 1;
done:
    cb_tcp_close(&conn);
    cb_tcp_listener_close(&lst);
    return tr;
}

int main(void)
{
    if (cb_net_init() != CB_INFO_OK) return fail("cb_net_init");

    /* --- Address parsing --- */
    cb_net_addr_t loop = cb_net_addr_v4("127.0.0.1", UDP_PORT_B);
    if (loop.info != CB_INFO_OK) return fail("cb_net_addr_v4");
    if (loop.ip != 0x7F000001u) return fail("loopback ip parse");
    if (loop.port != UDP_PORT_B) return fail("loopback port");
    printf("Address parse: OK (127.0.0.1:%u => 0x%08X:%u)\n",
           (unsigned)loop.port, (unsigned)loop.ip, (unsigned)loop.port);

    cb_net_addr_t bad = cb_net_addr_v4("not.an.ip.address", 0);
    if (bad.info != CB_INFO_NET_ADDR_INVALID) return fail("addr invalid detection");
    printf("Invalid address rejected: OK\n");

    /* --- UDP loopback --- */
    cb_udp_socket_t a = cb_udp_open(UDP_PORT_A);
    if (a.info != CB_INFO_OK) return fail("cb_udp_open A");
    cb_udp_socket_t b = cb_udp_open(UDP_PORT_B);
    if (b.info != CB_INFO_OK) return fail("cb_udp_open B");

    /* Non-blocking recv on empty socket should report WOULD_BLOCK. */
    char scratch[16];
    cb_udp_recv_result_t empty = cb_udp_recv(&b, scratch, sizeof(scratch));
    if (empty.info != CB_INFO_NET_WOULD_BLOCK) return fail("UDP empty recv should WOULD_BLOCK");
    printf("UDP non-blocking recv on empty socket: WOULD_BLOCK (correct)\n");

    const char *msg = "hello over UDP";
    size_t msg_len = strlen(msg) + 1;
    cb_net_io_result_t us = cb_udp_send(&a, loop, msg, msg_len);
    if (us.info != CB_INFO_OK || us.bytes != msg_len) return fail("cb_udp_send");

    cb_net_pollable_t up = { b.handle, CB_NET_POLL_READ, 0 };
    if (cb_net_poll(&up, 1, 1000) != CB_INFO_OK) return fail("UDP poll");
    if (!(up.revents & CB_NET_POLL_READ))        return fail("UDP poll no read signal");

    char ubuf[256];
    cb_udp_recv_result_t ur = cb_udp_recv(&b, ubuf, sizeof(ubuf));
    if (ur.info != CB_INFO_OK)           return fail("cb_udp_recv");
    if (ur.size != msg_len)              return fail("UDP size mismatch");
    if (memcmp(ubuf, msg, msg_len) != 0) return fail("UDP payload mismatch");
    if (ur.from.ip != 0x7F000001u)       return fail("UDP from.ip");
    if (ur.from.port != UDP_PORT_A)      return fail("UDP from.port");
    printf("UDP loopback: OK (%zu bytes from 127.0.0.1:%u)\n", ur.size, (unsigned)ur.from.port);

    cb_udp_close(&a);
    cb_udp_close(&b);

    /* --- TCP loopback (threaded) --- */
    tcp_server_sync_t sync;
    sync.ready = 0;
    sync.ok = 0;
    sync.mtx = cb_mutex_create();
    sync.cv  = cb_cond_create();

    cb_arena_t arena = cb_arena_create(1024, CB_ARENA_LINEAR);
    cb_thread_t srv = cb_thread_create(&arena, tcp_server_fn, &sync);
    if (srv.info != CB_INFO_OK) return fail("cb_thread_create");

    /* Wait until the listener is up so connect() doesn't race. */
    cb_mutex_lock(&sync.mtx);
    while (!sync.ready) cb_cond_wait(&sync.cv, &sync.mtx);
    cb_mutex_unlock(&sync.mtx);

    cb_net_addr_t srv_addr = cb_net_addr_v4("127.0.0.1", TCP_PORT);
    cb_tcp_socket_t cli = cb_tcp_connect(srv_addr);
    if (cli.info != CB_INFO_OK) return fail("cb_tcp_connect");

    const char *tcp_msg = "hello over TCP";
    size_t tcp_len = strlen(tcp_msg) + 1;
    cb_net_io_result_t ts = cb_tcp_send(&cli, tcp_msg, tcp_len);
    if (ts.info != CB_INFO_OK || ts.bytes != tcp_len) return fail("cb_tcp_send");

    /* Wait for echo. */
    cb_net_pollable_t tp = { cli.handle, CB_NET_POLL_READ, 0 };
    if (cb_net_poll(&tp, 1, 2000) != CB_INFO_OK) return fail("TCP poll");
    if (!(tp.revents & CB_NET_POLL_READ))        return fail("TCP poll no read");

    char tbuf[256];
    cb_net_io_result_t tr = cb_tcp_recv(&cli, tbuf, sizeof(tbuf));
    if (tr.info != CB_INFO_OK)            return fail("cb_tcp_recv");
    if (tr.bytes != tcp_len)              return fail("TCP bytes mismatch");
    if (memcmp(tbuf, tcp_msg, tcp_len) != 0) return fail("TCP payload mismatch");
    printf("TCP loopback echo: OK (%zu bytes round-tripped)\n", tr.bytes);

    cb_tcp_close(&cli);
    cb_thread_join(&srv);
    if (!sync.ok) return fail("server side reported failure");

    cb_mutex_destroy(&sync.mtx);
    cb_cond_destroy(&sync.cv);
    cb_arena_destroy(&arena);

    cb_net_shutdown();

    printf("All network tests passed!\n");
    return 0;
}
