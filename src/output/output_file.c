/**
 * @file output_file.c
 * @brief File output backend: batched writes, reopen-on-failure, fsync
 *        policy enforcement and rotation glue.
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
 * @brief File output instance.
 */
typedef struct hpu_file_out {
    hpu_output_base_t base;  /*!< Common header (offset 0) */
    hpu_mutex_t mu;          /*!< Serializes writes/rotation (sync mode) */
    char path[HPULOGC_MAX_PATH_LEN]; /*!< Active file path */
    unsigned file_mode;      /*!< Creation permission bits */
    hpu_rotate_cfg_t rotate; /*!< Rotation parameters */
    int use_utc;             /*!< Timezone for buckets/naming */
    int symlink_latest;      /*!< Maintain <path>.latest */
    int fd;                  /*!< Active descriptor, -1 when closed */
    int64_t file_size;       /*!< Active file size in bytes */
    int64_t next_boundary;   /*!< Next time bucket boundary (epoch sec) */
    uint64_t last_fsync_ns;  /*!< Last periodic fsync (monotonic ns) */
    size_t pending_lines;    /*!< Lines sitting in iobuf */
    unsigned long long lost; /*!< Lines lost to write failures */
    char iobuf[HPU_OUT_IO_BUF_SIZE]; /*!< Pending write bytes */
    size_t iolen;            /*!< Pending byte count */
} hpu_file_out_t;

/**
 * @brief Open (or reopen) the active file and reset the size counter.
 * @return 0 on success, -1 on failure (errno set).
 */
static int file_open_active(hpu_file_out_t* f)
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
static int file_flush_locked(hpu_file_out_t* f)
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

    /* Still failing: the pending lines are lost (counted per line). */
    f->lost += lines;
    return -1;
}

hpu_output_t* hpu_file_output_open(const hpulogc_output_t* cfg,
                                   int effective_fsync)
{
    hpu_file_out_t* f = calloc(1, sizeof(*f));
    const char* path = cfg->path != NULL ? cfg->path : "";
    char dir[HPULOGC_MAX_PATH_LEN];

    if (f == NULL) {
        return NULL;
    }
    f->base.type = HPULOGC_OUT_FILE;
    f->base.fsync_sev = effective_fsync;
    snprintf(f->base.name, sizeof(f->base.name), "%s", path);
    snprintf(f->path, sizeof(f->path), "%s", path);
    f->file_mode = cfg->file_mode != 0 ? cfg->file_mode : 0644;
    f->use_utc = 0; /* patched by the config layer when timezone = utc */
    f->symlink_latest = cfg->symlink_latest;
    f->fd = -1;

    f->rotate.enabled = (cfg->rotate != HPULOGC_ROTATE_NONE);
    f->rotate.by_size = (cfg->rotate == HPULOGC_ROTATE_SIZE ||
                         cfg->rotate == HPULOGC_ROTATE_BOTH);
    f->rotate.by_time = (cfg->rotate == HPULOGC_ROTATE_TIME ||
                         cfg->rotate == HPULOGC_ROTATE_BOTH);
    f->rotate.max_size = cfg->max_size;
    f->rotate.time_unit = (int)cfg->time_unit;
    f->rotate.max_files = cfg->max_files;
    if (cfg->rotate_naming != NULL) {
        snprintf(f->rotate.naming, sizeof(f->rotate.naming), "%s",
                 cfg->rotate_naming);
    } else {
        snprintf(f->rotate.naming, sizeof(f->rotate.naming), "%s",
                 HPU_ROTATE_DEFAULT_NAMING);
    }

    /* Create parent directories with the configured permissions. */
    if (hpu_path_dirname(f->path, dir, sizeof(dir)) == 0 &&
        strcmp(dir, ".") != 0) {
        if (hpu_fs_mkdir_all(dir,
                             cfg->dir_mode != 0 ? cfg->dir_mode : 0755) != 0) {
            free(f);
            return NULL;
        }
    }

    if (hpu_mutex_init(&f->mu) != 0) {
        free(f);
        return NULL;
    }
    if (file_open_active(f) != 0) {
        hpu_mutex_destroy(&f->mu);
        free(f);
        return NULL;
    }

#if HPULOGC_ENABLE_ROTATE
    if (f->rotate.by_time) {
        f->next_boundary = hpu_rotate_next_boundary(
            hpu_realtime_ns() / 1000000000LL, f->rotate.time_unit,
            f->use_utc);
    }
#endif
    return (hpu_output_t*)(void*)f;
}

void hpu_file_output_close(hpu_output_t* o)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;

    if (f == NULL) {
        return;
    }
    hpu_mutex_lock(&f->mu);
    if (f->fd >= 0) {
        hpu_fs_close(f->fd);
        f->fd = -1;
    }
    hpu_mutex_unlock(&f->mu);
    hpu_mutex_destroy(&f->mu);
    free(f);
}

int hpu_file_output_write_line(hpu_output_t* o, const char* line,
                               size_t len)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc = 0;

    hpu_mutex_lock(&f->mu);
    if (f->fd < 0) {
        if (file_open_active(f) != 0) {
            hpu_mutex_unlock(&f->mu);
            return -1;
        }
    }
    if (f->iolen + len > sizeof(f->iobuf)) {
        if (file_flush_locked(f) != 0) {
            rc = -1;
        }
    }
    if (len > sizeof(f->iobuf)) {
        /* Oversized single line: write directly. */
        if (hpu_fs_write(f->fd, line, len) == 0) {
            f->file_size += (int64_t)len;
        } else {
            rc = -1;
            f->lost += 1;
        }
    } else {
        memcpy(f->iobuf + f->iolen, line, len);
        f->iolen += len;
        f->pending_lines++;
        if (f->iolen >= FLUSH_THRESHOLD) {
            if (file_flush_locked(f) != 0) {
                rc = -1;
            }
        } else if (f->base.fsync_sev >= HPU_FSYNC_SEV_ENTRY) {
            /* per-entry fsync policy (spec 9) */
            if (file_flush_locked(f) != 0) {
                rc = -1;
            } else if (f->fd >= 0 && hpu_fs_sync(f->fd) != 0) {
                rc = -1;
            }
        }
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

int hpu_file_output_flush(hpu_output_t* o)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc;

    hpu_mutex_lock(&f->mu);
    rc = file_flush_locked(f);
    hpu_mutex_unlock(&f->mu);
    return rc;
}

int hpu_file_output_fsync(hpu_output_t* o)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc = 0;

    hpu_mutex_lock(&f->mu);
    if (file_flush_locked(f) != 0) {
        rc = -1;
    }
    if (f->fd >= 0 && hpu_fs_sync(f->fd) != 0) {
        rc = -1;
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

int hpu_file_output_flush_fsync(hpu_output_t* o)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc;

    hpu_mutex_lock(&f->mu);
    rc = file_flush_locked(f);
    if (rc == 0 && f->fd >= 0 && hpu_fs_sync(f->fd) != 0) {
        rc = -1;
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

int hpu_file_output_periodic(hpu_output_t* o, uint64_t now_ns,
                             uint64_t interval_ns)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc = 0;

    if (f->base.fsync_sev < HPU_FSYNC_SEV_PERIODIC) {
        return 0;
    }
    hpu_mutex_lock(&f->mu);
    if (f->last_fsync_ns == 0) {
        f->last_fsync_ns = now_ns;
    } else if (now_ns - f->last_fsync_ns >= interval_ns) {
        rc = file_flush_locked(f);
        if (rc == 0 && f->fd >= 0) {
            rc = hpu_fs_sync(f->fd);
        }
        f->last_fsync_ns = now_ns;
    }
    hpu_mutex_unlock(&f->mu);
    return rc;
}

int hpu_file_output_before_write(hpu_output_t* o, int64_t ts_sec)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    int rc = 0;
    int need_rotate = 0;

    hpu_mutex_lock(&f->mu);
#if HPULOGC_ENABLE_ROTATE
    if (f->rotate.enabled) {
        if (f->rotate.by_size && f->rotate.max_size > 0 &&
            f->file_size + (int64_t)f->iolen >=
                (int64_t)f->rotate.max_size) {
            need_rotate = 1;
        }
        if (!need_rotate && f->rotate.by_time && f->next_boundary > 0 &&
            ts_sec >= f->next_boundary) {
            need_rotate = 1;
        }
    }
    if (need_rotate) {
        /* Pending bytes belong to the old file: flush first. */
        if (file_flush_locked(f) != 0) {
            rc = -1;
        } else {
            rc = hpu_rotate_file(f->path, &f->fd, &f->file_size,
                                 &f->rotate, f->use_utc, ts_sec,
                                 f->symlink_latest, f->file_mode);
            if (rc == 0 && f->rotate.by_time) {
                f->next_boundary = hpu_rotate_next_boundary(
                    ts_sec, f->rotate.time_unit, f->use_utc);
            } else if (rc != 0) {
                rc = -1;
            }
        }
    }
#else
    (void)ts_sec;
    (void)need_rotate;
#endif
    hpu_mutex_unlock(&f->mu);
    return rc;
}

/**
 * @brief Report the cumulative lost-line counter (core statistics).
 *
 * @param o  File output handle.
 * @return   Lines lost to write failures so far.
 */
void hpu_file_output_set_utc(hpu_output_t* o, int use_utc)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;

    if (f == NULL) {
        return;
    }
    hpu_mutex_lock(&f->mu);
    if (f->use_utc != use_utc) {
        f->use_utc = use_utc;
#if HPULOGC_ENABLE_ROTATE
        if (f->rotate.by_time) {
            f->next_boundary = hpu_rotate_next_boundary(
                hpu_realtime_ns() / 1000000000LL, f->rotate.time_unit,
                f->use_utc);
        }
#endif
    }
    hpu_mutex_unlock(&f->mu);
}

unsigned long long hpu_file_output_lost_total(hpu_output_t* o)
{
    hpu_file_out_t* f = (hpu_file_out_t*)(void*)o;
    unsigned long long lost;

    if (f == NULL) {
        return 0;
    }
    hpu_mutex_lock(&f->mu);
    lost = f->lost;
    hpu_mutex_unlock(&f->mu);
    return lost;
}
