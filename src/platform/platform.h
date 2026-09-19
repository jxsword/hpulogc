/**
 * @file platform.h
 * @brief Umbrella include for the frozen platform abstraction contract.
 *
 * The contract below is the Phase 1 porting surface for future win32/darwin
 * backends (spec 3.3/16.1). Sub-contracts:
 *   - hpu_sync.h    mutex/condvar primitives (used by the locked ring buffer)
 *   - hpu_thread.h  thread creation/join, atfork registration
 *   - hpu_time.h    high-resolution clocks and local time conversion
 *   - hpu_fs.h      file open/write/fsync/rename/unlink/dir scan, mkdir,
 *                   isatty, symlink
 *   - hpu_path.h    lexical path helpers (dirname/basename/stem/normalize)
 *   - hpu_watcher.h config-file change watcher (inotify with poll fallback)
 *   - hpu_tid.h     system thread id
 *   - hpu_tls.h     thread-local storage slots with destructor
 *   - hpu_signal.h  SIGHUP registration for hot reload triggering
 *
 * Platform selection macros (defined by CMake):
 *   HPU_PLATFORM_POSIX  POSIX shared layer (Linux and macOS)
 *   HPU_PLATFORM_LINUX  Linux-specific layer
 *   HPU_PLATFORM_DARWIN macOS-specific layer (Phase 3, placeholder)
 *   HPU_PLATFORM_WIN32  Windows-specific layer (Phase 2, placeholder)
 */

#ifndef HPU_PLATFORM_H
#define HPU_PLATFORM_H

/* Capacity macros (HPULOGC_MAX_PATH_LEN etc.) come from the public header
 * so there is a single source of truth. */
#include "../../include/hpulogc.h"

#include "hpu_sync.h"
#include "hpu_thread.h"
#include "hpu_time.h"
#include "hpu_fs.h"
#include "hpu_path.h"
#include "hpu_watcher.h"
#include "hpu_tid.h"
#include "hpu_tls.h"
#include "hpu_signal.h"

#endif /* HPU_PLATFORM_H */
