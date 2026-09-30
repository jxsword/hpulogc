/* -*- coding: utf-8 -*- */

/**
 * @file sink_fifo.c
 * @brief Named pipe (FIFO) sink (POSIX only): write-end only, constant
 *        O_NONBLOCK (rd_v0.6 §4.10.9).
 *
 * The library only opens the write end of an existing FIFO; it never
 * creates endpoints and does no popen/child-process management (appendix
 * A.1 #10). Unlike TCP/UDS, start does not fail-fast when no reader is
 * present (ENXIO): the instance enters a waiting state and retries on
 * the emit path with the same exponential backoff as the TCP sink — a
 * reader legitimately starts later than the library (D-R20). A reader
 * closing mid-run surfaces as EPIPE and reopens with backoff. The FIFO
 * is never allowed to block the caller: EAGAIN (pipe full) discards the
 * record with failed accounting (D-S5 spirit). SIGPIPE is suppressed
 * per write via a thread-local signal mask, never by changing the
 * process-wide disposition.
 */

#include "output.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../platform/platform.h"

/** @brief Single-record staging buffer size: rendered line + '\n'
 *         (pre-allocated in priv). */
#define HPU_FIFO_STAGE_SIZE (64U * 1024U + 1U)

/** @brief Default/limit values for the reopen backoff (seconds),
 *         identical to the TCP sink (rd_v0.6 §4.10.8). */
#define HPU_FIFO_BACKOFF_DEFAULT_S   1
#define HPU_FIFO_BACKOFF_MAX_DEFAULT_S 30
#define HPU_FIFO_BACKOFF_MAX_LIMIT_S   3600

/** @brief Maximum accepted endpoint path length (with NUL). */
#define HPU_FIFO_PATH_MAX 128

/**
 * @brief FIFO sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_fifo_priv {
    char          path[HPU_FIFO_PATH_MAX]; /*!< Configured endpoint path */
    int           backoff_init_s;          /*!< reconnect backoff (seconds) */
    int           backoff_max_s;           /*!< reconnect backoff max (seconds) */
    int           fd;                      /*!< Write-end fd, -1 when closed */
    uint64_t      backoff_ns;              /*!< Current backoff window */
    uint64_t      retry_deadline_ns;       /*!< Monotonic reopen gate */
    unsigned char stage[HPU_FIFO_STAGE_SIZE]; /*!< line + '\n' staging */
} hpu_fifo_priv_t;

/**
 * @brief Strict decimal parser for bounded non-negative ints.
 * @return 0 on success, -1 on malformed input or range violation.
 */
static int fifo_parse_uint(const char* val, long lo, long hi, int* out)
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

static int fifo_configure(hpulogc_sink_t* sink, const char* key,
                          const char* val)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "path") == 0) {
        if (val == NULL || val[0] == '\0' ||
            strlen(val) >= sizeof(p->path)) {
            return -1;
        }
        memcpy(p->path, val, strlen(val) + 1);
        return 0;
    }
    if (strcmp(key, "reconnect backoff") == 0) {
        return fifo_parse_uint(val, 1, 60, &p->backoff_init_s);
    }
    if (strcmp(key, "reconnect backoff max") == 0) {
        return fifo_parse_uint(val, 1, HPU_FIFO_BACKOFF_MAX_LIMIT_S,
                               &p->backoff_max_s);
    }
    return -1; /* unknown key */
}

static int fifo_init(hpulogc_sink_t* sink)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);

    if (p->backoff_init_s == 0) {
        p->backoff_init_s = HPU_FIFO_BACKOFF_DEFAULT_S;
    }
    if (p->backoff_max_s == 0) {
        p->backoff_max_s = HPU_FIFO_BACKOFF_MAX_DEFAULT_S;
    }
    if (p->path[0] == '\0' || p->backoff_max_s < p->backoff_init_s) {
        return -1; /* required/invalid config -> HPULOGC_ERR_CONFIG */
    }
    return 0;
}

/**
 * @brief Arm (or extend) the reopen backoff window after a lost or
 *        never-established write end.
 */
static void fifo_arm_backoff(hpu_fifo_priv_t* p)
{
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
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
 * @brief Open the FIFO write end (O_NONBLOCK, never O_CREAT) and verify
 *        FIFO identity on the open descriptor.
 *
 * ENXIO (no reader) and ENOENT (endpoint removed mid-run) are retryable:
 * the caller stays in the waiting state with backoff (D-R20). All other
 * open errors and a non-FIFO endpoint are hard failures (start maps
 * them to HPULOGC_ERR_IO fail-fast).
 *
 * @return 0 opened, 1 retryable failure (waiting state armed), -2 hard
 *         failure.
 */
static int fifo_open_write_end(hpu_fifo_priv_t* p)
{
    struct stat st;
    int fd;

    fd = open(p->path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENXIO || errno == ENOENT) {
            fifo_arm_backoff(p);
            return 1; /* retryable: reader absent / endpoint removed */
        }
        return -2; /* EACCES, EISDIR, ... -> start fail-fast */
    }
    if (fstat(fd, &st) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -2;
    }
    if (!S_ISFIFO(st.st_mode)) {
        /* Not a FIFO (config error, race-free check on the descriptor):
         * fail-fast so the misconfiguration surfaces at init. */
        close(fd);
        errno = EINVAL;
        return -2;
    }
    p->fd = fd;
    return 0;
}

static int fifo_start(hpulogc_sink_t* sink)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);
    int rc = fifo_open_write_end(p);

    if (rc == -2) {
        return -1; /* HPULOGC_ERR_IO fail-fast (bad path or non-FIFO) */
    }
    /* rc 0 (opened) or 1 (waiting state): both are valid start outcomes
     * (§4.10.9: a missing reader is not a start failure). */
    return 0;
}

/**
 * @brief Attempt to reopen the write end when the backoff window has
 *        elapsed.
 * @return 0 open, -1 still closed (backoff window or retryable error).
 */
static int fifo_try_reopen(hpu_fifo_priv_t* p)
{
    int rc;

    if (hpu_now_ns() < p->retry_deadline_ns) {
        return -1;
    }
    rc = fifo_open_write_end(p);
    if (rc != 0) {
        /* Retryable (armed again by fifo_open_write_end) or a hard error
         * surfacing mid-run: both stay in the closed state; a hard error
         * arms the window here so emit does not spin on open(). */
        if (rc == -2) {
            fifo_arm_backoff(p);
        }
        return -1;
    }
    p->backoff_ns = (uint64_t)p->backoff_init_s * 1000000000ULL;
    p->retry_deadline_ns = 0;
    return 0;
}

/**
 * @brief Write one record with SIGPIPE suppressed via a thread-local
 *        signal mask (never a process-wide disposition change).
 *
 * @return 0 fully written, 1 transient (EAGAIN: nothing or a torn
 *         partial for records larger than PIPE_BUF made it in; the
 *         record is failed, the fd stays open), -1 disconnected
 *         (EPIPE or hard error; caller marks the write end lost).
 */
static int fifo_write_record(hpu_fifo_priv_t* p, size_t len)
{
    sigset_t block;
    sigset_t old;
    size_t off = 0;
    int rc = 0;

    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &block, &old) != 0) {
        return -1;
    }
    while (off < len) {
        ssize_t n = write(p->fd, p->stage + off, len - off);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                rc = 1; /* pipe full: transient discard, not a disconnect */
                break;
            }
            rc = -1; /* EPIPE (reader gone) or hard error */
            break;
        }
        off += (size_t)n;
    }
    /* A SIGPIPE raised by the writes above is pending for this thread:
     * consume it before restoring the mask, otherwise the restore
     * delivers it with the default disposition and kills the host
     * process — exactly what suppression must prevent. SIGPIPE can only
     * have been generated by our own writes inside this function, so
     * checking the pending set here is race-free; sigwait (POSIX.1-1995,
     * unlike sigtimedwait) returns immediately because the signal is
     * already pending. */
    {
        sigset_t pending;

        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
            int sig = 0;

            (void)sigwait(&block, &sig);
        }
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return rc;
}

/**
 * @brief Stage one record (line + '\n') and write it.
 * @return 0 delivered, 1 failed (transient or lost), 2 lost with the
 *         write end marked disconnected.
 */
static int fifo_deliver(hpulogc_sink_t* sink, hpu_fifo_priv_t* p,
                        const hpulogc_event_t* ev)
{
    size_t len = ev->line_len;
    int rc;

    if (len >= sizeof(p->stage)) {
        len = sizeof(p->stage) - 1; /* line is already max_log_length
                                       bounded; hard cap for safety */
    }
    if (len > 0) {
        memcpy(p->stage, ev->line, len);
    }
    p->stage[len] = '\n';
    rc = fifo_write_record(p, len + 1);
    if (rc == 0) {
        return 0;
    }
    if (rc == 1) {
        hpu_sink_account_failed(sink, 1);
        return 1;
    }
    fifo_arm_backoff(p);
    hpu_sink_account_failed(sink, 1);
    return 2;
}

static void fifo_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd < 0 && fifo_try_reopen(p) != 0) {
        hpu_sink_account_failed(sink, 1); /* inside the backoff window */
        return;
    }
    (void)fifo_deliver(sink, p, ev);
}

static int fifo_emit_batch(hpulogc_sink_t* sink,
                           const hpulogc_event_t* const* evs, size_t n)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    size_t done = 0;
    int lost_all = 0;

    if (p->fd < 0 && fifo_try_reopen(p) != 0) {
        return 0; /* whole batch lands in the backoff window */
    }
    for (i = 0; i < n; i++) {
        int rc = fifo_deliver(sink, p, evs[i]);

        if (rc == 0) {
            done++;
        } else if (rc == 1) {
            /* transient discard: later records still try (EAGAIN may
             * clear once the reader drains) */
            continue;
        } else {
            /* write end lost: the rest of the batch is lost
             * (at-most-once), no per-record write attempt */
            lost_all = 1;
            break;
        }
    }
    if (lost_all) {
        return (done > 0) ? (int)done : -1;
    }
    return (int)done;
}

static void fifo_destroy(hpulogc_sink_t* sink)
{
    hpu_fifo_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

/** @brief FIFO sink type (SYNC|ASYNC|LINE_ATOMIC; POSIX builds). */
static const hpulogc_sink_ops_t g_fifo_ops = {
    "fifo",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_fifo_priv_t),
    fifo_configure,
    fifo_init,
    fifo_start,
    fifo_emit,
    fifo_emit_batch,
    NULL, /* flush: each record is written inside its own emit call */
    NULL, /* sync: no FSYNC capability */
    NULL, /* periodic: reopen is emit-path driven */
    fifo_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_fifo_sink_ops(void)
{
    return &g_fifo_ops;
}

#endif /* !defined(_WIN32) */
