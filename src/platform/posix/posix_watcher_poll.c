/**
 * @file posix_watcher_poll.c
 * @brief Poll fallback watcher backend: mtime + size comparison.
 *
 * This is the portable backend for systems without a file change
 * notification mechanism (and the Linux fallback). Resolution is bounded
 * by the polling interval; the check uses nanosecond mtime plus size so
 * fast successive edits are still detected.
 */

#include "posix_watcher_poll.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/**
 * @brief Sleep for a duration without signal-unfriendly busy waits.
 * @param ms  Duration in milliseconds.
 */
static void poll_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* retry the remaining time */
    }
}

/**
 * @brief Snapshot the current mtime/size of the watched file.
 * @param w  Watcher handle.
 * @return   0 when the file is readable, -1 otherwise.
 */
static int poll_snapshot(hpu_watcher_t* w)
{
    struct stat st;

    if (stat(w->path, &st) != 0) {
        /* Missing file counts as a state change relative to an existing
         * baseline; the reload path reports the parse error. */
        if (w->primed && (w->last_size != -1)) {
            w->last_size = -1;
            return 1;
        }
        w->last_size = -1;
        return 0;
    }

    if (!w->primed) {
        w->last_mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ULL +
                           (uint64_t)st.st_mtim.tv_nsec;
        w->last_size = (int64_t)st.st_size;
        w->primed    = 1;
        return 0;
    }

    {
        uint64_t mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000ULL +
                            (uint64_t)st.st_mtim.tv_nsec;
        if (mtime_ns != w->last_mtime_ns || (int64_t)st.st_size != w->last_size) {
            w->last_mtime_ns = mtime_ns;
            w->last_size     = (int64_t)st.st_size;
            return 1;
        }
    }
    return 0;
}

int hpu_poll_watcher_start(hpu_watcher_t* w, const char* path)
{
    w->use_inotify = 0;
    w->fd = -1;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->primed = 0;
    w->last_size = 0;
    w->last_mtime_ns = 0;
    poll_snapshot(w); /* establish the baseline; missing file is fine */
    return 0;
}

int hpu_poll_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    uint32_t step = timeout_ms < 200U ? (timeout_ms == 0 ? 1U : timeout_ms)
                                      : 200U;

    for (;;) {
        int changed = poll_snapshot(w);
        if (changed > 0) {
            return 1;
        }
        if (waited >= timeout_ms) {
            return 0;
        }
        if (waited + step > timeout_ms) {
            step = timeout_ms - waited;
        }
        poll_sleep_ms(step);
        waited += step;
    }
}

void hpu_poll_watcher_stop(hpu_watcher_t* w)
{
    w->primed = 0;
    w->fd = -1;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
}
