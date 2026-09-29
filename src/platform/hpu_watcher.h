/**
 * @file hpu_watcher.h
 * @brief Platform contract: configuration file change watcher.
 *
 * Linux uses inotify and macOS uses kqueue (EVFILT_VNODE); each falls
 * back to polling (mtime + size) when the native mechanism cannot be
 * initialized. The choice is made at start time by the platform
 * implementation. Windows prefers ReadDirectoryChangesW on the parent
 * directory and falls back to polling the same way (spec 4.5/16.2).
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
    int  use_kqueue;      /*!< Non-zero when the kqueue backend is active
                               (macOS EVFILT_VNODE; poll fallback when 0) */
    int  vnode_dead;      /*!< kqueue: watched vnode was deleted/renamed
                               away; re-armed on the next wait */
    int  dir_watch_fd;    /*!< Parent directory watch (rename-over detection) */
    uint64_t last_mtime_ns; /*!< Poll fallback: last modification time */
    int64_t  last_size;     /*!< Poll fallback: last file size */
    int  primed;          /*!< Poll fallback: baseline snapshot taken */
    void* dir_handle;     /*!< Win32 only: parent directory handle for
                               ReadDirectoryChangesW (NULL otherwise) */
    void* rdc_io;         /*!< Win32 only: backend-private OVERLAPPED and
                               notification buffer (NULL otherwise) */
    int  use_rdc;         /*!< Win32: non-zero when the
                               ReadDirectoryChangesW backend is active */
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
