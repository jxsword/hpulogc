/**
 * @file darwin_platform.h
 * @brief macOS platform layer contract snapshot (Phase 3, placeholder).
 *
 * NOT compiled in Phase 1. macOS shares the POSIX layer (src/platform/posix/)
 * and only needs darwin-specific bits on top of the frozen contract
 * (spec 3.3/16.3):
 *
 *   - hpu_tid:   pthread_threadid_np (deprecated OSAtomic* is forbidden).
 *   - hpu_watcher: kqueue(EVFILT_VNODE) or the polling fallback; the POSIX
 *     poll backend is the default fallback already.
 *   - hpu_time:  clock_gettime is available since macOS 10.12; the POSIX
 *     layer applies directly.
 *   - hpu_fs:    POSIX semantics apply; fchmod/symlink fully supported.
 *
 * In Phase 3 this directory provides darwin_tid.c and darwin_watcher.c
 * (plus kqueue backend if chosen) implementing the same prototypes.
 */

#ifndef HPU_DARWIN_PLATFORM_H
#define HPU_DARWIN_PLATFORM_H

#include "../platform.h"

#endif /* HPU_DARWIN_PLATFORM_H */
