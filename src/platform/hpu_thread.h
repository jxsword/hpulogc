/**
 * @file hpu_thread.h
 * @brief Platform contract: thread creation/join and fork registration.
 */

#ifndef HPU_THREAD_H
#define HPU_THREAD_H

#include <stdint.h>

#if defined(HPU_PLATFORM_POSIX)
#include <pthread.h>

/**
 * @brief Thread handle (pthread-backed).
 */
typedef struct hpu_thread {
    pthread_t impl;   /*!< Platform thread storage */
    int       started; /*!< Non-zero once the thread was successfully created */
} hpu_thread_t;

#elif defined(HPU_PLATFORM_WIN32)
/* Phase 2 placeholder: _beginthreadex/CreateThread handle storage. */
typedef struct hpu_thread {
    void* impl;    /*!< Phase 2: native thread handle */
    int   started; /*!< Non-zero once the thread was successfully created */
} hpu_thread_t;

#endif

/**
 * @brief Thread entry point signature.
 *
 * @param arg Opaque argument passed through hpu_thread_create().
 */
typedef void (*hpu_thread_fn)(void* arg);

/**
 * @brief Create and start a thread.
 *
 * @param t    Handle filled on success.
 * @param fn   Entry function, invoked with @p arg on the new thread.
 * @param arg  Argument forwarded to @p fn (may be NULL).
 * @return     0 on success, negative errno-style value on failure.
 */
int hpu_thread_create(hpu_thread_t* t, hpu_thread_fn fn, void* arg);

/**
 * @brief Join a thread created by hpu_thread_create().
 *
 * @param t  Handle of a successfully created thread; joining twice or
 *           joining a never-started thread is a caller error.
 */
void hpu_thread_join(hpu_thread_t* t);

/**
 * @brief Register a fork(2) child handler.
 *
 * The handler runs in the forked child immediately after fork() and must be
 * async-signal-safe (spec 9: it only sets an atomic dirty flag). On
 * platforms without fork this is a no-op returning 0.
 *
 * @param child_fn  Handler invoked in the child after fork.
 * @return          0 on success, negative value when registration failed.
 */
int hpu_atfork_register(void (*child_fn)(void));

#endif /* HPU_THREAD_H */
