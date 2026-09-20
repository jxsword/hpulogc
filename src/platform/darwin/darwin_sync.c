/* -*- coding: utf-8 -*- */

/**
 * @file darwin_sync.c
 * @brief Darwin implementation of the mutex/condvar contract.
 *
 * Mutex semantics are identical to the POSIX shared layer. The condition
 * variable differs: macOS pthread_condattr_setclock() only accepts
 * CLOCK_REALTIME, so the shared layer's MONOTONIC-deadline scheme cannot
 * be initialized there (hpu_cond_init would fail at runtime). This file
 * initializes the condvar with the default (REALTIME) clock and
 * implements timed waits through pthread_cond_timedwait_relative_np,
 * which measures the timeout without reference to any clock and is
 * therefore immune to wall-clock adjustments (NTP, manual changes) —
 * the same guarantee the MONOTONIC condattr provides on Linux.
 */

#include "platform/platform.h"

#include <errno.h>

int hpu_mutex_init(hpu_mutex_t* m)
{
    if (m == NULL) {
        return -EINVAL;
    }
    return -pthread_mutex_init(&m->impl, NULL);
}

void hpu_mutex_destroy(hpu_mutex_t* m)
{
    if (m != NULL) {
        pthread_mutex_destroy(&m->impl);
    }
}

void hpu_mutex_lock(hpu_mutex_t* m)
{
    if (m != NULL) {
        (void)pthread_mutex_lock(&m->impl);
    }
}

void hpu_mutex_unlock(hpu_mutex_t* m)
{
    if (m != NULL) {
        (void)pthread_mutex_unlock(&m->impl);
    }
}

int hpu_cond_init(hpu_cond_t* c)
{
    if (c == NULL) {
        return -EINVAL;
    }
    /* Default attribute: the condvar's clock stays CLOCK_REALTIME, which
     * is the only one macOS supports; timed waits never read it (see
     * hpu_cond_timedwait_ms). */
    return -pthread_cond_init(&c->impl, NULL);
}

void hpu_cond_destroy(hpu_cond_t* c)
{
    if (c != NULL) {
        (void)pthread_cond_destroy(&c->impl);
    }
}

int hpu_cond_wait(hpu_cond_t* c, hpu_mutex_t* m)
{
    if (c == NULL || m == NULL) {
        return -EINVAL;
    }
    return -pthread_cond_wait(&c->impl, &m->impl);
}

int hpu_cond_timedwait_ms(hpu_cond_t* c, hpu_mutex_t* m, uint32_t timeout_ms)
{
    struct timespec rel;
    int rc;

    if (c == NULL || m == NULL) {
        return -EINVAL;
    }
    rel.tv_sec  = (time_t)(timeout_ms / 1000U);
    rel.tv_nsec = (long)(timeout_ms % 1000U) * 1000000L;
    rc = pthread_cond_timedwait_relative_np(&c->impl, &m->impl, &rel);
    if (rc == ETIMEDOUT) {
        return HPU_ETIMEDOUT;
    }
    return -rc;
}

void hpu_cond_signal(hpu_cond_t* c)
{
    if (c != NULL) {
        (void)pthread_cond_signal(&c->impl);
    }
}

void hpu_cond_broadcast(hpu_cond_t* c)
{
    if (c != NULL) {
        (void)pthread_cond_broadcast(&c->impl);
    }
}
