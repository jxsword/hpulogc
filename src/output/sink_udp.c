/* -*- coding: utf-8 -*- */

/**
 * @file sink_udp.c
 * @brief UDP sink (POSIX only): one datagram per record, payload bounded
 *        by the configured mtu (rd_v0.6 §4.10.8).
 *
 * Delivery is best-effort by design: oversized lines are truncated to
 * mtu (the event is accounted failed, not written), EAGAIN discards
 * without blocking, and no delivery guarantee is made (UDP itself plus
 * the constant-discard per-sink async queue, D-S5). The socket is
 * non-blocking; emit never waits on the network.
 */

#include "output.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/** @brief Default single-datagram payload bound: 1500 Ethernet MTU minus
 *         IPv4 (20) and UDP (8) headers. */
#define HPU_UDP_MTU_DEFAULT 1472
#define HPU_UDP_MTU_MIN     576
#define HPU_UDP_MTU_MAX     65507

/** @brief Maximum accepted host configuration length (with NUL). */
#define HPU_UDP_HOST_MAX 256

/**
 * @brief UDP sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_udp_priv {
    char         host[HPU_UDP_HOST_MAX]; /*!< Configured host */
    int          port;                   /*!< Configured port (1-65535) */
    int          mtu;                    /*!< Datagram payload bound */
    int          fd;                     /*!< Socket, -1 when closed */
    size_t       addrlen;                /*!< Resolved endpoint length */
    unsigned char addr[HPU_NET_ADDR_MAX]; /*!< Resolved endpoint (start) */
} hpu_udp_priv_t;

/**
 * @brief Strict decimal parser for bounded non-negative ints.
 * @return 0 on success, -1 on malformed input or range violation.
 */
static int udp_parse_uint(const char* val, long lo, long hi, int* out)
{
    char* end = NULL;
    long v;

    if (val == NULL || val[0] == '\0') {
        return -1;
    }
    v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int udp_configure(hpulogc_sink_t* sink, const char* key,
                         const char* val)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "host") == 0) {
        if (val == NULL || val[0] == '\0' ||
            strlen(val) >= sizeof(p->host)) {
            return -1;
        }
        memcpy(p->host, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(key, "port") == 0) {
        return udp_parse_uint(val, 1, 65535, &p->port);
    }
    if (strcmp(key, "mtu") == 0) {
        return udp_parse_uint(val, HPU_UDP_MTU_MIN, HPU_UDP_MTU_MAX,
                              &p->mtu);
    }
    return -1; /* unknown key */
}

static int udp_init(hpulogc_sink_t* sink)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);

    if (p->mtu == 0) {
        p->mtu = HPU_UDP_MTU_DEFAULT;
    }
    if (p->host[0] == '\0' || p->port == 0) {
        return -1; /* required keys missing -> HPULOGC_ERR_CONFIG */
    }
    return 0;
}

static int udp_start(hpulogc_sink_t* sink)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%d", p->port);
    if (hpu_net_resolve(p->host, port_str, HPU_NET_DGRAM, p->addr,
                        &p->addrlen) != 0) {
        return -1;
    }
    p->fd = hpu_net_dgram_open(p->addr, p->addrlen);
    return (p->fd < 0) ? -1 : 0;
}

/**
 * @brief Send one record as a single datagram (truncated to mtu).
 * @return 0 when the record was fully delivered, -1 when it must be
 *         accounted failed (truncation, EAGAIN or send error).
 */
static int udp_send_one(hpu_udp_priv_t* p, const hpulogc_event_t* ev)
{
    size_t len = ev->line_len;

    if (len > (size_t)p->mtu) {
        /* Truncate, still deliver: the receiver gets a parseable (short)
         * line while the accounting reflects the degraded delivery. */
        len = (size_t)p->mtu;
    }
    if (hpu_net_send_dgram(p->fd, ev->line, len, p->addr, p->addrlen) != 0) {
        return -1; /* EAGAIN / EMSGSIZE / hard error: discard, no retry */
    }
    if (len != ev->line_len) {
        return -1; /* truncated delivery counts as failed (§4.10.8) */
    }
    return 0;
}

static void udp_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd < 0) {
        return;
    }
    if (udp_send_one(p, ev) != 0) {
        hpu_sink_account_failed(sink, 1);
    }
}

static int udp_emit_batch(hpulogc_sink_t* sink,
                          const hpulogc_event_t* const* evs, size_t n)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    size_t done = 0;

    if (p->fd < 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (udp_send_one(p, evs[i]) == 0) {
            done++;
        }
    }
    return (int)done;
}

static void udp_destroy(hpulogc_sink_t* sink)
{
    hpu_udp_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
}

/** @brief UDP sink type (SYNC|ASYNC|LINE_ATOMIC; POSIX builds). */
static const hpulogc_sink_ops_t g_udp_ops = {
    "udp",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_udp_priv_t),
    udp_configure,
    udp_init,
    udp_start,
    udp_emit,
    udp_emit_batch,
    NULL, /* flush: datagrams go out on sendto, nothing buffered */
    NULL, /* sync: no FSYNC capability */
    NULL, /* periodic: emit-path driven only */
    udp_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_udp_sink_ops(void)
{
    return &g_udp_ops;
}

#endif /* !defined(_WIN32) */
