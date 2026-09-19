/**
 * @file posix_thread.c
 * @brief POSIX implementation of the thread contract (create/join/atfork).
 */

#include "platform/platform.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

/**
 * @brief Heap holder forwarding the contract entry signature to pthreads.
 */
typedef struct hpu_thread_start {
    hpu_thread_fn fn;  /*!< User entry point */
    void*         arg; /*!< User argument */
} hpu_thread_start_t;

static void* hpu_thread_trampoline(void* raw)
{
    hpu_thread_start_t* start = raw;

    start->fn(start->arg);
    free(start);
    return NULL;
}

int hpu_thread_create(hpu_thread_t* t, hpu_thread_fn fn, void* arg)
{
    hpu_thread_start_t* start;
    int rc;

    if (t == NULL || fn == NULL) {
        return -EINVAL;
    }

    start = malloc(sizeof(*start));
    if (start == NULL) {
        return -ENOMEM;
    }
    start->fn  = fn;
    start->arg = arg;

    rc = pthread_create(&t->impl, NULL, hpu_thread_trampoline, start);
    if (rc != 0) {
        free(start);
        t->started = 0;
        return -rc;
    }
    t->started = 1;
    return 0;
}

void hpu_thread_join(hpu_thread_t* t)
{
    if (t != NULL && t->started) {
        (void)pthread_join(t->impl, NULL);
        t->started = 0;
    }
}

int hpu_atfork_register(void (*child_fn)(void))
{
    /* Parent/prepare handlers are unnecessary: the child handler only
     * flips an atomic flag (spec 9). */
    return -pthread_atfork(NULL, NULL, child_fn);
}
