/**
 * @file atomic_msvc.h
 * @brief Atomic backend: MSVC Interlocked* (Windows, all language
 *        standards; MinGW-w64 uses the same family via intrin.h).
 *
 * Not compiled on Phase 1 (Linux); kept complete so Phase 2 can enable it
 * without touching shared code (spec 4.3). Fine-grained memory orders are
 * mapped conservatively: anything stricter than relaxed uses the
 * sequentially consistent Interlocked* variants (Windows has no
 * fine-grained ordering in the classic Interlocked API; Interlocked*
 * on modern MSVC is already seq_cst).
 */

#ifndef HPULOGC_ATOMIC_MSVC_H
#define HPULOGC_ATOMIC_MSVC_H

#if !defined(_WIN32)
#  error "atomic_msvc.h is only for Windows targets"
#endif

#include <intrin.h>

/**
 * @brief Compiler-only read barrier (Phase 2 validation fix: _ReadBarrier
 *        is an MSVC intrinsic absent from MinGW-w64 GCC, which uses an
 *        empty asm clobber instead).
 */
#if defined(__GNUC__) || defined(__clang__)
#  define HPU_MSVC_READ_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#  define HPU_MSVC_READ_BARRIER() _ReadBarrier()
#endif

typedef struct {
    volatile long v; /*!< 32-bit storage (long matches Interlocked API) */
} hpu_atomic_u32;

typedef struct {
    volatile long long v; /*!< 64-bit storage */
} hpu_atomic_u64;

/**
 * @brief Map the unified order to a barrier kind (relaxed keeps raw ops).
 */
static __inline int hpu_msvc_volatile_order(hpu_memory_order mo)
{
    return mo == HPU_MO_RELAXED
               ? 0 /* Volatile semantics only */
               : 1 /* Full barrier (seq_cst) */;
}

/**
 * @brief Atomic load (32-bit).
 */
static __inline uint32_t hpu_at_load_u32(const hpu_atomic_u32* p,
                                         hpu_memory_order mo)
{
    if (hpu_msvc_volatile_order(mo)) {
        HPU_MSVC_READ_BARRIER();
        return (uint32_t)p->v;
    }
    return (uint32_t)p->v;
}

/**
 * @brief Atomic store (32-bit).
 */
static __inline void hpu_at_store_u32(hpu_atomic_u32* p, uint32_t val,
                                      hpu_memory_order mo)
{
    if (hpu_msvc_volatile_order(mo)) {
        _InterlockedExchange(&p->v, (long)val);
    } else {
        p->v = (long)val;
    }
}

/**
 * @brief Atomic fetch-and-add (32-bit).
 */
static __inline uint32_t hpu_at_fetch_add_u32(hpu_atomic_u32* p, uint32_t val,
                                              hpu_memory_order mo)
{
    (void)mo;
    return (uint32_t)_InterlockedExchangeAdd(&p->v, (long)val);
}

/**
 * @brief Atomic fetch-and-sub (32-bit).
 */
static __inline uint32_t hpu_at_fetch_sub_u32(hpu_atomic_u32* p, uint32_t val,
                                              hpu_memory_order mo)
{
    (void)mo;
    return (uint32_t)_InterlockedExchangeAdd(&p->v, -(long)val);
}

/**
 * @brief Atomic exchange (32-bit).
 */
static __inline uint32_t hpu_at_exchange_u32(hpu_atomic_u32* p, uint32_t val,
                                             hpu_memory_order mo)
{
    (void)mo;
    return (uint32_t)_InterlockedExchange(&p->v, (long)val);
}

/**
 * @brief Strong compare-and-swap (32-bit).
 */
static __inline int hpu_at_cas_u32(hpu_atomic_u32* p, uint32_t* expected,
                                   uint32_t desired, hpu_memory_order succ,
                                   hpu_memory_order fail)
{
    (void)succ;
    (void)fail;
    uint32_t old = (uint32_t)_InterlockedCompareExchange(
        &p->v, (long)desired, (long)*expected);
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
}

/**
 * @brief Atomic load (64-bit).
 *
 * On x64/ARM64 an aligned volatile load is atomic; 32-bit x86 needs the
 * compare-exchange read (Phase 2 validation fix).
 */
static __inline uint64_t hpu_at_load_u64(const hpu_atomic_u64* p,
                                         hpu_memory_order mo)
{
#if defined(_WIN64)
    if (hpu_msvc_volatile_order(mo)) {
        HPU_MSVC_READ_BARRIER();
        return (uint64_t)p->v;
    }
    return (uint64_t)p->v;
#else
    (void)mo;
    /* 32-bit targets: a plain 64-bit volatile read may tear. */
    return (uint64_t)_InterlockedCompareExchange64(&p->v, 0, 0);
#endif
}

/**
 * @brief Atomic store (64-bit).
 */
static __inline void hpu_at_store_u64(hpu_atomic_u64* p, uint64_t val,
                                      hpu_memory_order mo)
{
#if defined(_WIN64)
    if (hpu_msvc_volatile_order(mo)) {
        (void)_InterlockedExchange64(&p->v, (long long)val);
    } else {
        p->v = (long long)val;
    }
#else
    (void)mo;
    {
        /* 32-bit targets: CAS loop (a plain store may tear). */
        long long old = p->v;

        while (_InterlockedCompareExchange64(&p->v, (long long)val, old) !=
               old) {
            old = p->v;
        }
    }
#endif
}

/**
 * @brief Atomic fetch-and-add (64-bit).
 */
static __inline uint64_t hpu_at_fetch_add_u64(hpu_atomic_u64* p, uint64_t val,
                                              hpu_memory_order mo)
{
    (void)mo;
    return (uint64_t)_InterlockedExchangeAdd64(&p->v, (long long)val);
}

/**
 * @brief Atomic fetch-and-sub (64-bit).
 */
static __inline uint64_t hpu_at_fetch_sub_u64(hpu_atomic_u64* p, uint64_t val,
                                              hpu_memory_order mo)
{
    (void)mo;
    return (uint64_t)_InterlockedExchangeAdd64(&p->v, -(long long)val);
}

/**
 * @brief Atomic exchange (64-bit).
 */
static __inline uint64_t hpu_at_exchange_u64(hpu_atomic_u64* p, uint64_t val,
                                             hpu_memory_order mo)
{
    (void)mo;
    return (uint64_t)_InterlockedExchange64(&p->v, (long long)val);
}

/**
 * @brief Strong compare-and-swap (64-bit).
 */
static __inline int hpu_at_cas_u64(hpu_atomic_u64* p, uint64_t* expected,
                                   uint64_t desired, hpu_memory_order succ,
                                   hpu_memory_order fail)
{
    (void)succ;
    (void)fail;
    long long old = _InterlockedCompareExchange64(&p->v, (long long)desired,
                                                  (long long)*expected);
    if ((uint64_t)old == *expected) {
        return 1;
    }
    *expected = (uint64_t)old;
    return 0;
}

#endif /* HPULOGC_ATOMIC_MSVC_H */
