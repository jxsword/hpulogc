/**
 * @file posix_watcher.c
 * @brief Generic POSIX watcher: uses the poll fallback backend.
 *
 * Compiled for POSIX platforms without a native notification mechanism
 * (macOS Phase 3). On Linux, linux_watcher.c provides the public symbols
 * with an inotify backend instead and this file is excluded from the
 * build.
 */

#include "platform/platform.h"
#include "posix_watcher_poll.h"

int hpu_watcher_start(hpu_watcher_t* w, const char* path)
{
    if (w == NULL || path == NULL) {
        return -1;
    }
    return hpu_poll_watcher_start(w, path);
}

int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    if (w == NULL) {
        return -1;
    }
    return hpu_poll_watcher_wait(w, timeout_ms);
}

void hpu_watcher_stop(hpu_watcher_t* w)
{
    if (w != NULL) {
        hpu_poll_watcher_stop(w);
    }
}
