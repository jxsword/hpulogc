/**
 * @file posix_tid.c
 * @brief POSIX implementation of the thread id contract.
 *
 * Linux uses gettid(2) for the same numeric id as /proc; other POSIX
 * systems fall back to a cast of pthread_self().
 */

#include "platform/platform.h"

#include <pthread.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

/**
 * @brief Per-thread cached id: the syscall runs once per thread.
 */
static __thread uint64_t tid_cache;
static __thread int tid_valid;

uint64_t hpu_thread_id(void)
{
    if (!tid_valid) {
#if defined(__linux__)
        tid_cache = (uint64_t)(pid_t)syscall(SYS_gettid);
#else
        tid_cache = (uint64_t)(uintptr_t)pthread_self();
#endif
        tid_valid = 1;
    }
    return tid_cache;
}
