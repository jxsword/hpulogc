/**
 * @file atomic_stdatomic.h
 * @brief Atomic backend: C11 <stdatomic.h> (Linux/macOS, C11 builds).
 *
 * Requires a C11 compiler with atomics (GCC >= 4.9, spec 2.1). Selected
 * with HPULOGC_ATOMIC_BACKEND_STDATOMIC.
 */

#ifndef HPULOGC_ATOMIC_STDATOMIC_H
#define HPULOGC_ATOMIC_STDATOMIC_H

#include <stdatomic.h>

typedef _Atomic uint32_t hpu_atomic_u32; /*!< 32-bit atomic storage */
typedef _Atomic uint64_t hpu_atomic_u64; /*!< 64-bit atomic storage */

/**
 * @brief Atomic load (32-bit).
 * @param p   Atomic object.
 * @param mo  Memory order.
 * @return    Current value.
 */
static inline uint32_t hpu_at_load_u32(const hpu_atomic_u32* p,
                                       hpu_memory_order mo)
{
    return atomic_load_explicit((const _Atomic uint32_t*)p,
                                (memory_order)mo);
}

/**
 * @brief Atomic store (32-bit).
 * @param p    Atomic object.
 * @param val  Value to store.
 * @param mo   Memory order.
 */
static inline void hpu_at_store_u32(hpu_atomic_u32* p, uint32_t val,
                                    hpu_memory_order mo)
{
    atomic_store_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic fetch-and-add (32-bit).
 * @return Value before the addition.
 */
static inline uint32_t hpu_at_fetch_add_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    return atomic_fetch_add_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic fetch-and-sub (32-bit).
 * @return Value before the subtraction.
 */
static inline uint32_t hpu_at_fetch_sub_u32(hpu_atomic_u32* p, uint32_t val,
                                            hpu_memory_order mo)
{
    return atomic_fetch_sub_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic exchange (32-bit).
 * @return Previous value.
 */
static inline uint32_t hpu_at_exchange_u32(hpu_atomic_u32* p, uint32_t val,
                                           hpu_memory_order mo)
{
    return atomic_exchange_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Strong compare-and-swap (32-bit).
 *
 * @param p        Atomic object.
 * @param expected In: expected value; out: observed value on failure.
 * @param desired  New value on success.
 * @param succ     Ordering on success.
 * @param fail     Ordering on failure.
 * @return         Non-zero when the swap happened.
 */
static inline int hpu_at_cas_u32(hpu_atomic_u32* p, uint32_t* expected,
                                 uint32_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    uint32_t e = *expected;
    int ok = atomic_compare_exchange_strong_explicit(
        p, &e, desired, (memory_order)succ, (memory_order)fail);
    *expected = e;
    return ok;
}

/**
 * @brief Atomic load (64-bit). @see hpu_at_load_u32()
 */
static inline uint64_t hpu_at_load_u64(const hpu_atomic_u64* p,
                                       hpu_memory_order mo)
{
    return atomic_load_explicit((const _Atomic uint64_t*)p,
                                (memory_order)mo);
}

/**
 * @brief Atomic store (64-bit). @see hpu_at_store_u32()
 */
static inline void hpu_at_store_u64(hpu_atomic_u64* p, uint64_t val,
                                    hpu_memory_order mo)
{
    atomic_store_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic fetch-and-add (64-bit). @see hpu_at_fetch_add_u32()
 */
static inline uint64_t hpu_at_fetch_add_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    return atomic_fetch_add_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic fetch-and-sub (64-bit). @see hpu_at_fetch_sub_u32()
 */
static inline uint64_t hpu_at_fetch_sub_u64(hpu_atomic_u64* p, uint64_t val,
                                            hpu_memory_order mo)
{
    return atomic_fetch_sub_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Atomic exchange (64-bit). @see hpu_at_exchange_u32()
 */
static inline uint64_t hpu_at_exchange_u64(hpu_atomic_u64* p, uint64_t val,
                                           hpu_memory_order mo)
{
    return atomic_exchange_explicit(p, val, (memory_order)mo);
}

/**
 * @brief Strong compare-and-swap (64-bit). @see hpu_at_cas_u32()
 */
static inline int hpu_at_cas_u64(hpu_atomic_u64* p, uint64_t* expected,
                                 uint64_t desired, hpu_memory_order succ,
                                 hpu_memory_order fail)
{
    uint64_t e = *expected;
    int ok = atomic_compare_exchange_strong_explicit(
        p, &e, desired, (memory_order)succ, (memory_order)fail);
    *expected = e;
    return ok;
}

#endif /* HPULOGC_ATOMIC_STDATOMIC_H */
