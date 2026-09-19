/**
 * @file hpu_watcher.h
 * @brief Platform contract: configuration file change watcher.
 *
 * Linux uses inotify when available and falls back to polling
 * (mtime + size) at the configured interval; the choice is made at start
 * time by the platform implementation. Windows/macOS will always poll
 * (Phase 2/3).
 */

#ifndef HPU_WATCHER_H
#define HPU_WATCHER_H

#include <stdint.h>

/**
 * @brief Watcher handle (opaque platform storage).
 */
typedef struct hpu_watcher {
    int  fd;              /*!< Platform watch handle (inotify fd, -1 when unused) */
    int  watch_fd;        /*!< Inner watch descriptor (inotify wd, -1 when unused) */
    char path[512];       /*!< Watched file path (poll fallback + dir watch) */
    int  use_inotify;     /*!< Non-zero when the inotify backend is active */
    int  dir_watch_fd;    /*!< Parent directory watch (rename-over detection) */
    uint64_t last_mtime_ns; /*!< Poll fallback: last modification time */
    int64_t  last_size;     /*!< Poll fallback: last file size */
    int  primed;          /*!< Poll fallback: baseline snapshot taken */
} hpu_watcher_t;

/**
 * @brief Start watching a configuration file for changes.
 *
 * Prefers the platform change-notification mechanism and silently falls
 * back to polling when unavailable. The initial state is a baseline: a
 * change reported afterwards.
 *
 * @param w     Watcher handle (zeroed by the caller).
 * @param path  Configuration file path (copied internally).
 * @return      0 on success, -1 on failure.
 */
int hpu_watcher_start(hpu_watcher_t* w, const char* path);

/**
 * @brief Wait until the watched file changes or the timeout elapses.
 *
 * @param w           Watcher handle started with hpu_watcher_start().
 * @param timeout_ms  Maximum wait in milliseconds.
 * @return            1 when a change was detected, 0 on timeout, -1 on
 *                    error.
 */
int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms);

/**
 * @brief Stop watching and release platform resources.
 * @param w  Watcher handle.
 */
void hpu_watcher_stop(hpu_watcher_t* w);

#endif /* HPU_WATCHER_H */
