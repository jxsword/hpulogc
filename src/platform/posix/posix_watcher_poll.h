/**
 * @file posix_watcher_poll.h
 * @brief Internal header: shared poll-fallback watcher backend.
 *
 * Used directly by Linux (when inotify is unavailable) and by generic POSIX
 * builds (macOS Phase 3). Not part of the frozen contract; internal to the
 * platform layer.
 */

#ifndef HPU_POSIX_WATCHER_POLL_H
#define HPU_POSIX_WATCHER_POLL_H

#include "platform/platform.h"

/**
 * @brief Start the poll fallback watcher (mtime + size baseline).
 * @param w     Watcher handle.
 * @param path  Config file path.
 * @return      0 on success, -1 on failure.
 */
int hpu_poll_watcher_start(hpu_watcher_t* w, const char* path);

/**
 * @brief Poll for changes (see hpu_watcher_wait() for the contract).
 */
int hpu_poll_watcher_wait(hpu_watcher_t* w, uint32_t timeout_ms);

/**
 * @brief Stop the poll fallback watcher (releases nothing, clears state).
 */
void hpu_poll_watcher_stop(hpu_watcher_t* w);

#endif /* HPU_POSIX_WATCHER_POLL_H */
