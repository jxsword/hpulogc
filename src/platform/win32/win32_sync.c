/* -*- coding: utf-8 -*- */

/**
 * @file win32_sync.c
 * @brief Win32 implementation of the mutex/condvar contract.
 *
 * CRITICAL_SECTION + CONDITION_VARIABLE. Timed waits use
 * SleepConditionVariableCS with millisecond granularity, matching the
 * contract's timedwait-ms signature directly.
 */

#include "platform/platform.h"

#include <errno.h>

int hpu_mutex_init(hpu_mutex_t* m)
{
    if (m == NULL) {
        return -EINVAL;
    }
    InitializeCriticalSection(&m->impl);
    return 0;
}

void hpu_mutex_destroy(hpu_mutex_t* m)
{
    if (m != NULL) {
        DeleteCriticalSection(&m->impl);
    }
}

void hpu_mutex_lock(hpu_mutex_t* m)
{
    if (m != NULL) {
        EnterCriticalSection(&m->impl);
    }
}

void hpu_mutex_unlock(hpu_mutex_t* m)
{
    if (m != NULL) {
        LeaveCriticalSection(&m->impl);
    }
}

int hpu_cond_init(hpu_cond_t* c)
{
    if (c == NULL) {
        return -EINVAL;
    }
    InitializeConditionVariable(&c->impl);
    return 0;
}

void hpu_cond_destroy(hpu_cond_t* c)
{
    /* CONDITION_VARIABLE needs no destruction; keep the signature. */
    (void)c;
}

int hpu_cond_wait(hpu_cond_t* c, hpu_mutex_t* m)
{
    if (c == NULL || m == NULL) {
        return -EINVAL;
    }
    /* Returns non-zero on wake; never times out with INFINITE. */
    if (SleepConditionVariableCS(&c->impl, &m->impl, INFINITE)) {
        return 0;
    }
    return -1;
}

int hpu_cond_timedwait_ms(hpu_cond_t* c, hpu_mutex_t* m, uint32_t timeout_ms)
{
    if (c == NULL || m == NULL) {
        return -EINVAL;
    }
    if (SleepConditionVariableCS(&c->impl, &m->impl, (DWORD)timeout_ms)) {
        return 0; /* signaled (or spurious wake, loop-compatible) */
    }
    return HPU_ETIMEDOUT;
}

void hpu_cond_signal(hpu_cond_t* c)
{
    if (c != NULL) {
        WakeConditionVariable(&c->impl);
    }
}

void hpu_cond_broadcast(hpu_cond_t* c)
{
    if (c != NULL) {
        WakeAllConditionVariable(&c->impl);
    }
}
