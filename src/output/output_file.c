/**
 * @file output_file.c
 * @brief rollingfile sink: batched writes, reopen-on-failure, fsync policy
 *        enforcement, rotation glue and the I/O failure injection hook.
 */

#include "output.h"
#include "rotate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

/** @brief Pending byte threshold that forces a flush. */
#define FLUSH_THRESHOLD (48U * 1024U)

/** @brief Test-only injection point (NULL = never fails). */
int (*hpu_io_fail_hook)(int op, const char* path) = NULL;

/**
 * @brief rollingfile sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_file_priv {
    hpu_mutex_t mu;          /*!< Serializes writes/rotation (sync mode) */
    int mu_ready;            /*!< Mutex initialized (destroy guard) */
    char path[HPULOGC_MAX_PATH_LEN]; /*!< Active file path */
    unsigned file_mode;      /*!< Creation permission bits */
    unsigned dir_mode;       /*!< Parent directory permission bits */
    hpu_rotate_cfg_t rotate; /*!< Rotation parameters */
    int symlink_latest;      /*!< Maintain <path>.latest */
    int fsync_key;           /*!< Per-sink fsync=true (upgrades severity) */
    int fd;                  /*!< Active descriptor, -1 when closed */
    int64_t file_size;       /*!< Active file size in bytes */
    int64_t next_boundary;   /*!< Next time bucket boundary (epoch sec) */
    uint64_t last_fsync_ns;  /*!< Last periodic fsync (monotonic ns) */
    size_t pending_lines;    /*!< Lines sitting in iobuf */
    char iobuf[HPU_OUT_IO_BUF_SIZE]; /*!< Pending write bytes */
    size_t iolen;            /*!< Pending byte count */
} hpu_file_priv_t;

/**
 * @brief Open (or reopen) the active file and reset the size counter.
 * @return 0 on success, -1 on failure (errno set).
 */
static int file_open_active(hpu_file_priv_t* f)
{
    int fd;

    if (hpu_io_fail_hook != NULL &&
        hpu_io_fail_hook(0, f->path) != 0) {
        errno = EIO;
        return -1;
    }
    fd = hpu_fs_open_append(f->path, f->file_mode);

    if (fd < 0) {
        return -1;
    }
    f->fd = fd;
    f->file_size = hpu_fs_size(fd);
    if (f->file_size < 0) {
        f->file_size = 0;
    }
    return 0;
}

/**
 * @brief Flush pending bytes; on failure reopen once and retry.
 * @return 0 on success, -1 when the pending bytes were lost.
 */
static int file_flush_locked(hpulogc_sink_t* sink, hpu_file_priv_t* f)
{
    size_t len = f->iolen;
    size_t lines = f->pending_lines;
    const char* data = f->iobuf;

    if (len == 0) {
        return 0;
    }
    f->iolen = 0;
    f->pending_lines = 0;

    if (hpu_io_fail_hook == NULL || hpu_io_fail_hook(1, f->path) == 0) {
        if (hpu_fs_write(f->fd, data, len) == 0) {
            f->file_size += (int64_t)len;
            return 0;
        }
    }

    /* Runtime write failure: reopen once and retry (spec 9). */
    hpu_fs_close(f->fd);
    f->fd = -1;
    if (file_open_active(f) == 0 &&
        (hpu_io_fail_hook == NULL || hpu_io_fail_hook(1, f->path) == 0) &&
        hpu_fs_write(f->fd, data, len) == 0) {
        f->file_size += (int64_t)len;
        return 0;
    }

    /* Still failing: the pending lines are lost (per-sink failed, §4.10.3). */
    hpu_sink_account_failed(sink, lines);
    return -1;
}

static int file_configure(hpulogc_sink_t* sink, const char* key,
                          const char* val)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);

    if (strcmp(key, "path") == 0) {
        if (val == NULL || val[0] == '\0' ||
            strlen(val) >= sizeof(f->path)) {
            return -1;
        }
        snprintf(f->path, sizeof(f->path), "%s", val);
        return 0;
    }
    if (strcmp(key, "file perms") == 0) {
        if (val == NULL) {
            return -1;
        }
        f->file_mode = (unsigned)strtol(val, NULL, 8) & 07777u;
        return 0;
    }
    if (strcmp(key, "dir perms") == 0) {
        if (val == NULL) {
            return -1;
        }
        f->dir_mode = (unsigned)strtol(val, NULL, 8) & 07777u;
        return 0;
    }
    if (strcmp(key, "rotate") == 0) {
        if (val == NULL) {
            return -1;
        }
        f->rotate.enabled = strcmp(val, "none") != 0;
        f->rotate.by_size = strcmp(val, "size") == 0 ||
                            strcmp(val, "both") == 0;
        f->rotate.by_time = strcmp(val, "time") == 0 ||
                            strcmp(val, "both") == 0;
        if (!f->rotate.enabled && strcmp(val, "none") != 0) {
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "max size") == 0) {
        unsigned long long sz;

        if (val == NULL || hpu_parse_size_str(val, &sz) != 0) {
            return -1;
        }
        f->rotate.max_size = (size_t)sz;
        return 0;
    }
    if (strcmp(key, "time unit") == 0) {
        if (val == NULL) {
            return -1;
        }
        if (strcmp(val, "hour") == 0) {
            f->rotate.time_unit = HPULOGC_TU_HOUR;
        } else if (strcmp(val, "day") == 0) {
            f->rotate.time_unit = HPULOGC_TU_DAY;
        } else if (strcmp(val, "week") == 0) {
            f->rotate.time_unit = HPULOGC_TU_WEEK;
        } else if (strcmp(val, "month") == 0) {
            f->rotate.time_unit = HPULOGC_TU_MONTH;
        } else {
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "max files") == 0) {
        long v;

        if (val == NULL) {
            return -1;
        }
        v = strtol(val, NULL, 10);
        if (v < 0 || v > 100000) {
            return -1;
        }
        f->rotate.max_files = (int)v;
        return 0;
    }
    if (strcmp(key, "fsync") == 0) {
        if (val == NULL ||
            (strcmp(val, "true") != 0 && strcmp(val, "false") != 0)) {
            return -1;
        }
        f->fsync_key = strcmp(val, "true") == 0;
        return 0;
    }
    if (strcmp(key, "symlink latest") == 0) {
        if (val == NULL ||
            (strcmp(val, "true") != 0 && strcmp(val, "false") != 0)) {
            return -1;
        }
        f->symlink_latest = strcmp(val, "true") == 0;
#if defined(_WIN32)
        fprintf(stderr,
                "hpulogc: symlink latest ignored on Windows (sink %s)\n",
                hpu_sink_base(sink)->name);
#endif
        return 0;
    }
    if (strcmp(key, "rotate naming") == 0) {
        if (val == NULL || strlen(val) >= sizeof(f->rotate.naming)) {
            return -1;
        }
        snprintf(f->rotate.naming, sizeof(f->rotate.naming), "%s", val);
        return 0;
    }
    return -1; /* unknown key */
}

static int file_init(hpulogc_sink_t* sink)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    hpu_output_base_t* b = hpu_sink_base(sink);

    if (f->path[0] == '\0') {
        fprintf(stderr, "hpulogc: rollingfile sink '%s' requires 'path'\n",
                b->name);
        return -1;
    }
    if (f->file_mode == 0) {
        f->file_mode = 0644;
    }
    if (f->dir_mode == 0) {
        f->dir_mode = 0755;
    }
    if (f->rotate.naming[0] == '\0') {
        snprintf(f->rotate.naming, sizeof(f->rotate.naming), "%s",
                 HPU_ROTATE_DEFAULT_NAMING);
    }
    if (f->fsync_key) {
        b->fsync_sev = HPU_FSYNC_SEV_ENTRY; /* per-sink fsync is stricter */
    }
    if (b->name[0] == '\0') {
        snprintf(b->name, sizeof(b->name), "%s", f->path);
    }
    return 0;
}

static int file_start(hpulogc_sink_t* sink)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    char dir[HPULOGC_MAX_PATH_LEN];

    /* Create parent directories with the configured permissions. */
    if (hpu_path_dirname(f->path, dir, sizeof(dir)) == 0 &&
        strcmp(dir, ".") != 0) {
        if (hpu_fs_mkdir_all(dir, f->dir_mode) != 0) {
            return -1;
        }
    }
    if (hpu_mutex_init(&f->mu) != 0) {
        return -1;
    }
    f->mu_ready = 1;
    f->fd = -1;
    if (file_open_active(f) != 0) {
        return -1;
    }
#if HPULOGC_ENABLE_ROTATE
    if (f->rotate.by_time) {
        f->next_boundary = hpu_rotate_next_boundary(
            hpu_realtime_ns() / 1000000000LL, f->rotate.time_unit,
            hpu_sink_base(sink)->use_utc);
    }
#endif
    return 0;
}

/**
 * @brief Emit one event with the mutex held.
 *
 * Shared by the single-event and batched delivery paths. On failure the
 * event is lost; the caller owns the `failed` accounting (the batch path
 * reports the success count instead, so accounting must stay here-free).
 *
 * @return 0 when the line is buffered/written, -1 when it is lost.
 */
static int file_emit_one_locked(hpulogc_sink_t* sink, hpu_file_priv_t* f,
                                const hpulogc_event_t* ev)
{
    const char* line = ev->line;
    size_t len = ev->line_len;
    int64_t ts_sec = ev->realtime_ns / 1000000000LL;
    int rc = 0;

#if HPULOGC_ENABLE_ROTATE
    if (f->rotate.enabled) {
        int need_rotate = 0;

        /* O(1) rotation checks before the line is committed (§4.6). */
        if (f->rotate.by_size && f->rotate.max_size > 0 &&
            f->file_size + (int64_t)f->iolen >=
                (int64_t)f->rotate.max_size) {
            need_rotate = 1;
        }
        if (!need_rotate && f->rotate.by_time && f->next_boundary > 0 &&
            ts_sec >= f->next_boundary) {
            need_rotate = 1;
        }
        if (need_rotate) {
            /* Pending bytes belong to the old file: flush first. */
            if (file_flush_locked(sink, f) != 0) {
                return -1;
            }
            rc = hpu_rotate_file(f->path, &f->fd, &f->file_size,
                                 &f->rotate,
                                 hpu_sink_base(sink)->use_utc, ts_sec,
                                 f->symlink_latest, f->file_mode);
            if (rc == 0 && f->rotate.by_time) {
                f->next_boundary = hpu_rotate_next_boundary(
                    ts_sec, f->rotate.time_unit,
                    hpu_sink_base(sink)->use_utc);
            } else if (rc != 0) {
                /* Rotation failure: the line is lost. */
                return -1;
            }
        }
    }
#else
    (void)ts_sec;
#endif
    if (f->fd < 0) {
        if (file_open_active(f) != 0) {
            return -1;
        }
    }
    if (f->iolen + len > sizeof(f->iobuf)) {
        if (file_flush_locked(sink, f) != 0) {
            return -1;
        }
    }
    if (len > sizeof(f->iobuf)) {
        /* Oversized single line: write directly. */
        if (hpu_fs_write(f->fd, line, len) == 0) {
            f->file_size += (int64_t)len;
        } else {
            return -1;
        }
    } else {
        memcpy(f->iobuf + f->iolen, line, len);
        f->iolen += len;
        f->pending_lines++;
        if (f->iolen >= FLUSH_THRESHOLD) {
            if (file_flush_locked(sink, f) != 0) {
                return -1;
            }
        } else if (hpu_sink_base(sink)->fsync_sev >= HPU_FSYNC_SEV_ENTRY) {
            /* per-entry fsync policy (spec 9) */
            if (file_flush_locked(sink, f) != 0) {
                return -1;
            }
            if (f->fd >= 0 && hpu_fs_sync(f->fd) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static void file_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    int rc;

    hpu_mutex_lock(&f->mu);
    rc = file_emit_one_locked(sink, f, ev);
    hpu_mutex_unlock(&f->mu);
    if (rc != 0) {
        hpu_sink_account_failed(sink, 1);
    }
}

static int file_emit_batch(hpulogc_sink_t* sink,
                           const hpulogc_event_t* const* evs, size_t n)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    size_t i;
    int done = 0;

    /* One lock acquisition amortizes the mutex cost over the whole batch;
     * per-event rotation decisions and buffer flushes stay identical to
     * the single-event path. Failures are reported via the return value
     * (§4.10.6: core books done into written, the rest into failed). */
    hpu_mutex_lock(&f->mu);
    for (i = 0; i < n; i++) {
        if (file_emit_one_locked(sink, f, evs[i]) == 0) {
            done++;
        }
    }
    hpu_mutex_unlock(&f->mu);
    return done;
}

static int file_flush(hpulogc_sink_t* sink)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    int rc;

    hpu_mutex_lock(&f->mu);
    rc = file_flush_locked(sink, f);
    hpu_mutex_unlock(&f->mu);
    return rc;
}

static int file_sync(hpulogc_sink_t* sink)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    int rc = 0;

    hpu_mutex_lock(&f->mu);
    if (file_flush_locked(sink, f) != 0) {
        rc = -1;
    }
    if (f->fd >= 0 && hpu_fs_sync(f->fd) != 0) {
        rc = -1;
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

static int file_periodic(hpulogc_sink_t* sink, uint64_t now_ns,
                         uint64_t interval_ns)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);
    int rc = 0;

    if (hpu_sink_base(sink)->fsync_sev < HPU_FSYNC_SEV_PERIODIC) {
        return 0;
    }
    hpu_mutex_lock(&f->mu);
    if (f->last_fsync_ns == 0) {
        f->last_fsync_ns = now_ns;
    } else if (now_ns - f->last_fsync_ns >= interval_ns) {
        rc = file_flush_locked(sink, f);
        if (rc == 0 && f->fd >= 0) {
            rc = hpu_fs_sync(f->fd);
        }
        f->last_fsync_ns = now_ns;
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

static void file_destroy(hpulogc_sink_t* sink)
{
    hpu_file_priv_t* f = hpulogc_sink_priv(sink);

    if (f->mu_ready) {
        hpu_mutex_lock(&f->mu);
        if (f->fd >= 0) {
            hpu_fs_close(f->fd);
            f->fd = -1;
        }
        hpu_mutex_unlock(&f->mu);
        hpu_mutex_destroy(&f->mu);
        f->mu_ready = 0;
    }
}

/** @brief rollingfile sink type (all four capabilities). */
static const hpulogc_sink_ops_t g_rollingfile_ops = {
    "rollingfile",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC |
        HPULOGC_CAP_FSYNC,
    sizeof(hpu_file_priv_t),
    file_configure,
    file_init,
    file_start,
    file_emit,
    file_emit_batch,
    file_flush,
    file_sync,
    file_periodic,
    file_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_rollingfile_sink_ops(void)
{
    return &g_rollingfile_ops;
}
