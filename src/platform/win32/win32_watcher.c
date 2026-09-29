/* -*- coding: utf-8 -*- */

/**
 * @file win32_watcher.c
 * @brief Win32 watcher: ReadDirectoryChangesW backend with poll fallback
 *        (spec 4.5/16.2).
 *
 * The notification backend watches the parent directory of the config
 * file (ReadDirectoryChangesW cannot watch a single file) and filters
 * events by file name, mirroring the Linux inotify structure: directory
 * events filtered by basename cover editor-style atomic replaces, and a
 * relevant notification is confirmed with an mtime/size snapshot so a
 * baseline taken at start still holds. When the directory cannot be
 * opened or the backend breaks at runtime, the watcher silently degrades
 * to mtime/size polling (start baseline, wait 1/0/-1, vanished file
 * counts as a change).
 */

#include "platform/platform.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

/**
 * @brief Backend-private state for the ReadDirectoryChangesW backend.
 *
 * The OVERLAPPED and its buffer must stay valid while a change
 * notification is pending (across wait calls), so they live in one
 * heap block referenced by hpu_watcher_t::rdc_io.
 */
typedef struct rdc_context {
    OVERLAPPED ov;     /*!< Overlapped control block (hEvent is manual reset) */
    DWORD      buf_len; /*!< Bytes of valid notification data in buf */
    BYTE       buf[4096]; /*!< FILE_NOTIFY_INFORMATION receive buffer */
} rdc_context_t;

/**
 * @brief Extract the file name component of the watched path.
 *
 * Both separators are accepted because the path comes from user config.
 *
 * @param path  Watched path.
 * @return      Pointer inside @p path to the file name (never NULL).
 */
static const char* watcher_basename(const char* path)
{
    const char* slash  = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');

    if (bslash > slash) {
        slash = bslash;
    }
    return slash == NULL ? path : slash + 1;
}

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

/**
 * @brief Poll-fallback wait: mtime/size polling in <=200ms slices.
 * @return 1 on a detected change, 0 on timeout.
 */
static int poll_wait(hpu_watcher_t* w, uint32_t timeout_ms)
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

/**
 * @brief Arm one directory-change notification request.
 *
 * Only a single overlapped request may be outstanding, so wait re-arms
 * after every consumed notification.
 *
 * @param w  Watcher handle with an active RDC backend.
 * @return   0 on success, -1 when the request cannot be issued.
 */
static int rdc_rearm(hpu_watcher_t* w)
{
    HANDLE h = (HANDLE)w->dir_handle;
    rdc_context_t* ctx = (rdc_context_t*)w->rdc_io;
    DWORD bytes = 0;

    ResetEvent(ctx->ov.hEvent);
    ctx->ov.Offset     = 0;
    ctx->ov.OffsetHigh = 0;
    if (!ReadDirectoryChangesW(h, ctx->buf, (DWORD)sizeof(ctx->buf), FALSE,
                               FILE_NOTIFY_CHANGE_FILE_NAME |
                                   FILE_NOTIFY_CHANGE_LAST_WRITE |
                                   FILE_NOTIFY_CHANGE_SIZE,
                               &bytes, &ctx->ov, NULL)) {
        return -1;
    }
    return 0;
}

/**
 * @brief Classify pending directory notifications against the watched
 *        file name.
 *
 * Renames away and removals count as relevant: the following snapshot
 * reports the vanished file as a change, matching the inotify
 * IN_DELETE_SELF / IN_MOVE_SELF semantics.
 *
 * @param w      Watcher handle with an active RDC backend.
 * @param bytes  Valid byte count from GetOverlappedResult.
 * @return       1 when an event for the watched file arrived, 0 when
 *               only unrelated events arrived.
 */
static int rdc_classify(hpu_watcher_t* w, DWORD bytes)
{
    const rdc_context_t* ctx = (const rdc_context_t*)w->rdc_io;
    const char* base = watcher_basename(w->path);
    wchar_t name_w[256];
    DWORD name_len;
    size_t off = 0;

    if (bytes == 0) {
        /* Buffer overflowed: events were coalesced or dropped, assume
         * the watched file may have changed and let the snapshot decide. */
        return 1;
    }

    name_len = (DWORD)MultiByteToWideChar(CP_UTF8, 0, base, -1, NULL, 0);
    if (name_len == 0 || name_len > (DWORD)(sizeof(name_w) / sizeof(wchar_t))) {
        return 1; /* unfilterable: treat any event as relevant */
    }
    (void)MultiByteToWideChar(CP_UTF8, 0, base, -1, name_w, (int)name_len);
    name_len -= 1; /* exclude the NUL terminator for comparison */

    while (off + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= (size_t)bytes) {
        const FILE_NOTIFY_INFORMATION* info =
            (const FILE_NOTIFY_INFORMATION*)(const void*)(ctx->buf + off);
        DWORD chars = info->FileNameLength / (DWORD)sizeof(WCHAR);

        if ((info->Action == FILE_ACTION_MODIFIED ||
             info->Action == FILE_ACTION_REMOVED ||
             info->Action == FILE_ACTION_RENAMED_OLD_NAME ||
             info->Action == FILE_ACTION_RENAMED_NEW_NAME) &&
            chars == name_len && name_len > 0 &&
            lstrcmpiW(info->FileName, name_w) == 0) {
            return 1;
        }
        if (info->NextEntryOffset == 0) {
            break;
        }
        off += (size_t)info->NextEntryOffset;
    }
    return 0;
}

/**
 * @brief Release the notification backend and degrade to polling.
 *
 * Used both by stop and when the directory handle breaks at runtime
 * (directory removed, watch request rejected).
 *
 * @param w  Watcher handle (RDC fields cleared on return).
 */
static void rdc_teardown(hpu_watcher_t* w)
{
    HANDLE h = (HANDLE)w->dir_handle;
    rdc_context_t* ctx = (rdc_context_t*)w->rdc_io;

    if (h != NULL && h != INVALID_HANDLE_VALUE && ctx != NULL) {
        (void)CancelIoEx(h, &ctx->ov);
    }
    if (ctx != NULL) {
        if (ctx->ov.hEvent != NULL) {
            (void)CloseHandle(ctx->ov.hEvent);
        }
        (void)HeapFree(GetProcessHeap(), 0, ctx);
    }
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(h);
    }
    w->dir_handle = NULL;
    w->rdc_io     = NULL;
    w->use_rdc    = 0;
}

/**
 * @brief Try to establish the ReadDirectoryChangesW watch.
 *
 * @param w  Watcher handle with path and baseline already initialized.
 * @return   0 on success, -1 when the backend is unavailable (caller
 *           keeps the poll fallback).
 */
static int rdc_setup(hpu_watcher_t* w)
{
    char dir[HPULOGC_MAX_PATH_LEN];
    wchar_t wdir[HPULOGC_MAX_PATH_LEN];
    rdc_context_t* ctx;
    HANDLE h;
    int n;

    if (hpu_path_dirname(w->path, dir, sizeof(dir)) != 0) {
        return -1;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    if (n <= 0 || (size_t)n > HPULOGC_MAX_PATH_LEN) {
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, n);

    ctx = (rdc_context_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (SIZE_T)sizeof(rdc_context_t));
    if (ctx == NULL) {
        return -1;
    }
    ctx->ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ctx->ov.hEvent == NULL) {
        (void)HeapFree(GetProcessHeap(), 0, ctx);
        return -1;
    }
    h = CreateFileW(wdir, FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        (void)CloseHandle(ctx->ov.hEvent);
        (void)HeapFree(GetProcessHeap(), 0, ctx);
        return -1;
    }

    w->dir_handle = (void*)h;
    w->rdc_io     = (void*)ctx;
    w->use_rdc    = 1;
    if (rdc_rearm(w) != 0) {
        rdc_teardown(w);
        return -1;
    }
    return 0;
}

/**
 * @brief Notification wait on the directory handle.
 *
 * @param w  Watcher handle with an active RDC backend.
 * @return   1 on a detected change, 0 on timeout; degrades to
 *           poll_wait() when the backend breaks.
 */
static int rdc_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;

    for (;;) {
        ULONGLONG now = GetTickCount64();
        DWORD remain;
        DWORD rc;
        DWORD bytes = 0;
        int relevant;

        if (now >= deadline) {
            return 0;
        }
        remain = (DWORD)(deadline - now);

        rc = WaitForSingleObject(((rdc_context_t*)w->rdc_io)->ov.hEvent,
                                 remain);
        if (rc == WAIT_FAILED) {
            rdc_teardown(w);
            return poll_wait(w, timeout_ms);
        }
        if (rc == WAIT_TIMEOUT) {
            return 0;
        }

        if (!GetOverlappedResult((HANDLE)w->dir_handle,
                                 &((rdc_context_t*)w->rdc_io)->ov, &bytes,
                                 FALSE)) {
            rdc_teardown(w);
            return poll_wait(w, timeout_ms);
        }
        relevant = rdc_classify(w, bytes);
        if (rdc_rearm(w) != 0) {
            rdc_teardown(w);
            return poll_wait(w, timeout_ms);
        }
        if (relevant && watcher_snapshot(w) > 0) {
            return 1;
        }
        /* unrelated notification or content-identical rewrite: the loop
         * continues on the remaining time budget */
    }
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
    w->use_rdc      = 0;
    w->dir_handle   = NULL;
    w->rdc_io       = NULL;
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->primed       = 0;
    w->last_size    = 0;
    w->last_mtime_ns = 0;
    (void)watcher_snapshot(w); /* establish the baseline; missing is fine */
    (void)rdc_setup(w); /* poll fallback stays active on failure */
    return 0;
}

int hpu_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms)
{
    if (w == NULL) {
        return -1;
    }
    if (w->use_rdc) {
        return rdc_wait(w, timeout_ms);
    }
    return poll_wait(w, timeout_ms);
}

void hpu_watcher_stop(hpu_watcher_t* w)
{
    if (w != NULL) {
        if (w->use_rdc) {
            rdc_teardown(w);
        }
        w->primed   = 0;
        w->fd       = -1;
        w->watch_fd = -1;
        w->dir_watch_fd = -1;
        w->dir_handle   = NULL;
        w->rdc_io       = NULL;
        w->use_rdc      = 0;
    }
}
