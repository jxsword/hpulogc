/* -*- coding: utf-8 -*- */

/**
 * @file sink_tcp.c
 * @brief TCP sink (POSIX only): line-framed stream output with
 *        exponential-backoff reconnect (rd_v0.6 §4.10.8).
 *
 * Each record is written as its rendered line plus one '\n' (syslog/
 * rsyslog line-protocol compatible). start() connects fail-fast; a
 * mid-run disconnect (EPIPE/ECONNRESET/...) switches the instance to a
 * disconnected state and retries with exponential backoff driven by the
 * emit path (async workers never call periodic). Events hitting the
 * backoff window are accounted failed; nothing is buffered for retransmit
 * (at-most-once). Batched lines are coalesced into a pre-allocated priv
 * buffer so steady-state emitting does zero mallocs.
 */

#include "output.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/** @brief Coalesced write buffer size (pre-allocated in priv). */
#define HPU_TCP_COALESCE_SIZE (64U * 1024U)

/** @brief Default/limit values for the reconnect backoff (seconds). */
#define HPU_TCP_BACKOFF_DEFAULT_S   1
#define HPU_TCP_BACKOFF_MAX_DEFAULT_S 30
#define HPU_TCP_BACKOFF_MAX_LIMIT_S   3600

/** @brief Maximum accepted host configuration length (with NUL). */
#define HPU_TCP_HOST_MAX 256

/**
 * @brief TCP sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_tcp_priv {
    char          host[HPU_TCP_HOST_MAX]; /*!< Configured host */
    int           port;                   /*!< Configured port (1-65535) */
    int           backoff_init_s;         /*!< reconnect backoff (seconds) */
    int           backoff_max_s;          /*!< reconnect backoff max (seconds) */
    int           fd;                     /*!< Socket, -1 when disconnected */
    int           connected;              /*!< fd is a live connection */
    uint64_t      backoff_ns;             /*!< Current backoff window */
    uint64_t      retry_deadline_ns;      /*!< Monotonic reconnect gate */
    size_t        addrlen;                /*!< Resolved endpoint length */
    unsigned char addr[HPU_NET_ADDR_MAX]; /*!< Resolved endpoint (start) */
    size_t        buf_len;                /*!< Bytes pending in buf */
    size_t        buf_lines;              /*!< Records pending in buf */
    unsigned char buf[HPU_TCP_COALESCE_SIZE]; /*!< Coalesced line frames */
} hpu_tcp_priv_t;

/**
 * @brief Strict decimal parser for bounded non-negative ints.
 * @return 0 on success, -1 on malformed input or range violation.
 */
static int tcp_parse_uint(const char* val, long lo, long hi, int* out)
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

static int tcp_configure(hpulogc_sink_t* sink, const char* key,
                         const char* val)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "host") == 0) {
        if (val == NULL || val[0] == '\0' ||
            strlen(val) >= sizeof(p->host)) {
            return -1;
        }
        memcpy(p->host, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(key, "port") == 0) {
        return tcp_parse_uint(val, 1, 65535, &p->port);
    }
    if (strcmp(key, "reconnect backoff") == 0) {
        return tcp_parse_uint(val, 1, 60, &p->backoff_init_s);
    }
    if (strcmp(key, "reconnect backoff max") == 0) {
        return tcp_parse_uint(val, 1, HPU_TCP_BACKOFF_MAX_LIMIT_S,
                              &p->backoff_max_s);
    }
    return -1; /* unknown key */
}

static int tcp_init(hpulogc_sink_t* sink)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);

    if (p->backoff_init_s == 0) {
        p->backoff_init_s = HPU_TCP_BACKOFF_DEFAULT_S;
    }
    if (p->backoff_max_s == 0) {
        p->backoff_max_s = HPU_TCP_BACKOFF_MAX_DEFAULT_S;
    }
    if (p->host[0] == '\0' || p->port == 0 ||
        p->backoff_max_s < p->backoff_init_s) {
        return -1; /* required/invalid config -> HPULOGC_ERR_CONFIG */
    }
    return 0;
}

static int tcp_start(hpulogc_sink_t* sink)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%d", p->port);
    if (hpu_net_resolve(p->host, port_str, HPU_NET_STREAM, p->addr,
                        &p->addrlen) != 0) {
        return -1;
    }
    /* Fail-fast first connect (rollingfile open semantics): the returned
     * error maps to HPULOGC_ERR_IO and fails init. */
    p->fd = hpu_net_stream_open(p->addr, p->addrlen);
    if (p->fd < 0) {
        return -1;
    }
    p->connected = 1;
    return 0;
}

/**
 * @brief Drop the connection and arm the backoff window.
 *
 * The retry gate is set to now + current backoff, then the backoff
 * doubles (capped at reconnect backoff max); a successful reconnect
 * resets it to the initial value.
 */
static void tcp_mark_disconnected(hpu_tcp_priv_t* p)
{
    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
    p->buf_len = 0;   /* pending coalesced lines are lost (at-most-once) */
    p->buf_lines = 0;
    if (p->backoff_ns == 0) {
        p->backoff_ns = (uint64_t)p->backoff_init_s * 1000000000ULL;
    }
    p->retry_deadline_ns = hpu_now_ns() + p->backoff_ns;
    p->backoff_ns *= 2;
    if (p->backoff_ns > (uint64_t)p->backoff_max_s * 1000000000ULL) {
        p->backoff_ns = (uint64_t)p->backoff_max_s * 1000000000ULL;
    }
}

/**
 * @brief Attempt a reconnect when the backoff window has elapsed.
 * @return 0 connected, -1 still disconnected (backoff or connect error).
 */
static int tcp_reconnect(hpu_tcp_priv_t* p)
{
    if (hpu_now_ns() < p->retry_deadline_ns) {
        return -1;
    }
    /* Address comes from start-time resolution: re-resolving here would
     * mean blocking + allocating inside the emit callback (§4.10.4). */
    p->fd = hpu_net_stream_open(p->addr, p->addrlen);
    if (p->fd < 0) {
        p->retry_deadline_ns = hpu_now_ns() + p->backoff_ns;
        p->backoff_ns *= 2;
        if (p->backoff_ns > (uint64_t)p->backoff_max_s * 1000000000ULL) {
            p->backoff_ns = (uint64_t)p->backoff_max_s * 1000000000ULL;
        }
        return -1;
    }
    p->connected = 1;
    p->backoff_ns = (uint64_t)p->backoff_init_s * 1000000000ULL;
    p->retry_deadline_ns = 0;
    return 0;
}

/**
 * @brief Write out pending coalesced line frames.
 *
 * On failure the connection is marked down and the still-buffered lines
 * are lost; @p done is decremented so the caller's return value reflects
 * only records that actually left this process.
 *
 * @return 0 when the caller may continue, -1 when disconnected.
 */
static int tcp_flush_pending(hpu_tcp_priv_t* p, size_t* done)
{
    size_t lost;

    if (p->buf_len == 0) {
        return 0;
    }
    lost = p->buf_lines;
    if (hpu_net_send(p->fd, p->buf, p->buf_len) != 0) {
        tcp_mark_disconnected(p);
        *done -= lost;
        return -1;
    }
    p->buf_len = 0;
    p->buf_lines = 0;
    return 0;
}

static void tcp_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);

    if (!p->connected && tcp_reconnect(p) != 0) {
        hpu_sink_account_failed(sink, 1); /* inside the backoff window */
        return;
    }
    /* Borrowed event: both sends complete inside this callback. Two
     * sends from the serialized callback keep the line frame contiguous
     * on the stream. */
    if (hpu_net_send(p->fd, ev->line, ev->line_len) != 0 ||
        hpu_net_send(p->fd, "\n", 1) != 0) {
        tcp_mark_disconnected(p);
        hpu_sink_account_failed(sink, 1);
    }
}

static int tcp_emit_batch(hpulogc_sink_t* sink,
                          const hpulogc_event_t* const* evs, size_t n)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    size_t done = 0;

    if (!p->connected && tcp_reconnect(p) != 0) {
        return 0; /* whole batch lands in the backoff window */
    }
    for (i = 0; i < n; i++) {
        const hpulogc_event_t* ev = evs[i];
        size_t need = ev->line_len + 1; /* line frame + '\n' */

        if (need > sizeof(p->buf)) {
            /* Oversized single line: bypass the coalesce buffer. */
            if (tcp_flush_pending(p, &done) != 0) {
                return (done > 0) ? (int)done : -1;
            }
            if (hpu_net_send(p->fd, ev->line, ev->line_len) != 0 ||
                hpu_net_send(p->fd, "\n", 1) != 0) {
                tcp_mark_disconnected(p);
                return (done > 0) ? (int)done : -1;
            }
            done++;
            continue;
        }
        if (p->buf_len + need > sizeof(p->buf)) {
            if (tcp_flush_pending(p, &done) != 0) {
                return (done > 0) ? (int)done : -1;
            }
        }
        if (ev->line_len > 0) {
            memcpy(p->buf + p->buf_len, ev->line, ev->line_len);
        }
        p->buf[p->buf_len + ev->line_len] = '\n';
        p->buf_len += need;
        p->buf_lines++;
        done++;
    }
    if (tcp_flush_pending(p, &done) != 0) {
        return (done > 0) ? (int)done : -1;
    }
    return (int)done;
}

static void tcp_destroy(hpulogc_sink_t* sink)
{
    hpu_tcp_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
}

/** @brief TCP sink type (SYNC|ASYNC|LINE_ATOMIC; POSIX builds). */
static const hpulogc_sink_ops_t g_tcp_ops = {
    "tcp",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_tcp_priv_t),
    tcp_configure,
    tcp_init,
    tcp_start,
    tcp_emit,
    tcp_emit_batch,
    NULL, /* flush: the coalesce buffer is drained per emit_batch call */
    NULL, /* sync: no FSYNC capability */
    NULL, /* periodic: reconnect is emit-path driven */
    tcp_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_tcp_sink_ops(void)
{
    return &g_tcp_ops;
}

#endif /* !defined(_WIN32) */
