/*
 * cbase_netsim.c - Deterministic in-process UDP network simulator.
 *
 * A pair-free-form virtual network: endpoints bind by cb_net_addr_t, sends
 * enqueue onto a sorted pending list, cb_netsim_step() moves matured items
 * into per-endpoint ready FIFOs, recv pops from those.
 *
 * Single-threaded by contract — no mutexes. All randomness is drawn from the
 * sim's own cb_rng_t (seeded at create), so with a fixed clock_fn the entire
 * trace is bit-exact reproducible.
 *
 * Pending list is a singly-linked list sorted by scheduled_ns (insertion
 * sort). Datagrams during a test are few; a heap is overkill. Ready lists
 * are per-endpoint FIFOs (head+tail). Payloads live in cb__alloc'ed buffers
 * owned by the pending/ready nodes and freed on deliver, drop, or flush.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* --- Internal node types --- */

struct cb__netsim_pending_t
{
    cb__netsim_pending_t *next;       /* sorted by scheduled_ns ascending */
    cb_net_addr_t         src;
    cb_net_addr_t         dest;
    uint64_t              scheduled_ns;
    uint8_t              *payload;    /* cb__alloc'd */
    size_t                len;
};

struct cb__netsim_endpoint_t
{
    cb__netsim_endpoint_t *next;
    cb_net_addr_t          addr;
    /* Ready FIFO, chained via cb__netsim_pending_t.next */
    cb__netsim_pending_t  *ready_head;
    cb__netsim_pending_t  *ready_tail;
    /* Queue-cap accounting (pending + ready). Widened to u64 so the cap
     * comparison in send_to cannot overflow even with many endpoints /
     * pathological cumulative flight. Upper-bounded per-send by the
     * CB_NETSIM_MAX_DATAGRAM_BYTES check on len. */
    uint64_t               bytes_in_flight;
};

/* --- Helpers --- */

static uint64_t cb__netsim_default_clock(void *user)
{
    (void)user;
    return cb_time_now_ns();
}

static bool cb__netsim_params_ok(const cb_netsim_params_t *p)
{
    if (p->latency_ms_min > p->latency_ms_max)               return false;
    if (p->reorder_swap_ms_min > p->reorder_swap_ms_max)     return false;
    return true;
}

static cb__netsim_endpoint_t *cb__netsim_find_endpoint(cb_netsim_t *net,
                                                       cb_net_addr_t addr)
{
    cb__netsim_endpoint_t *e = net->endpoints;
    while (e)
    {
        if (e->addr.ip == addr.ip && e->addr.port == addr.port)
            return e;
        e = e->next;
    }
    return NULL;
}

static void cb__netsim_free_node(cb_netsim_t *net, cb__netsim_pending_t *n)
{
    if (!n) return;
    cb__free(net->arena, n->payload);
    cb__free(net->arena, n);
}

static void cb__netsim_drop_endpoint_ready(cb_netsim_t *net,
                                           cb__netsim_endpoint_t *ep)
{
    cb__netsim_pending_t *p = ep->ready_head;
    while (p)
    {
        cb__netsim_pending_t *next = p->next;
        cb__netsim_free_node(net, p);
        p = next;
    }
    ep->ready_head = NULL;
    ep->ready_tail = NULL;
    ep->bytes_in_flight = 0;
}

/* Insert pending node in ascending scheduled_ns order. On ties, the new
 * node is inserted AFTER existing equal-keyed nodes (stable, FIFO). */
static void cb__netsim_pending_insert(cb_netsim_t *net,
                                      cb__netsim_pending_t *node)
{
    node->next = NULL;
    if (!net->pending)
    {
        net->pending = node;
        return;
    }

    if (node->scheduled_ns < net->pending->scheduled_ns)
    {
        node->next = net->pending;
        net->pending = node;
        return;
    }

    cb__netsim_pending_t *cur = net->pending;
    while (cur->next && cur->next->scheduled_ns <= node->scheduled_ns)
    {
        cur = cur->next;
    }
    node->next = cur->next;
    cur->next  = node;
}

/* Enqueue a freshly-built pending node. Caller has already rolled all
 * impairments and computed scheduled_ns. Accounts for bytes_in_flight on
 * the destination endpoint (if the endpoint exists). */
static void cb__netsim_enqueue_node(cb_netsim_t *net,
                                    cb__netsim_pending_t *node)
{
    cb__netsim_endpoint_t *dest_ep = cb__netsim_find_endpoint(net, node->dest);
    if (dest_ep)
    {
        dest_ep->bytes_in_flight += (uint64_t)node->len;
    }
    cb__netsim_pending_insert(net, node);
}

/* --- Public API --- */

cb_netsim_t cb_netsim_create(cb_arena_t *arena, uint64_t seed,
                             cb_netsim_params_t params)
{
    cb_netsim_t net;
    net.info             = CB_INFO_OK;
    net.arena            = arena;
    net.rng              = cb_rng_seed(seed, 0ull);
    net.params           = params;
    net.clock_fn         = cb__netsim_default_clock;
    net.clock_user       = NULL;
    net.endpoints        = NULL;
    net.pending          = NULL;
    net.stat_enqueued    = 0;
    net.stat_delivered   = 0;
    net.stat_dropped     = 0;
    net.stat_duplicated  = 0;
    net.stat_corrupted   = 0;
    net.stat_reordered   = 0;
    net.stat_queue_full  = 0;

    if (!cb__netsim_params_ok(&params))
    {
        net.info = CB_INFO_NETSIM_BAD_PARAMS;
    }
    return net;
}

void cb_netsim_destroy(cb_netsim_t *net)
{
    if (!net) return;

    /* Free all pending */
    cb__netsim_pending_t *p = net->pending;
    while (p)
    {
        cb__netsim_pending_t *next = p->next;
        cb__netsim_free_node(net, p);
        p = next;
    }
    net->pending = NULL;

    /* Free all endpoints + their ready FIFOs */
    cb__netsim_endpoint_t *e = net->endpoints;
    while (e)
    {
        cb__netsim_endpoint_t *next = e->next;
        cb__netsim_drop_endpoint_ready(net, e);
        cb__free(net->arena, e);
        e = next;
    }
    net->endpoints = NULL;
}

cb_info_t cb_netsim_set_params(cb_netsim_t *net, cb_netsim_params_t params)
{
    if (!cb__netsim_params_ok(&params))
    {
        return CB_INFO_NETSIM_BAD_PARAMS;
    }
    net->params = params;
    return CB_INFO_OK;
}

void cb_netsim_set_clock(cb_netsim_t *net, cb_netsim_clock_fn fn, void *user)
{
    if (fn)
    {
        net->clock_fn   = fn;
        net->clock_user = user;
    }
    else
    {
        net->clock_fn   = cb__netsim_default_clock;
        net->clock_user = NULL;
    }
}

cb_netsim_endpoint_t cb_netsim_bind(cb_netsim_t *net, cb_net_addr_t addr)
{
    cb_netsim_endpoint_t h;
    h.info = CB_INFO_OK;
    h.net  = net;
    h.addr = addr;

    if (cb__netsim_find_endpoint(net, addr))
    {
        h.info = CB_INFO_NETSIM_DUPLICATE_ADDR;
        h.net  = NULL;
        return h;
    }

    cb__netsim_endpoint_t *e = (cb__netsim_endpoint_t *)
        cb__alloc(net->arena, sizeof(cb__netsim_endpoint_t),
                  sizeof(void *));
    if (!e)
    {
        h.info = CB_INFO_NETSIM_ALLOC_FAILED;
        h.net  = NULL;
        return h;
    }
    e->next              = net->endpoints;
    e->addr              = addr;
    e->ready_head        = NULL;
    e->ready_tail        = NULL;
    e->bytes_in_flight   = 0;
    net->endpoints       = e;

    return h;
}

void cb_netsim_close(cb_netsim_endpoint_t *ep)
{
    if (!ep || !ep->net) return;
    cb_netsim_t *net = ep->net;

    /* Unlink the endpoint from net->endpoints. Hard-delete (not a tombstone)
     * so a later cb_netsim_bind on the same addr starts a fresh endpoint and
     * cannot inherit orphaned pending datagrams from the closed one. */
    cb__netsim_endpoint_t **link = &net->endpoints;
    cb__netsim_endpoint_t  *e    = NULL;
    while (*link)
    {
        cb__netsim_endpoint_t *cur = *link;
        if (cur->addr.ip == ep->addr.ip && cur->addr.port == ep->addr.port)
        {
            e     = cur;
            *link = cur->next;
            break;
        }
        link = &cur->next;
    }
    if (!e)
    {
        ep->net = NULL;
        return;
    }

    /* Drop the endpoint's ready FIFO. */
    cb__netsim_drop_endpoint_ready(net, e);

    /* Scrub any pending entries addressed to this endpoint — they would
     * otherwise mis-deliver to a rebind at the same addr (or fall through to
     * a silent stat_dropped at step time, which is the same outcome but
     * wastes memory until matured). Count each as a drop. */
    cb__netsim_pending_t **plink = &net->pending;
    while (*plink)
    {
        cb__netsim_pending_t *p = *plink;
        if (p->dest.ip == ep->addr.ip && p->dest.port == ep->addr.port)
        {
            *plink = p->next;
            cb__netsim_free_node(net, p);
            net->stat_dropped++;
        }
        else
        {
            plink = &p->next;
        }
    }

    cb__free(net->arena, e);
    ep->net = NULL;
}

/* Build + enqueue one copy of a datagram. Handles per-copy corruption and
 * reorder rolls. Base `base_scheduled_ns` already includes the per-packet
 * latency roll. Returns true on success, false on alloc failure. */
static bool cb__netsim_enqueue_copy(cb_netsim_t *net,
                                    cb_net_addr_t src, cb_net_addr_t dest,
                                    const void *buf, size_t len,
                                    uint64_t base_scheduled_ns)
{
    cb__netsim_pending_t *node = (cb__netsim_pending_t *)
        cb__alloc(net->arena, sizeof(cb__netsim_pending_t), sizeof(void *));
    if (!node) return false;
    node->next         = NULL;
    node->src          = src;
    node->dest         = dest;
    node->scheduled_ns = base_scheduled_ns;
    node->len          = len;
    node->payload      = NULL;

    if (len > 0)
    {
        node->payload = (uint8_t *)cb__alloc(net->arena, len, 1);
        if (!node->payload)
        {
            cb__free(net->arena, node);
            return false;
        }
        memcpy(node->payload, buf, len);
    }

    /* Corruption roll (per-copy). Flip one random bit in the payload. */
    if (len > 0 && cb_rng_chance_fx16(&net->rng, net->params.corrupt_prob))
    {
        /* Draw a bit index in [0, len*8). len <= uint32 by contract
         * (we're test-scale), so u32_below is safe. */
        uint32_t bits = (uint32_t)len * 8u;
        uint32_t bit  = cb_rng_u32_below(&net->rng, bits);
        node->payload[bit >> 3] ^= (uint8_t)(1u << (bit & 7u));
        net->stat_corrupted++;
    }

    /* Reorder roll (per-copy). Adds extra latency to push this past the
     * next-newer packet in the common case. */
    if (cb_rng_chance_fx16(&net->rng, net->params.reorder_prob))
    {
        int32_t rlo = (int32_t)net->params.reorder_swap_ms_min;
        int32_t rhi = (int32_t)net->params.reorder_swap_ms_max;
        int32_t rms = cb_rng_i32_range(&net->rng, rlo, rhi);
        node->scheduled_ns += (uint64_t)rms * 1000000ull;
        net->stat_reordered++;
    }

    cb__netsim_enqueue_node(net, node);
    return true;
}

cb_info_t cb_netsim_send_to(cb_netsim_endpoint_t *ep,
                            cb_net_addr_t dest,
                            const void *buf, size_t len)
{
    if (!ep || !ep->net) return CB_INFO_GENERIC_ERROR;
    cb_netsim_t *net = ep->net;

    /* Payload size cap: reject BEFORE touching any counter or state so a
     * too-large send is a pure, observable failure with no side effects.
     * Also guarantees the (size_t -> uint64_t) accounting below cannot
     * silently truncate on 32-bit size_t platforms. */
    if (len > CB_NETSIM_MAX_DATAGRAM_BYTES)
    {
        return CB_INFO_NETSIM_PAYLOAD_TOO_LARGE;
    }

    net->stat_enqueued++;

    /* Destination-not-bound -> silent drop (counts as drop). */
    cb__netsim_endpoint_t *dest_ep = cb__netsim_find_endpoint(net, dest);
    if (!dest_ep)
    {
        net->stat_dropped++;
        return CB_INFO_OK;
    }

    /* Queue cap check BEFORE random rolls so rolls happen only for enqueued
     * packets — keeps determinism cleaner w.r.t. capacity behavior. All
     * arithmetic in u64; len is bounded above by CB_NETSIM_MAX_DATAGRAM_BYTES
     * so `in_flight + len` cannot wrap. */
    if (net->params.max_queue_bytes != 0 &&
        dest_ep->bytes_in_flight + (uint64_t)len >
            (uint64_t)net->params.max_queue_bytes)
    {
        net->stat_queue_full++;
        return CB_INFO_OK;
    }

    /* Drop roll. */
    if (cb_rng_chance_fx16(&net->rng, net->params.drop_prob))
    {
        net->stat_dropped++;
        return CB_INFO_OK;
    }

    /* Latency roll for the primary copy. */
    uint64_t now = net->clock_fn(net->clock_user);
    int32_t  lms = cb_rng_i32_range(&net->rng,
                                    (int32_t)net->params.latency_ms_min,
                                    (int32_t)net->params.latency_ms_max);
    uint64_t primary_sched = now + (uint64_t)lms * 1000000ull;

    if (!cb__netsim_enqueue_copy(net, ep->addr, dest, buf, len, primary_sched))
    {
        return CB_INFO_NETSIM_ALLOC_FAILED;
    }

    /* Duplicate roll. If it fires, enqueue a second copy with its own
     * independently-rolled latency. */
    if (cb_rng_chance_fx16(&net->rng, net->params.dup_prob))
    {
        int32_t lms2 = cb_rng_i32_range(&net->rng,
                                        (int32_t)net->params.latency_ms_min,
                                        (int32_t)net->params.latency_ms_max);
        uint64_t dup_sched = now + (uint64_t)lms2 * 1000000ull;
        if (!cb__netsim_enqueue_copy(net, ep->addr, dest, buf, len, dup_sched))
        {
            return CB_INFO_NETSIM_ALLOC_FAILED;
        }
        net->stat_duplicated++;
    }

    return CB_INFO_OK;
}

uint32_t cb_netsim_step(cb_netsim_t *net)
{
    uint64_t now = net->clock_fn(net->clock_user);
    uint32_t moved = 0;

    while (net->pending && net->pending->scheduled_ns <= now)
    {
        cb__netsim_pending_t *node = net->pending;
        net->pending = node->next;
        node->next = NULL;

        cb__netsim_endpoint_t *dest_ep =
            cb__netsim_find_endpoint(net, node->dest);
        if (!dest_ep)
        {
            /* Endpoint closed or never existed — drop silently. */
            net->stat_dropped++;
            cb__netsim_free_node(net, node);
            continue;
        }

        /* Append to ready FIFO. */
        if (dest_ep->ready_tail)
        {
            dest_ep->ready_tail->next = node;
            dest_ep->ready_tail = node;
        }
        else
        {
            dest_ep->ready_head = node;
            dest_ep->ready_tail = node;
        }
        moved++;
    }
    return moved;
}

cb_info_t cb_netsim_recv_from(cb_netsim_endpoint_t *ep,
                              cb_net_addr_t *out_src,
                              void *out_buf, size_t out_cap,
                              size_t *out_len)
{
    if (!ep || !ep->net || !out_len) return CB_INFO_GENERIC_ERROR;
    cb_netsim_t *net = ep->net;

    cb__netsim_endpoint_t *e = cb__netsim_find_endpoint(net, ep->addr);
    if (!e || !e->ready_head)
    {
        *out_len = 0;
        return CB_INFO_NETSIM_EMPTY;
    }

    cb__netsim_pending_t *node = e->ready_head;
    if (node->len > out_cap)
    {
        *out_len = node->len;
        return CB_INFO_NETSIM_BUF_TOO_SMALL;
    }

    /* Pop head. */
    e->ready_head = node->next;
    if (!e->ready_head) e->ready_tail = NULL;

    if (out_src) *out_src = node->src;
    if (node->len > 0 && out_buf)
    {
        memcpy(out_buf, node->payload, node->len);
    }
    *out_len = node->len;

    if (e->bytes_in_flight >= (uint64_t)node->len)
        e->bytes_in_flight -= (uint64_t)node->len;
    else
        e->bytes_in_flight = 0;

    cb__netsim_free_node(net, node);
    net->stat_delivered++;
    return CB_INFO_OK;
}

void cb_netsim_flush(cb_netsim_t *net)
{
    /* Drop everything pending. */
    cb__netsim_pending_t *p = net->pending;
    while (p)
    {
        cb__netsim_pending_t *next = p->next;
        cb__netsim_free_node(net, p);
        p = next;
    }
    net->pending = NULL;

    /* Drop each endpoint's ready FIFO. */
    cb__netsim_endpoint_t *e = net->endpoints;
    while (e)
    {
        cb__netsim_drop_endpoint_ready(net, e);
        e = e->next;
    }
}
