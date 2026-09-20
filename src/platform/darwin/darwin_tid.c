/* -*- coding: utf-8 -*- */

/**
 * @file darwin_tid.c
 * @brief Darwin implementation of the thread id contract.
 *
 * pthread_threadid_np(NULL, &tid) returns the system-wide thread id of
 * the calling thread — the numeric id the system reports elsewhere
 * (contract hpu_tid.h names it as the macOS equivalent of Linux
 * gettid(2); the POSIX shared layer's pthread_self() cast fallback is
 * only a pointer, not a system-wide id). Available since macOS 10.6.
 */

#include "platform/platform.h"

#include <pthread.h>

uint64_t hpu_thread_id(void)
{
    uint64_t tid = 0;

    /* Fails only on invalid arguments (thread must be NULL or valid);
     * 0 as the fallback value mirrors an impossible real id. */
    if (pthread_threadid_np(NULL, &tid) != 0) {
        return 0;
    }
    return tid;
}
