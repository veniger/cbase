/*
 * cb_transport — vtable indirection for connectionless datagram I/O.
 *
 * Two backends today:
 *   1. UDP   — pass-through to cb_udp_send / cb_udp_recv / cb_udp_close.
 *   2. NetSim — adapts cb_netsim_send_to / cb_netsim_recv_from to the same
 *               cb_net_io_result_t / cb_udp_recv_result_t shapes, so a
 *               consumer that only sees `cb_transport_t *` cannot tell which
 *               backend is wired in.
 *
 * No allocation. The factories only fill out a cb_transport_t value; the
 * caller owns the underlying socket / endpoint storage.
 */

#include <stddef.h>

/* --- UDP adapter --------------------------------------------------------- */

static cb_net_io_result_t cb__transport_udp_send(void *impl, cb_net_addr_t to,
                                                 const void *data, size_t size)
{
    cb_udp_socket_t *s = (cb_udp_socket_t *)impl;
    return cb_udp_send(s, to, data, size);
}

static cb_udp_recv_result_t cb__transport_udp_recv(void *impl,
                                                   void *buf, size_t buf_size)
{
    cb_udp_socket_t *s = (cb_udp_socket_t *)impl;
    return cb_udp_recv(s, buf, buf_size);
}

static cb_info_t cb__transport_udp_close(void *impl)
{
    cb_udp_socket_t *s = (cb_udp_socket_t *)impl;
    return cb_udp_close(s);
}

static const cb_transport_ops_t CB__TRANSPORT_UDP_OPS = {
    cb__transport_udp_send,
    cb__transport_udp_recv,
    cb__transport_udp_close,
};

/* --- NetSim adapter ------------------------------------------------------ */

/* netsim_send_to returns cb_info_t. To match cb_net_io_result_t, we report
 * bytes=size on enqueue success and bytes=0 on failure. NetSim drops are
 * silent (real-UDP-equivalent) so OK is the dominant success path. */
static cb_net_io_result_t cb__transport_netsim_send(void *impl, cb_net_addr_t to,
                                                    const void *data, size_t size)
{
    cb_netsim_endpoint_t *ep = (cb_netsim_endpoint_t *)impl;
    cb_net_io_result_t r;
    r.info  = cb_netsim_send_to(ep, to, data, size);
    r.bytes = (r.info == CB_INFO_OK) ? size : 0u;
    return r;
}

/* netsim_recv_from uses (out_src, out_buf, out_cap, out_len*). Adapt to
 * cb_udp_recv_result_t. Map CB_INFO_NETSIM_EMPTY -> CB_INFO_NET_WOULD_BLOCK
 * so polling callers don't have to branch on backend. */
static cb_udp_recv_result_t cb__transport_netsim_recv(void *impl,
                                                      void *buf, size_t buf_size)
{
    cb_netsim_endpoint_t *ep = (cb_netsim_endpoint_t *)impl;
    cb_udp_recv_result_t r;
    r.info      = CB_INFO_OK;
    r.size      = 0u;
    r.from.info = CB_INFO_OK;
    r.from.ip   = 0u;
    r.from.port = 0u;

    cb_net_addr_t src;
    size_t        got = 0u;
    cb_info_t     info = cb_netsim_recv_from(ep, &src, buf, buf_size, &got);
    if (info == CB_INFO_NETSIM_EMPTY) {
        r.info = CB_INFO_NET_WOULD_BLOCK;
        return r;
    }
    if (info != CB_INFO_OK) {
        r.info = info;       /* surface BUF_TOO_SMALL etc. as-is */
        return r;
    }
    r.size = got;
    r.from = src;
    return r;
}

/* The netsim endpoint's lifetime is owned by the cb_netsim_t instance, not
 * by the transport. Closing the transport is a no-op; the caller still calls
 * cb_netsim_close on the endpoint when they're done. */
static cb_info_t cb__transport_netsim_close(void *impl)
{
    (void)impl;
    return CB_INFO_OK;
}

static const cb_transport_ops_t CB__TRANSPORT_NETSIM_OPS = {
    cb__transport_netsim_send,
    cb__transport_netsim_recv,
    cb__transport_netsim_close,
};

/* --- Factories ----------------------------------------------------------- */

cb_transport_t cb_transport_udp(cb_udp_socket_t *udp)
{
    cb_transport_t t;
    t.info = CB_INFO_OK;
    t.ops  = &CB__TRANSPORT_UDP_OPS;
    t.impl = udp;
    if (udp == NULL || udp->info != CB_INFO_OK) {
        t.info = CB_INFO_GENERIC_ERROR;
    }
    return t;
}

cb_transport_t cb_transport_netsim(cb_netsim_endpoint_t *ep)
{
    cb_transport_t t;
    t.info = CB_INFO_OK;
    t.ops  = &CB__TRANSPORT_NETSIM_OPS;
    t.impl = ep;
    if (ep == NULL || ep->info != CB_INFO_OK) {
        t.info = CB_INFO_GENERIC_ERROR;
    }
    return t;
}

/* --- Dispatchers --------------------------------------------------------- */

cb_net_io_result_t cb_transport_send(cb_transport_t *t, cb_net_addr_t to,
                                     const void *data, size_t size)
{
    cb_net_io_result_t r;
    r.info  = CB_INFO_OK;
    r.bytes = 0u;
    if (t == NULL || t->ops == NULL || t->ops->send == NULL) {
        r.info = CB_INFO_GENERIC_ERROR;
        return r;
    }
    return t->ops->send(t->impl, to, data, size);
}

cb_udp_recv_result_t cb_transport_recv(cb_transport_t *t,
                                       void *buf, size_t buf_size)
{
    cb_udp_recv_result_t r;
    r.info      = CB_INFO_OK;
    r.size      = 0u;
    r.from.info = CB_INFO_OK;
    r.from.ip   = 0u;
    r.from.port = 0u;
    if (t == NULL || t->ops == NULL || t->ops->recv == NULL) {
        r.info = CB_INFO_GENERIC_ERROR;
        return r;
    }
    return t->ops->recv(t->impl, buf, buf_size);
}

cb_info_t cb_transport_close(cb_transport_t *t)
{
    if (t == NULL || t->ops == NULL || t->ops->close == NULL) {
        return CB_INFO_GENERIC_ERROR;
    }
    return t->ops->close(t->impl);
}
