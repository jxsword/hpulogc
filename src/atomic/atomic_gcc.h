/**
 * @file atomic_gcc.h
 * @brief Atomic backend: GCC/Clang builtins for C99 builds.
 *
 * Two variants live here (spec 3.3: "C99 __atomic_* (fallback __sync_*)"):
 *   - HPULOGC_ATOMIC_BACKEND_GCC       __atomic_* builtins (GCC >= 4.7),
 *                                      full memory-order support.
 *   - HPULOGC_ATOMIC_BACKEND_GCC_SYNC  legacy __sync_* builtins (full
 *                                      barriers, fine-grained orders are
 *                                      collapsed to seq_cst).
 */

#ifndef HPULOGC_ATOMIC_GCC_H
#define HPULOGC_ATOMIC_GCC_H

#include <stdint.h>

#if defined(HPULOGC_ATOMIC_BACKEND_GCC_SYNC)

typedef struct {
    volatile unsigned int v; /*!< 32-bit storage (accessed via __sync only) */
} hpu_atomic_u32;

typedef struct {
    volatile uint64_t v; /*!< 64-bit storage (accessed via __sync only) */
} hpu_atomic_u64;

/**
 * @brief Atomic seq_cst load (32-bit, __sync fallback).
 */
static inline uint32_t hpu_at_load_u32(const hpu_atomic_u32* p,
                                       hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_add(&((hpu_atomic_u32*)p)->v, 0U);
}

/**
 * @brief Atomic seq_cst store (32-bit, __sync fallback).
 */
static inline void hpu_at_store_u32(hpu_atomic_u32* p, uint32_t val,
                                    hpu_memory_order mo)
{
    (void)mo;
    (void)__sync_lock_test_and_set(&p->v, val);
    __sync_synchronize();
}

/**
 * @brief Atomic fetch-and-add (32-bit, __sync fallback).
 */
static inline uint32_t hpu_at_fetch_add_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_add(&p->v, val);
}

/**
 * @brief Atomic fetch-and-sub (32-bit, __sync fallback).
 */
static inline uint32_t hpu_at_fetch_sub_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_sub(&p->v, val);
}

/**
 * @brief Atomic exchange (32-bit, __sync fallback).
 */
static inline uint32_t hpu_at_exchange_u32(hpu_atomic_u32* p, uint32_t val,
                                           hpu_memory_order mo)
{
    (void)mo;
    return __sync_lock_test_and_set(&p->v, val);
}

/**
 * @brief Strong compare-and-swap (32-bit, __sync fallback).
 */
static inline int hpu_at_cas_u32(hpu_atomic_u32* p, uint32_t* expected,
                                 uint32_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    (void)succ;
    (void)fail;
    return __sync_bool_compare_and_swap(&p->v, *expected, desired);
}

/**
 * @brief Atomic seq_cst load (64-bit, __sync fallback).
 */
static inline uint64_t hpu_at_load_u64(const hpu_atomic_u64* p,
                                       hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_add(&((hpu_atomic_u64*)p)->v, 0ULL);
}

/**
 * @brief Atomic seq_cst store (64-bit, __sync fallback).
 */
static inline void hpu_at_store_u64(hpu_atomic_u64* p, uint64_t val,
                                    hpu_memory_order mo)
{
    (void)mo;
    (void)__sync_lock_test_and_set(&p->v, val);
    __sync_synchronize();
}

/**
 * @brief Atomic fetch-and-add (64-bit, __sync fallback).
 */
static inline uint64_t hpu_at_fetch_add_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_add(&p->v, val);
}

/**
 * @brief Atomic fetch-and-sub (64-bit, __sync fallback).
 */
static inline uint64_t hpu_at_fetch_sub_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    (void)mo;
    return __sync_fetch_and_sub(&p->v, val);
}

/**
 * @brief Atomic exchange (64-bit, __sync fallback).
 */
static inline uint64_t hpu_at_exchange_u64(hpu_atomic_u64* p, uint64_t val,
                                           hpu_memory_order mo)
{
    (void)mo;
    return __sync_lock_test_and_set(&p->v, val);
}

/**
 * @brief Strong compare-and-swap (64-bit, __sync fallback).
 */
static inline int hpu_at_cas_u64(hpu_atomic_u64* p, uint64_t* expected,
                                 uint64_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    (void)succ;
    (void)fail;
    return __sync_bool_compare_and_swap(&p->v, *expected, desired);
}

#else /* HPULOGC_ATOMIC_BACKEND_GCC: __atomic_* builtins */

typedef struct {
    unsigned int v; /*!< 32-bit storage; always accessed via __atomic */
} hpu_atomic_u32;

typedef struct {
    uint64_t v; /*!< 64-bit storage; always accessed via __atomic */
} hpu_atomic_u64;

/**
 * @brief Atomic load (32-bit).
 *
 * The hpu_memory_order enum values deliberately match the __ATOMIC_*
 * builtin numbering, so the cast is identity.
 */
static inline uint32_t hpu_at_load_u32(const hpu_atomic_u32* p,
                                       hpu_memory_order mo)
{
    return __atomic_load_n(&((hpu_atomic_u32*)p)->v, (int)mo);
}

/**
 * @brief Atomic store (32-bit).
 */
static inline void hpu_at_store_u32(hpu_atomic_u32* p, uint32_t val,
                                    hpu_memory_order mo)
{
    __atomic_store_n(&p->v, val, (int)mo);
}

/**
 * @brief Atomic fetch-and-add (32-bit).
 */
static inline uint32_t hpu_at_fetch_add_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    return __atomic_fetch_add(&p->v, val, (int)mo);
}

/**
 * @brief Atomic fetch-and-sub (32-bit).
 */
static inline uint32_t hpu_at_fetch_sub_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    return __atomic_fetch_sub(&p->v, val, (int)mo);
}

/**
 * @brief Atomic exchange (32-bit).
 */
static inline uint32_t hpu_at_exchange_u32(hpu_atomic_u32* p, uint32_t val,
                                           hpu_memory_order mo)
{
    return __atomic_exchange_n(&p->v, val, (int)mo);
}

/**
 * @brief Strong compare-and-swap (32-bit).
 */
static inline int hpu_at_cas_u32(hpu_atomic_u32* p, uint32_t* expected,
                                 uint32_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    return __atomic_compare_exchange_n(&p->v, expected, desired, 0,
                                       (int)succ, (int)fail);
}

/**
 * @brief Atomic load (64-bit).
 */
static inline uint64_t hpu_at_load_u64(const hpu_atomic_u64* p,
                                       hpu_memory_order mo)
{
    return __atomic_load_n(&((hpu_atomic_u64*)p)->v, (int)mo);
}

/**
 * @brief Atomic store (64-bit).
 */
static inline void hpu_at_store_u64(hpu_atomic_u64* p, uint64_t val,
                                    hpu_memory_order mo)
{
    __atomic_store_n(&p->v, val, (int)mo);
}

/**
 * @brief Atomic fetch-and-add (64-bit).
 */
static inline uint64_t hpu_at_fetch_add_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    return __atomic_fetch_add(&p->v, val, (int)mo);
}

/**
 * @brief Atomic fetch-and-sub (64-bit).
 */
static inline uint64_t hpu_at_fetch_sub_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    return __atomic_fetch_sub(&p->v, val, (int)mo);
}

/**
 * @brief Atomic exchange (64-bit).
 */
static inline uint64_t hpu_at_exchange_u64(hpu_atomic_u64* p, uint64_t val,
                                           hpu_memory_order mo)
{
    return __atomic_exchange_n(&p->v, val, (int)mo);
}

/**
 * @brief Strong compare-and-swap (64-bit).
 */
static inline int hpu_at_cas_u64(hpu_atomic_u64* p, uint64_t* expected,
                                 uint64_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    return __atomic_compare_exchange_n(&p->v, expected, desired, 0,
                                       (int)succ, (int)fail);
}

#endif /* backend variant */

#endif /* HPULOGC_ATOMIC_GCC_H */
