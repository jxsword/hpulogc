/**
 * @file posix_sync.c
 * @brief POSIX implementation of the mutex/condvar contract.
 *
 * Condition variables use CLOCK_MONOTONIC so timed waits survive wall
 * clock adjustments (NTP, manual changes).
 */

#include "platform/platform.h"

#include <errno.h>
#include <time.h>

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
    pthread_condattr_t attr;
    int rc;

    if (c == NULL) {
        return -EINVAL;
    }

    rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return -rc;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc != 0) {
        pthread_condattr_destroy(&attr);
        return -rc;
    }
    rc = pthread_cond_init(&c->impl, &attr);
    pthread_condattr_destroy(&attr);
    return -rc;
}

void hpu_cond_destroy(hpu_cond_t* c)
{
    if (c != NULL) {
        pthread_cond_destroy(&c->impl);
    }
}

int hpu_cond_wait(hpu_cond_t* c, hpu_mutex_t* m)
{
    return -pthread_cond_wait(&c->impl, &m->impl);
}

int hpu_cond_timedwait_ms(hpu_cond_t* c, hpu_mutex_t* m, uint32_t timeout_ms)
{
    struct timespec ts;
    uint64_t nsec;
    int rc;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    nsec = (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ULL;
    ts.tv_sec += (time_t)(nsec / 1000000000ULL);
    ts.tv_nsec = (long)(nsec % 1000000000ULL);

    rc = pthread_cond_timedwait(&c->impl, &m->impl, &ts);
    if (rc == ETIMEDOUT) {
        return HPU_ETIMEDOUT;
    }
    return -rc;
}

void hpu_cond_signal(hpu_cond_t* c)
{
    (void)pthread_cond_signal(&c->impl);
}

void hpu_cond_broadcast(hpu_cond_t* c)
{
    (void)pthread_cond_broadcast(&c->impl);
}
