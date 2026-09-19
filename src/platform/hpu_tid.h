/**
 * @file hpu_tid.h
 * @brief Platform contract: system-wide thread identifier.
 */

#ifndef HPU_TID_H
#define HPU_TID_H

#include <stdint.h>

/**
 * @brief Return the OS thread id of the calling thread.
 *
 * Linux: gettid(2); macOS: pthread_threadid_np; generic fallback: cast of
 * pthread_self()/GetCurrentThreadId(). The value is suitable for the %tid
 * placeholder.
 *
 * @return Numeric thread id.
 */
uint64_t hpu_thread_id(void);

#endif /* HPU_TID_H */
