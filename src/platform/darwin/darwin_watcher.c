/**
 * @file darwin_watcher.c
 * @brief macOS watcher: kqueue (EVFILT_VNODE) backend with poll fallback.
 *
 * Watches the configuration file vnode (NOTE_WRITE/EXTEND/ATTRIB plus
 * DELETE/RENAME) and its parent directory (NOTE_WRITE) so both in-place
 * edits and editor-style atomic replaces are detected:
 *
 *   - a file event is always relevant;
 *   - kqueue cannot report which directory entry changed, so a directory
 *     event is confirmed by comparing the watched file's mtime/size
 *     against the baseline snapshot (shared with the poll backend);
 *   - after NOTE_DELETE/NOTE_RENAME the vnode watch is dead; the next
 *     wait re-arms on the new file, which also refreshes the baseline.
 *
 * When kqueue cannot be initialized (fd limit, restricted environments)
 * the backend transparently degrades to mtime/size polling (spec 4.5/
 * 16.3).
 */

#include "platform/platform.h"
#include "../posix/posix_watcher_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* O_DIRECTORY is present on modern macOS but harmless to define here. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

/**
 * @brief Register a vnode watch on the configured file (replaces any
 *        dead watch from a previous replace).
 * @param w  Watcher handle (fd = kqueue fd must be valid).
 * @return   0 on success, -1 when the file cannot be opened.
 */
static int kq_add_file(hpu_watcher_t* w)
{
    struct kevent ev;
    int vfd;

    if (w->watch_fd >= 0) {
        return 0; /* already armed */
    }
    vfd = open(w->path, O_EVTONLY | O_CLOEXEC);
    if (vfd < 0) {
        return -1; /* file absent; the directory watch keeps firing */
    }
    EV_SET(&ev, (uintptr_t)vfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE |
               NOTE_RENAME,
           0, NULL);
    if (kevent(w->fd, &ev, 1, NULL, 0, NULL) != 0) {
        (void)close(vfd);
        return -1;
    }
    w->watch_fd = vfd;
    w->vnode_dead = 0;
    return 0;
}

/**
 * @brief Try to establish the kqueue watches on the file and directory.
 * @param w     Watcher handle.
 * @param path  Config file path.
 * @return      0 on success, -1 when kqueue is unavailable.
 */
static int kq_setup(hpu_watcher_t* w, const char* path)
{
    char dir[HPULOGC_MAX_PATH_LEN];
    struct kevent ev;
    int dfd;

    w->fd = kqueue();
    if (w->fd < 0) {
        return -1;
    }
    (void)fcntl(w->fd, F_SETFD, FD_CLOEXEC);

    snprintf(w->path, sizeof(w->path), "%s", path);
    if (kq_add_file(w) != 0) {
        goto fail;
    }

    if (hpu_path_dirname(path, dir, sizeof(dir)) != 0) {
        goto fail;
    }
    dfd = open(dir, O_EVTONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        goto fail;
    }
    EV_SET(&ev, (uintptr_t)dfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE, 0, NULL);
    if (kevent(w->fd, &ev, 1, NULL, 0, NULL) != 0) {
        (void)close(dfd);
        goto fail;
    }
    w->dir_watch_fd = dfd;

    w->use_kqueue = 1;
    w->primed = 0;
    w->last_size = 0;
    w->last_mtime_ns = 0;
    (void)hpu_poll_snapshot(w); /* establish the stat baseline */
    return 0;

fail:
    if (w->watch_fd >= 0) {
        (void)close(w->watch_fd);
        w->watch_fd = -1;
    }
    (void)close(w->fd);
    w->fd = -1;
    return -1;
}

int hpu_watcher_start(hpu_watcher_t* w, const char* path)
{
    if (w == NULL || path == NULL) {
        return -1;
    }

    w->use_kqueue = 0;
    w->vnode_dead = 0;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
    w->fd = -1;
    if (kq_setup(w, path) == 0) {
        return 0;
    }
    /* kqueue unavailable (fd limit, sandbox): poll fallback */
    return hpu_poll_watcher_start(w, path);
}

int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    uint64_t deadline = hpu_now_ns() / 1000000ULL + timeout_ms;

    if (w == NULL) {
        return -1;
    }
    if (!w->use_kqueue) {
        return hpu_poll_watcher_wait(w, timeout_ms);
    }
    if (w->vnode_dead) {
        /* Re-arm on the new file after an atomic replace; failing to
         * reopen is not fatal (the file may be absent for a while). */
        (void)kq_add_file(w);
    }

    for (;;) {
        struct timespec ts;
        struct kevent ev;
        uint64_t now;
        uint64_t remain;
        int n;

        now = hpu_now_ns() / 1000000ULL;
        if (now >= deadline) {
            return 0;
        }
        remain = deadline - now;
        ts.tv_sec  = (time_t)(remain / 1000ULL);
        ts.tv_nsec = (long)(remain % 1000ULL) * 1000000L;

        n = kevent(w->fd, NULL, 0, &ev, 1, &ts);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return 0; /* timeout elapsed */
        }
        if (ev.filter != EVFILT_VNODE) {
            continue;
        }
        if ((uintptr_t)ev.ident == (uintptr_t)w->watch_fd) {
            if (ev.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                /* Atomic replace or removal: the vnode is gone. Report
                 * the change; the next wait re-arms on the new file. */
                (void)close(w->watch_fd);
                w->watch_fd = -1;
                w->vnode_dead = 1;
                return 1;
            }
            if (ev.fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB)) {
                (void)hpu_poll_snapshot(w); /* refresh the baseline */
                return 1;
            }
            continue;
        }
        if ((uintptr_t)ev.ident == (uintptr_t)w->dir_watch_fd) {
            /* Name-agnostic event: report only when the watched file
             * itself changed relative to the baseline (e.g. it was
             * re-created after a removal). */
            if (hpu_poll_snapshot(w) != 0) {
                return 1;
            }
            continue;
        }
    }
}

void hpu_watcher_stop(hpu_watcher_t* w)
{
    if (w == NULL) {
        return;
    }
    if (w->use_kqueue) {
        if (w->watch_fd >= 0) {
            (void)close(w->watch_fd);
        }
        if (w->dir_watch_fd >= 0) {
            (void)close(w->dir_watch_fd);
        }
        if (w->fd >= 0) {
            (void)close(w->fd);
        }
        w->use_kqueue = 0;
    } else {
        hpu_poll_watcher_stop(w);
    }
    w->fd = -1;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
    w->vnode_dead = 0;
}
