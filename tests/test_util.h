/**
 * @file test_util.h
 * @brief Minimal self-contained test framework for hpulogc tests.
 *
 * Each test binary registers cases with TEST(name) { ... } (self-registering
 * via constructor attributes) and calls hpu_test_run_all() from a main
 * provided by tests/test_main.c. Failures print file:line and increment a
 * process-wide counter; the process exits non-zero when anything failed.
 */

#ifndef HPU_TEST_UTIL_H
#define HPU_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Maximum test cases per binary. */
#define HPU_TEST_MAX 512

/** @brief Number of registered test cases. */
extern int hpu_test_count;
/** @brief Number of failed assertions overall. */
extern int hpu_test_failed;
/** @brief Name of the currently running case. */
extern const char* hpu_test_current;
/** @brief Set non-zero to see per-assertion output. */
extern int hpu_test_verbose;

/**
 * @brief Register a test case (used by the TEST macro; do not call directly).
 */
void hpu_test_register(void (*fn)(void), const char* name);

/**
 * @brief Run every registered case and print a summary.
 * @return Number of failed assertions (0 = all good).
 */
int hpu_test_run_all(void);

/**
 * @brief Internal failure reporter.
 */
void hpu_test_fail(const char* file, int line, const char* fmt, ...);

#define HPU_FAIL(...) \
    hpu_test_fail(__FILE__, __LINE__, __VA_ARGS__)

/** @brief Assert a boolean condition (returns from the case on failure). */
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            HPU_FAIL("check failed: %s", #cond);                        \
            return;                                                     \
        }                                                               \
    } while (0)

/** @brief Assert a boolean condition without returning. */
#define EXPECT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            HPU_FAIL("check failed: %s", #cond);                        \
        }                                                               \
    } while (0)

/** @brief Assert equality of two integer-ish values. */
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        long long hpu_va_ = (long long)(a);                             \
        long long hpu_vb_ = (long long)(b);                             \
        if (hpu_va_ != hpu_vb_) {                                       \
            HPU_FAIL("check failed: %s == %s (%lld != %lld)", #a, #b,   \
                     hpu_va_, hpu_vb_);                                 \
            return;                                                     \
        }                                                               \
    } while (0)

/** @brief Assert equality of two strings (NULL-safe). */
#define CHECK_STREQ(a, b)                                               \
    do {                                                                \
        const char* hpu_va_ = (a);                                      \
        const char* hpu_vb_ = (b);                                      \
        if ((hpu_va_ == NULL) != (hpu_vb_ == NULL) ||                   \
            (hpu_va_ != NULL && strcmp(hpu_va_, hpu_vb_) != 0)) {       \
            HPU_FAIL("check failed: %s == %s (\"%s\" vs \"%s\")", #a,   \
                     #b, hpu_va_ ? hpu_va_ : "(null)",                  \
                     hpu_vb_ ? hpu_vb_ : "(null)");                     \
            return;                                                     \
        }                                                               \
    } while (0)

/** @brief Assert two memory blocks of given length are equal. */
#define CHECK_MEMEQ(a, b, n)                                            \
    do {                                                                \
        if (memcmp((a), (b), (n)) != 0) {                               \
            HPU_FAIL("check failed: memcmp(%s, %s, %s)", #a, #b, #n);   \
            return;                                                     \
        }                                                               \
    } while (0)

/** @brief Unconditional failure (returns from the case). */
#define FAIL_MSG(...)                          \
    do {                                       \
        HPU_FAIL(__VA_ARGS__);                 \
        return;                                \
    } while (0)

/** @brief Define one test case with automatic registration.
 *
 * Phase 2: MSVC has no __attribute__((constructor)); registration uses
 * the CRT user-initializer section (.CRT$XCU) instead. The registration
 * pointer is a distinct symbol per case, which keeps the trick working
 * under link-time section GC in the test binaries.
 */
#if defined(_MSC_VER)

#define HPU_TEST_CONCAT_(a, b) a##b
#define HPU_TEST_CONCAT(a, b) HPU_TEST_CONCAT_(a, b)

#define TEST(name)                                                      \
    static void hpu_test_##name(void);                                  \
    static void hpu_test_reg_##name(void)                               \
    {                                                                   \
        hpu_test_register(hpu_test_##name, #name);                      \
    }                                                                   \
    __pragma(section(".CRT$XCU", long, read))                           \
    __declspec(allocate(".CRT$XCU"))                                    \
    static void (*const HPU_TEST_CONCAT(hpu_test_ptr_, name))(void) =   \
        hpu_test_reg_##name;                                            \
    static void hpu_test_##name(void)

#else /* GCC / Clang */

#define TEST(name)                                                      \
    static void hpu_test_##name(void);                                  \
    __attribute__((constructor)) static void hpu_test_auto_##name(void) \
    {                                                                   \
        hpu_test_register(hpu_test_##name, #name);                      \
    }                                                                   \
    static void hpu_test_##name(void)

#endif

#endif /* HPU_TEST_UTIL_H */
