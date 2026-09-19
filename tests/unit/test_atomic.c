/**
 * @file test_atomic.c
 * @brief Behavioral equivalence tests for all atomic backends.
 *
 * The same source is compiled once per backend (stdatomic / gcc-atomic /
 * gcc-sync) by forcing the backend macro; all must pass identically.
 */

#include "test_util.h"
#include "atomic/hpulogc_atomic.h"

#include <pthread.h>

TEST(atomic_u32_basic)
{
    hpu_atomic_u32 v;

    hpu_at_store_u32(&v, 5, HPU_MO_RELAXED);
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_RELAXED), 5);
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_SEQ_CST), 5);
    CHECK_EQ(hpu_at_fetch_add_u32(&v, 3, HPU_MO_RELAXED), 5);
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_RELAXED), 8);
    CHECK_EQ(hpu_at_fetch_sub_u32(&v, 2, HPU_MO_SEQ_CST), 8);
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_RELAXED), 6);
    CHECK_EQ(hpu_at_exchange_u32(&v, 42, HPU_MO_SEQ_CST), 6);
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_RELAXED), 42);
}

TEST(atomic_u32_cas)
{
    hpu_atomic_u32 v;
    uint32_t expected;

    hpu_at_store_u32(&v, 10, HPU_MO_RELAXED);

    expected = 11;
    CHECK_EQ(hpu_at_cas_u32(&v, &expected, 20, HPU_MO_SEQ_CST,
                            HPU_MO_RELAXED), 0);
    CHECK_EQ(expected, 10); /* observed value reported on failure */

    expected = 10;
    CHECK(hpu_at_cas_u32(&v, &expected, 20, HPU_MO_SEQ_CST,
                         HPU_MO_RELAXED));
    CHECK_EQ(hpu_at_load_u32(&v, HPU_MO_RELAXED), 20);
}

TEST(atomic_u64_basic)
{
    hpu_atomic_u64 v;
    uint64_t expected;

    hpu_at_store_u64(&v, 0x0123456789ABCDEFull, HPU_MO_RELAXED);
    CHECK_EQ((long long)hpu_at_load_u64(&v, HPU_MO_SEQ_CST),
             (long long)0x0123456789ABCDEFull);
    CHECK_EQ((long long)hpu_at_fetch_add_u64(&v, 1, HPU_MO_SEQ_CST),
             (long long)0x0123456789ABCDEFull);
    CHECK_EQ((long long)hpu_at_load_u64(&v, HPU_MO_RELAXED),
             (long long)0x0123456789ABCDF0ull);
    CHECK_EQ((long long)hpu_at_exchange_u64(&v, 7, HPU_MO_SEQ_CST),
             (long long)0x0123456789ABCDF0ull);

    expected = 7;
    CHECK(hpu_at_cas_u64(&v, &expected, 9, HPU_MO_SEQ_CST, HPU_MO_RELAXED));
    CHECK_EQ(hpu_at_load_u64(&v, HPU_MO_RELAXED), 9);
}

TEST(atomic_size_alias)
{
    hpu_atomic_size v;

    hpu_at_store_sz(&v, 100, HPU_MO_RELAXED);
    CHECK_EQ((long long)hpu_at_load_sz(&v, HPU_MO_SEQ_CST), 100);
    CHECK_EQ((long long)hpu_at_fetch_add_sz(&v, 23, HPU_MO_SEQ_CST), 100);
    CHECK_EQ((long long)hpu_at_fetch_sub_sz(&v, 123, HPU_MO_SEQ_CST), 123);
    CHECK_EQ((long long)hpu_at_load_sz(&v, HPU_MO_RELAXED), 0);
}

/** @brief Shared contention target for the threaded fetch_add test. */
static hpu_atomic_u32 g_contended_u32;
/** @brief Shared contention target (64-bit). */
static hpu_atomic_u64 g_contended_u64;
/** @brief Per-thread iteration count for the contention tests. */
#define CONTEND_ITERS 20000
/** @brief Number of contending threads. */
#define CONTEND_THREADS 4

static void* contend_worker(void* raw)
{
    unsigned id = (unsigned)(uintptr_t)raw;
    int i;

    (void)id;
    for (i = 0; i < CONTEND_ITERS; i++) {
        hpu_at_fetch_add_u32(&g_contended_u32, 1, HPU_MO_SEQ_CST);
        hpu_at_fetch_add_u64(&g_contended_u64, 2, HPU_MO_SEQ_CST);
    }
    return NULL;
}

TEST(atomic_contended_add)
{
    pthread_t threads[CONTEND_THREADS];
    unsigned i;

    hpu_at_store_u32(&g_contended_u32, 0, HPU_MO_RELAXED);
    hpu_at_store_u64(&g_contended_u64, 0, HPU_MO_RELAXED);

    for (i = 0; i < CONTEND_THREADS; i++) {
        CHECK_EQ(pthread_create(&threads[i], NULL, contend_worker,
                                (void*)(uintptr_t)i), 0);
    }
    for (i = 0; i < CONTEND_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    CHECK_EQ(hpu_at_load_u32(&g_contended_u32, HPU_MO_SEQ_CST),
             CONTEND_THREADS * CONTEND_ITERS);
    CHECK_EQ(hpu_at_load_u64(&g_contended_u64, HPU_MO_SEQ_CST),
             (long long)CONTEND_THREADS * CONTEND_ITERS * 2);
}

/**
 * @brief CAS-based flag spin: exercises acquire/release pairing.
 */
static hpu_atomic_u32 g_cas_flag;

static void* cas_flag_worker(void* raw)
{
    unsigned id = (unsigned)(uintptr_t)raw;
    int acquired = 0;
    int i;

    (void)id;
    for (i = 0; i < 2000; i++) {
        uint32_t expected = 0;

        if (hpu_at_cas_u32(&g_cas_flag, &expected, 1, HPU_MO_ACQ_REL,
                           HPU_MO_ACQUIRE)) {
            acquired++;
            hpu_at_store_u32(&g_cas_flag, 0, HPU_MO_RELEASE);
        }
    }
    return (void*)(uintptr_t)acquired;
}

TEST(atomic_cas_flag_handoff)
{
    pthread_t threads[CONTEND_THREADS];
    unsigned i;
    long total = 0;

    hpu_at_store_u32(&g_cas_flag, 0, HPU_MO_RELAXED);
    for (i = 0; i < CONTEND_THREADS; i++) {
        CHECK_EQ(pthread_create(&threads[i], NULL, cas_flag_worker,
                                (void*)(uintptr_t)i), 0);
    }
    for (i = 0; i < CONTEND_THREADS; i++) {
        void* ret = NULL;

        pthread_join(threads[i], &ret);
        total += (long)(uintptr_t)ret;
    }
    /* every acquisition eventually released: total is a positive multiple
     * of nothing in particular, but every acquire paired with a store 0 */
    CHECK(total > 0);
    CHECK_EQ(hpu_at_load_u32(&g_cas_flag, HPU_MO_SEQ_CST), 0);
}
