/* -*- coding: utf-8 -*- */

/**
 * @file win32_thread.c
 * @brief Win32 implementation of the thread contract.
 *
 * Threads are created with _beginthreadex (CRT-initialized, required for
 * CRT functions inside the thread). hpu_atfork_register() is a no-op:
 * Windows has no fork(2) (spec 9: the fork configuration is accepted but
 * ignored).
 */

#include "platform/platform.h"

#include <errno.h>
#include <process.h>
#include <stdlib.h>

/**
 * @brief Heap holder forwarding the contract entry signature to the CRT.
 */
typedef struct hpu_thread_start {
    hpu_thread_fn fn;  /*!< User entry point */
    void*         arg; /*!< User argument */
} hpu_thread_start_t;

/**
 * @brief _beginthreadex trampoline.
 */
static unsigned __stdcall hpu_thread_trampoline(void* raw)
{
    hpu_thread_start_t* start = raw;

    start->fn(start->arg);
    free(start);
    return 0;
}

int hpu_thread_create(hpu_thread_t* t, hpu_thread_fn fn, void* arg)
{
    hpu_thread_start_t* start;
    uintptr_t handle;

    if (t == NULL || fn == NULL) {
        return -EINVAL;
    }

    start = malloc(sizeof(*start));
    if (start == NULL) {
        return -ENOMEM;
    }
    start->fn  = fn;
    start->arg = arg;

    handle = _beginthreadex(NULL, 0, hpu_thread_trampoline, start, 0, NULL);
    if (handle == 0) {
        free(start);
        t->started = 0;
        return -ENOMEM;
    }
    t->impl    = (void*)handle;
    t->started = 1;
    return 0;
}

void hpu_thread_join(hpu_thread_t* t)
{
    if (t != NULL && t->started) {
        HANDLE h = (HANDLE)t->impl;

        (void)WaitForSingleObject(h, INFINITE);
        (void)CloseHandle(h);
        t->started = 0;
    }
}

int hpu_atfork_register(void (*child_fn)(void))
{
    /* No fork(2) on Windows: registration is accepted and ignored. */
    (void)child_fn;
    return 0;
}
