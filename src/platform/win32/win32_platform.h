/**
 * @file win32_platform.h
 * @brief Windows platform layer contract snapshot (Phase 2, placeholder).
 *
 * NOT compiled in Phase 1. This header mirrors the frozen contract from
 * src/platform/platform.h and documents the planned win32 implementation
 * so Phase 2 can proceed without touching shared code (spec 3.3/16.2):
 *
 *   - hpu_sync:      SRWLOCK or CRITICAL_SECTION mutex, CONDITION_VARIABLE;
 *                    timed wait via SleepConditionVariableCS.
 *   - hpu_thread:    _beginthreadex handles; hpu_atfork_register() is a
 *                    no-op (no fork on Windows).
 *   - hpu_time:      QueryPerformanceCounter (monotonic),
 *                    GetSystemTimePreciseAsFileTime (realtime),
 *                    localtime_s / _mkgmtime.
 *   - hpu_fs:        CreateFile + OVERLAPPED-less WriteFile with a
 *                    write-all loop, FlushFileBuffers (= fsync), renamed
 *                    via MoveFileEx, cleanup via FindFirstFile/FindNextFile
 *                    (dir scan semantics), permissions ignored with a
 *                    warning (spec 4.7), no symlink without privilege
 *                    (symlink_latest ignored + warning).
 *   - hpu_path:      backslash separators, UTF-8 <-> UTF-16 conversion
 *                    (WideCharToMultiByte) around every file API.
 *   - hpu_watcher:   always the polling backend (mtime + size at
 *                    hot reload interval); no inotify.
 *   - hpu_tid:       GetCurrentThreadId().
 *   - hpu_tls:       TlsAlloc/TlsGetValue/TlsSetValue/TlsFree.
 *   - hpu_signal:    no SIGHUP; hpu_signal_install_hup() returns -1.
 *
 * In Phase 2 this directory additionally provides win32_sync.c,
 * win32_thread.c, win32_time.c, win32_fs.c, win32_path.c, win32_watcher.c,
 * win32_tid.c, win32_tls.c implementing the same prototypes.
 */

#ifndef HPU_WIN32_PLATFORM_H
#define HPU_WIN32_PLATFORM_H

#include "../platform.h"

#endif /* HPU_WIN32_PLATFORM_H */
