/**
 * @file darwin_platform.h
 * @brief macOS platform layer contract snapshot (Phase 3).
 *
 * macOS shares the POSIX layer (src/platform/posix/) with darwin-specific
 * replacements on top of the frozen contract (spec 3.3/16.3):
 *
 *   - hpu_sync:  darwin_sync.c - pthread_condattr_setclock() on macOS only
 *     accepts CLOCK_REALTIME, so timed waits use
 *     pthread_cond_timedwait_relative_np (clock-independent, like the
 *     MONOTONIC condattr on Linux).
 *   - hpu_tid:   darwin_tid.c - pthread_threadid_np (system-wide id;
 *     deprecated OSAtomic* is forbidden).
 *   - hpu_watcher: the kqueue (EVFILT_VNODE) backend per spec 16.3
 *     ("kqueue or polling fallback"), with the shared POSIX poll backend
 *     (mtime + size) as the transparent fallback (darwin_watcher.c).
 *   - hpu_time:  clock_gettime available since macOS 10.12; the POSIX
 *     layer applies directly.
 *   - hpu_fs:    POSIX semantics apply; fchmod/symlink fully supported.
 *
 * Directory contents: darwin_sync.c, darwin_tid.c, darwin_watcher.c
 * (kqueue backend) plus this snapshot header.
 */

#ifndef HPU_DARWIN_PLATFORM_H
#define HPU_DARWIN_PLATFORM_H

#include "../platform.h"

#endif /* HPU_DARWIN_PLATFORM_H */
