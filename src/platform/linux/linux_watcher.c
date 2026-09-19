/**
 * @file linux_watcher.c
 * @brief Linux watcher: inotify backend with poll fallback.
 *
 * Watches both the file itself (IN_MODIFY/IN_CLOSE_WRITE/IN_DELETE_SELF)
 * and its parent directory (IN_MOVED_TO/IN_CREATE filtered by file name)
 * so editor-style atomic replaces are detected. When inotify cannot be
 * initialized (EMFILE, ENOSPC, restricted environments) the backend
 * transparently degrades to mtime/size polling (spec 4.5).
 */

#include "platform/platform.h"
#include "../posix/posix_watcher_poll.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/poll.h>
#include <unistd.h>

/**
 * @brief Extract the file name component of the watched path.
 * @param path  Watched path.
 * @return      Pointer inside @p path to the file name (never NULL).
 */
static const char* watcher_basename(const char* path)
{
    const char* slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

/**
 * @brief Try to establish inotify watches on the file and its directory.
 * @param w     Watcher handle.
 * @param path  Config file path.
 * @return      0 on success, -1 when inotify is unavailable.
 */
static int inotify_setup(hpu_watcher_t* w, const char* path)
{
    char dir[HPULOGC_MAX_PATH_LEN];
    uint32_t file_mask = IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB |
                         IN_DELETE_SELF | IN_MOVE_SELF;
    uint32_t dir_mask = IN_MOVED_TO | IN_CREATE | IN_CLOSE_WRITE;

    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0) {
        return -1;
    }

    w->watch_fd = inotify_add_watch(w->fd, path, file_mask);
    if (w->watch_fd < 0) {
        goto fail;
    }

    if (hpu_path_dirname(path, dir, sizeof(dir)) != 0) {
        goto fail;
    }
    w->dir_watch_fd = inotify_add_watch(w->fd, dir, dir_mask);
    if (w->dir_watch_fd < 0) {
        goto fail;
    }

    w->use_inotify = 1;
    return 0;

fail:
    close(w->fd);
    w->fd = -1;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
    return -1;
}

/**
 * @brief Drain and classify pending inotify events.
 * @param w  Watcher handle.
 * @return   1 when a change relevant to the watched file occurred, 0 when
 *           only unrelated events arrived.
 */
static int inotify_drain(hpu_watcher_t* w)
{
    char buf[4096];
    const char* name = watcher_basename(w->path);
    int relevant = 0;

    for (;;) {
        ssize_t n = read(w->fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }

        {
            ssize_t off = 0;
            while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
                const struct inotify_event* ev =
                    (const struct inotify_event*)(void*)(buf + off);

                if (ev->wd == w->watch_fd) {
                    relevant = 1; /* the file itself changed */
                } else if (ev->wd == w->dir_watch_fd && ev->len > 0) {
                    if (strcmp(ev->name, name) == 0) {
                        relevant = 1;
                    }
                }
                off += (ssize_t)(sizeof(struct inotify_event) + ev->len);
            }
        }
        break; /* one read usually drains; poll() will report more */
    }
    return relevant;
}

int hpu_watcher_start(hpu_watcher_t* w, const char* path)
{
    if (w == NULL || path == NULL) {
        return -1;
    }

    if (inotify_setup(w, path) == 0) {
        return 0;
    }
    /* inotify unavailable (fd limit, kernel restrictions): poll fallback */
    return hpu_poll_watcher_start(w, path);
}

int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    struct pollfd pfd;

    if (w == NULL) {
        return -1;
    }
    if (!w->use_inotify) {
        return hpu_poll_watcher_wait(w, timeout_ms);
    }

    pfd.fd = w->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        if (inotify_drain(w) > 0) {
            return 1;
        }
        /* only unrelated events: keep waiting for the remaining time */
        timeout_ms = 0;
    }
}

void hpu_watcher_stop(hpu_watcher_t* w)
{
    if (w == NULL) {
        return;
    }
    if (w->use_inotify) {
        if (w->watch_fd >= 0) {
            (void)inotify_rm_watch(w->fd, w->watch_fd);
        }
        if (w->dir_watch_fd >= 0) {
            (void)inotify_rm_watch(w->fd, w->dir_watch_fd);
        }
        if (w->fd >= 0) {
            (void)close(w->fd);
        }
        w->use_inotify = 0;
    } else {
        hpu_poll_watcher_stop(w);
    }
    w->fd = -1;
    w->watch_fd = -1;
    w->dir_watch_fd = -1;
}
