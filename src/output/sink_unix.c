/* -*- coding: utf-8 -*- */

/**
 * @file sink_unix.c
 * @brief Unix domain socket sink (POSIX only): connect-type client for
 *        an externally bound endpoint, dgram or stream
 *        (rd_v0.6 §4.10.9).
 *
 * The library only connects to an existing socket file; it never
 * creates, binds or unlinks endpoints. start() connects fail-fast; a
 * mid-run peer restart switches the instance to a disconnected state
 * with the same event-driven exponential backoff as the TCP sink. In
 * dgram mode every record is one datagram (record boundary == datagram
 * boundary, no framing); in stream mode each record is the rendered
 * line plus '\n', coalesced into a pre-allocated priv buffer so
 * steady-state emitting does zero mallocs. EAGAIN (peer queue full) is
 * a transient discard accounted failed without leaving the connected
 * state.
 */

#include "output.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/** @brief Coalesced write buffer size (pre-allocated in priv, stream
 *         mode only). */
#define HPU_UNIX_COALESCE_SIZE (64U * 1024U)

/** @brief Default/limit values for the reconnect backoff (seconds),
 *         identical to the TCP sink (rd_v0.6 §4.10.8). */
#define HPU_UNIX_BACKOFF_DEFAULT_S   1
#define HPU_UNIX_BACKOFF_MAX_DEFAULT_S 30
#define HPU_UNIX_BACKOFF_MAX_LIMIT_S   3600

/** @brief Maximum accepted endpoint path length (with NUL). The
 *         platform sun_path limit is enforced by the contract at
 *         start; this bound is the configuration-level cap. */
#define HPU_UNIX_PATH_MAX 128

/**
 * @brief Unix sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_unix_priv {
    char          path[HPU_UNIX_PATH_MAX]; /*!< Configured endpoint path */
    int           socktype;                /*!< 0 unset, 1 dgram, 2 stream */
    int           backoff_init_s;          /*!< reconnect backoff (seconds) */
    int           backoff_max_s;           /*!< reconnect backoff max (seconds) */
    int           fd;                      /*!< Socket, -1 when disconnected */
    int           connected;               /*!< fd is a live connection */
    uint64_t      backoff_ns;              /*!< Current backoff window */
    uint64_t      retry_deadline_ns;       /*!< Monotonic reconnect gate */
    size_t        buf_len;                 /*!< Bytes pending in buf (stream) */
    size_t        buf_lines;               /*!< Records pending in buf (stream) */
    unsigned char buf[HPU_UNIX_COALESCE_SIZE]; /*!< Coalesced line frames */
} hpu_unix_priv_t;

/**
 * @brief Strict decimal parser for bounded non-negative ints.
 * @return 0 on success, -1 on malformed input or range violation.
 */
static int unix_parse_uint(const char* val, long lo, long hi, int* out)
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

static int unix_configure(hpulogc_sink_t* sink, const char* key,
                          const char* val)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "path") == 0) {
        if (val == NULL || val[0] == '\0' ||
            strlen(val) >= sizeof(p->path)) {
            return -1;
        }
        memcpy(p->path, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(key, "socktype") == 0) {
        if (val == NULL) {
            return -1;
        }
        if (strcmp(val, "dgram") == 0) {
            p->socktype = 1;
            return 0;
        }
        if (strcmp(val, "stream") == 0) {
            p->socktype = 2;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "reconnect backoff") == 0) {
        return unix_parse_uint(val, 1, 60, &p->backoff_init_s);
    }
    if (strcmp(key, "reconnect backoff max") == 0) {
        return unix_parse_uint(val, 1, HPU_UNIX_BACKOFF_MAX_LIMIT_S,
                               &p->backoff_max_s);
    }
    return -1; /* unknown key */
}

static int unix_init(hpulogc_sink_t* sink)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);

    if (p->socktype == 0) {
        p->socktype = 1; /* default dgram (§4.10.9) */
    }
    if (p->backoff_init_s == 0) {
        p->backoff_init_s = HPU_UNIX_BACKOFF_DEFAULT_S;
    }
    if (p->backoff_max_s == 0) {
        p->backoff_max_s = HPU_UNIX_BACKOFF_MAX_DEFAULT_S;
    }
    if (p->path[0] == '\0' || p->backoff_max_s < p->backoff_init_s) {
        return -1; /* required/invalid config -> HPULOGC_ERR_CONFIG */
    }
    return 0;
}

static int unix_start(hpulogc_sink_t* sink)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);

    /* Fail-fast first connect (rollingfile open semantics): the returned
     * error maps to HPULOGC_ERR_IO and fails init. */
    if (p->socktype == 2) {
        p->fd = hpu_net_unix_stream_open(p->path);
    } else {
        p->fd = hpu_net_unix_dgram_open(p->path);
    }
    if (p->fd < 0) {
        return -1;
    }
    p->connected = 1;
    return 0;
}

/**
 * @brief Drop the connection and arm the backoff window.
 *
 * Identical to the TCP sink state machine: the retry gate is set to
 * now + current backoff, then the backoff doubles (capped at reconnect
 * backoff max); a successful reconnect resets it to the initial value.
 */
static void unix_mark_disconnected(hpu_unix_priv_t* p)
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
static int unix_reconnect(hpu_unix_priv_t* p)
{
    if (hpu_now_ns() < p->retry_deadline_ns) {
        return -1;
    }
    /* The endpoint path is config-static, so reconnecting needs no
     * resolution work inside the emit callback (§4.10.4). */
    if (p->socktype == 2) {
        p->fd = hpu_net_unix_stream_open(p->path);
    } else {
        p->fd = hpu_net_unix_dgram_open(p->path);
    }
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
 * @brief Send one record as a single datagram (dgram mode).
 * @return 0 delivered, 1 transient (EAGAIN: drop, stay connected),
 *         -1 disconnected (ECONNREFUSED or hard error).
 */
static int unix_send_dgram(hpu_unix_priv_t* p, const char* data, size_t len)
{
    if (hpu_net_send(p->fd, data, len) != 0) {
        /* The socket is non-blocking; a full peer receive queue maps to
         * EAGAIN (contract guarantee) and is not a disconnect. */
        return (errno == EAGAIN) ? 1 : -1;
    }
    return 0;
}

/**
 * @brief Write out pending coalesced line frames (stream mode).
 *
 * On failure the connection is marked down and the still-buffered lines
 * are lost; @p done is decremented so the caller's return value reflects
 * only records that actually left this process.
 *
 * @return 0 when the caller may continue, -1 when disconnected.
 */
static int unix_flush_pending(hpu_unix_priv_t* p, size_t* done)
{
    size_t lost;

    if (p->buf_len == 0) {
        return 0;
    }
    lost = p->buf_lines;
    if (hpu_net_send(p->fd, p->buf, p->buf_len) != 0) {
        unix_mark_disconnected(p);
        *done -= lost;
        return -1;
    }
    p->buf_len = 0;
    p->buf_lines = 0;
    return 0;
}

static void unix_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);
    int rc;

    if (!p->connected && unix_reconnect(p) != 0) {
        hpu_sink_account_failed(sink, 1); /* inside the backoff window */
        return;
    }
    if (p->socktype == 1) {
        rc = unix_send_dgram(p, ev->line, ev->line_len);
        if (rc == 1) {
            hpu_sink_account_failed(sink, 1); /* EAGAIN: transient drop */
            return;
        }
        if (rc != 0) {
            unix_mark_disconnected(p);
            hpu_sink_account_failed(sink, 1);
        }
        return;
    }
    /* Borrowed event: both sends complete inside this callback. Two
     * sends from the serialized callback keep the line frame contiguous
     * on the stream. */
    if (hpu_net_send(p->fd, ev->line, ev->line_len) != 0 ||
        hpu_net_send(p->fd, "\n", 1) != 0) {
        unix_mark_disconnected(p);
        hpu_sink_account_failed(sink, 1);
    }
}

static int unix_emit_batch(hpulogc_sink_t* sink,
                           const hpulogc_event_t* const* evs, size_t n)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    size_t done = 0;

    if (!p->connected && unix_reconnect(p) != 0) {
        return 0; /* whole batch lands in the backoff window */
    }
    if (p->socktype == 1) {
        for (i = 0; i < n; i++) {
            int rc = unix_send_dgram(p, evs[i]->line, evs[i]->line_len);

            if (rc == 0) {
                done++;
            } else if (rc == 1) {
                /* EAGAIN: this record is failed, later ones still try. */
                continue;
            } else {
                unix_mark_disconnected(p);
                return (done > 0) ? (int)done : -1;
            }
        }
        return (int)done;
    }
    for (i = 0; i < n; i++) {
        const hpulogc_event_t* ev = evs[i];
        size_t need = ev->line_len + 1; /* line frame + '\n' */

        if (need > sizeof(p->buf)) {
            /* Oversized single line: bypass the coalesce buffer. */
            if (unix_flush_pending(p, &done) != 0) {
                return (done > 0) ? (int)done : -1;
            }
            if (hpu_net_send(p->fd, ev->line, ev->line_len) != 0 ||
                hpu_net_send(p->fd, "\n", 1) != 0) {
                unix_mark_disconnected(p);
                return (done > 0) ? (int)done : -1;
            }
            done++;
            continue;
        }
        if (p->buf_len + need > sizeof(p->buf)) {
            if (unix_flush_pending(p, &done) != 0) {
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
    if (unix_flush_pending(p, &done) != 0) {
        return (done > 0) ? (int)done : -1;
    }
    return (int)done;
}

static void unix_destroy(hpulogc_sink_t* sink)
{
    hpu_unix_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
}

/** @brief Unix domain socket sink type (SYNC|ASYNC|LINE_ATOMIC;
 *         POSIX builds). */
static const hpulogc_sink_ops_t g_unix_ops = {
    "unix",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_unix_priv_t),
    unix_configure,
    unix_init,
    unix_start,
    unix_emit,
    unix_emit_batch,
    NULL, /* flush: the coalesce buffer is drained per emit_batch call */
    NULL, /* sync: no FSYNC capability */
    NULL, /* periodic: reconnect is emit-path driven */
    unix_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_unix_sink_ops(void)
{
    return &g_unix_ops;
}

#endif /* !defined(_WIN32) */
