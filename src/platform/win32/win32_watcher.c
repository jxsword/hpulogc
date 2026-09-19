/* -*- coding: utf-8 -*- */

/**
 * @file win32_watcher.c
 * @brief Win32 watcher: always the polling backend (mtime + size), no
 *        inotify equivalent exists (spec 4.5/16.2).
 *
 * Semantics match the POSIX poll fallback: start takes a baseline
 * snapshot, wait returns 1 on a detected change (and refreshes the
 * baseline), 0 on timeout; a vanished watched file counts as a change
 * relative to an existing baseline.
 */

#include "platform/platform.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/**
 * @brief Snapshot the current mtime/size of the watched file.
 * @return 1 on a detected change, 0 otherwise.
 */
static int watcher_snapshot(hpu_watcher_t* w)
{
    wchar_t wpath[HPULOGC_MAX_PATH_LEN];
    WIN32_FILE_ATTRIBUTE_DATA info;
    uint64_t mtime_ns;
    int64_t size;

    if (w == NULL || w->path[0] == '\0') {
        return 0;
    }
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, w->path, -1, NULL, 0);

        if (n <= 0 || (size_t)n > HPULOGC_MAX_PATH_LEN) {
            return 0;
        }
        MultiByteToWideChar(CP_UTF8, 0, w->path, -1, wpath, n);
    }

    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &info)) {
        /* Missing file counts as a change relative to an existing
         * baseline; the reload path reports the parse error. */
        if (w->primed && w->last_size != -1) {
            w->last_size = -1;
            return 1;
        }
        w->last_size = -1;
        return 0;
    }

    mtime_ns = ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32 |
                (uint64_t)info.ftLastWriteTime.dwLowDateTime) *
               100ULL;
    size = (int64_t)(((uint64_t)info.nFileSizeHigh << 32) |
                     (uint64_t)info.nFileSizeLow);

    if (!w->primed) {
        w->last_mtime_ns = mtime_ns;
        w->last_size     = size;
        w->primed        = 1;
        return 0;
    }

    if (mtime_ns != w->last_mtime_ns || size != w->last_size) {
        w->last_mtime_ns = mtime_ns;
        w->last_size     = size;
        return 1;
    }
    return 0;
}

int hpu_watcher_start(hpu_watcher_t* w, const char* path)
{
    if (w == NULL || path == NULL) {
        return -1;
    }
    w->use_inotify  = 0;
    w->fd           = -1;
    w->watch_fd     = -1;
    w->dir_watch_fd = -1;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->primed       = 0;
    w->last_size    = 0;
    w->last_mtime_ns = 0;
    (void)watcher_snapshot(w); /* establish the baseline; missing is fine */
    return 0;
}

int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    uint32_t step = timeout_ms < 200U ? (timeout_ms == 0 ? 1U : timeout_ms)
                                      : 200U;

    for (;;) {
        int changed = watcher_snapshot(w);

        if (changed > 0) {
            return 1;
        }
        if (waited >= timeout_ms) {
            return 0;
        }
        if (waited + step > timeout_ms) {
            step = timeout_ms - waited;
        }
        Sleep(step);
        waited += step;
    }
}

void hpu_watcher_stop(hpu_watcher_t* w)
{
    if (w != NULL) {
        w->primed   = 0;
        w->fd       = -1;
        w->watch_fd = -1;
        w->dir_watch_fd = -1;
    }
}
