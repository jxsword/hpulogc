/**
 * @file hpulogc_atomic.h
 * @brief Unified atomic operation interface with pluggable backends.
 *
 * Backend selection (spec 4.3/11), driven by CMake via exactly one of:
 *   - HPULOGC_ATOMIC_BACKEND_STDATOMIC  C11 <stdatomic.h> (C11 builds)
 *   - HPULOGC_ATOMIC_BACKEND_GCC        GCC/Clang __atomic_* builtins;
 *                                       define HPULOGC_ATOMIC_BACKEND_GCC_SYNC
 *                                       instead for the __sync_* fallback
 *   - HPULOGC_ATOMIC_BACKEND_MSVC       MSVC Interlocked* (Windows, any standard)
 *
 * With none defined, the header auto-detects. All backends expose the same
 * typed operations so tests can verify behavioral equivalence.
 */

#ifndef HPULOGC_ATOMIC_H
#define HPULOGC_ATOMIC_H

#include <stdint.h>

/**
 * @brief Memory ordering for atomic operations (mirrors C11 semantics).
 */
typedef enum {
    HPU_MO_RELAXED = 0, /*!< No ordering constraints */
    HPU_MO_CONSUME = 1, /*!< Consume ordering (treated as acquire) */
    HPU_MO_ACQUIRE = 2, /*!< Acquire ordering */
    HPU_MO_RELEASE = 3, /*!< Release ordering */
    HPU_MO_ACQ_REL = 4, /*!< Acquire+release ordering */
    HPU_MO_SEQ_CST = 5  /*!< Sequentially consistent */
} hpu_memory_order;

#if !defined(HPULOGC_ATOMIC_BACKEND_STDATOMIC) && \
    !defined(HPULOGC_ATOMIC_BACKEND_GCC) && \
    !defined(HPULOGC_ATOMIC_BACKEND_GCC_SYNC) && \
    !defined(HPULOGC_ATOMIC_BACKEND_MSVC)
/* Auto-detection (spec 4.3): C11 stdatomic first, then GCC/Clang builtins,
 * then MSVC. */
#  if defined(_MSC_VER)
#    define HPULOGC_ATOMIC_BACKEND_MSVC
#  elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
        !defined(__STDC_NO_ATOMICS__)
#    define HPULOGC_ATOMIC_BACKEND_STDATOMIC
#  elif defined(__GNUC__) || defined(__clang__)
#    define HPULOGC_ATOMIC_BACKEND_GCC
#  else
#    error "hpulogc atomic: no usable backend for this compiler"
#  endif
#endif

#if defined(HPULOGC_ATOMIC_BACKEND_STDATOMIC)
#  include "atomic_stdatomic.h"
#elif defined(HPULOGC_ATOMIC_BACKEND_GCC) || \
      defined(HPULOGC_ATOMIC_BACKEND_GCC_SYNC)
#  include "atomic_gcc.h"
#else
#  include "atomic_msvc.h"
#endif

/* ---- size_t-sized atomics (buffer byte counters etc.) ---- */

/**
 * @brief True when pointers are 64-bit (selects the usize backend width).
 */
#define HPU_USIZE_IS_64 (UINTPTR_MAX == UINT64_MAX)

#if HPU_USIZE_IS_64
typedef hpu_atomic_u64 hpu_atomic_size; /*!< Atomic size_t storage */
/** @brief Atomic load of a size_t-sized value. */
#define hpu_at_load_sz(p, mo)          hpu_at_load_u64((p), (mo))
/** @brief Atomic store of a size_t-sized value. */
#define hpu_at_store_sz(p, v, mo)      hpu_at_store_u64((p), (v), (mo))
/** @brief Atomic add of a size_t-sized value, returns the previous value. */
#define hpu_at_fetch_add_sz(p, v, mo)  hpu_at_fetch_add_u64((p), (v), (mo))
/** @brief Atomic subtract of a size_t-sized value, returns the previous value. */
#define hpu_at_fetch_sub_sz(p, v, mo)  hpu_at_fetch_sub_u64((p), (v), (mo))
#else
typedef hpu_atomic_u32 hpu_atomic_size; /*!< Atomic size_t storage */
/** @brief Atomic load of a size_t-sized value. */
#define hpu_at_load_sz(p, mo)          hpu_at_load_u32((p), (mo))
/** @brief Atomic store of a size_t-sized value. */
#define hpu_at_store_sz(p, v, mo)      hpu_at_store_u32((p), (v), (mo))
/** @brief Atomic add of a size_t-sized value, returns the previous value. */
#define hpu_at_fetch_add_sz(p, v, mo)  hpu_at_fetch_add_u32((p), (v), (mo))
/** @brief Atomic subtract of a size_t-sized value, returns the previous value. */
#define hpu_at_fetch_sub_sz(p, v, mo)  hpu_at_fetch_sub_u32((p), (v), (mo))
#endif

/**
 * @brief CPU relaxation hint for bounded spin loops.
 */
static inline void hpu_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    /* no-op on other architectures */
#endif
}

#endif /* HPULOGC_ATOMIC_H */
