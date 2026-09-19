/**
 * @file hpu_sync.h
 * @brief Platform contract: mutex and condition variable primitives.
 *
 * Implemented per platform (posix/ today, win32/ in Phase 2). The locked
 * ring buffer embeds these by value so the ring stays single-file portable.
 */

#ifndef HPU_SYNC_H
#define HPU_SYNC_H

#include <stdint.h>

#if defined(HPU_PLATFORM_POSIX)
#include <pthread.h>

/**
 * @brief Recursive-safe-free plain mutex handle (pthread-backed).
 */
typedef struct hpu_mutex {
    pthread_mutex_t impl; /*!< Platform mutex storage */
} hpu_mutex_t;

/**
 * @brief Condition variable handle (pthread-backed).
 */
typedef struct hpu_cond {
    pthread_cond_t impl; /*!< Platform condition variable storage */
} hpu_cond_t;

#elif defined(HPU_PLATFORM_WIN32)
/* Phase 2: CRITICAL_SECTION or SRWLOCK + CONDITION_VARIABLE. Placeholder
 * only; win32 sources are not compiled in Phase 1. */
typedef struct hpu_mutex {
    void* impl; /*!< Phase 2: CRITICAL_SECTION storage */
} hpu_mutex_t;

typedef struct hpu_cond {
    void* impl; /*!< Phase 2: CONDITION_VARIABLE storage */
} hpu_cond_t;

#else
#error "hpulogc platform contract: unsupported platform (define HPU_PLATFORM_*)"
#endif

/**
 * @brief Initialize a mutex.
 * @param m  Mutex handle.
 * @return   0 on success, negative errno-style value on failure.
 */
int hpu_mutex_init(hpu_mutex_t* m);

/**
 * @brief Destroy a mutex previously initialized by hpu_mutex_init().
 * @param m  Mutex handle.
 */
void hpu_mutex_destroy(hpu_mutex_t* m);

/** @brief Lock the mutex (blocking). @param m Mutex handle. */
void hpu_mutex_lock(hpu_mutex_t* m);

/** @brief Unlock the mutex. @param m Mutex handle. */
void hpu_mutex_unlock(hpu_mutex_t* m);

/**
 * @brief Initialize a condition variable.
 * @param c  Condition handle.
 * @return   0 on success, negative errno-style value on failure.
 */
int hpu_cond_init(hpu_cond_t* c);

/**
 * @brief Destroy a condition variable.
 * @param c  Condition handle.
 */
void hpu_cond_destroy(hpu_cond_t* c);

/**
 * @brief Wait on the condition variable (atomically releases the mutex).
 * @param c  Condition handle.
 * @param m  Associated mutex, currently locked by the caller.
 * @return   0 on success, negative errno-style value on failure.
 */
int hpu_cond_wait(hpu_cond_t* c, hpu_mutex_t* m);

/**
 * @brief Wait on the condition variable with a timeout.
 * @param c        Condition handle.
 * @param m        Associated mutex, currently locked by the caller.
 * @param timeout_ms  Maximum wait in milliseconds (0 = immediate check).
 * @return   0 when signaled, 1 on timeout (HPU_ETIMEDOUT), negative on error.
 */
int hpu_cond_timedwait_ms(hpu_cond_t* c, hpu_mutex_t* m, uint32_t timeout_ms);

/** @brief Wake one waiter. @param c Condition handle. */
void hpu_cond_signal(hpu_cond_t* c);

/** @brief Wake all waiters. @param c Condition handle. */
void hpu_cond_broadcast(hpu_cond_t* c);

/** @brief Timedout result code for hpu_cond_timedwait_ms(). */
#define HPU_ETIMEDOUT 1

#endif /* HPU_SYNC_H */
