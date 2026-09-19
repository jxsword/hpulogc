/* -*- coding: utf-8 -*- */

/**
 * @file portability.h
 * @brief Test-infrastructure portability shim (threads, barriers, paths).
 *
 * Phase 2 addition (allowed area: tests/). Centralizes everything the
 * Phase 1 tests took from POSIX so the same test sources run on Windows
 * (MSVC) and POSIX:
 *   - hpu_test_thread_create/join  (pthread vs _beginthreadex)
 *   - hpu_test_barrier             (pthread_barrier vs CONDITION_VARIABLE)
 *   - hpu_test_sleep_ms            (usleep vs Sleep)
 *   - hpu_test_getpid              (getpid vs _getpid)
 *   - hpu_test_tmpdir              (/tmp vs %TEMP%)
 *   - hpu_test_unlink / rmtree     (POSIX calls vs Win32 equivalents)
 *   - hpu_test_popen/pclose        (popen vs _popen)
 */

#ifndef HPU_TEST_PORTABILITY_H
#define HPU_TEST_PORTABILITY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>

/** @brief Thread handle (Windows). */
typedef struct hpu_test_thread {
    HANDLE handle; /*!< _beginthreadex handle */
} hpu_test_thread_t;

/**
 * @brief Thread entry trampoline signature (pthread shape: the shim wraps
 *        the __stdcall contract internally so test functions stay
 *        `void* fn(void*)` on every platform).
 */
typedef void* (*hpu_test_thread_fn)(void*);

/** @brief Barrier (Windows, CONDITION_VARIABLE based). */
typedef struct hpu_test_barrier {
    CRITICAL_SECTION   mu;     /*!< Count guard */
    CONDITION_VARIABLE cond;   /*!< Release signal */
    LONG               count;  /*!< Waiters arrived so far */
    LONG               total;  /*!< Parties required */
} hpu_test_barrier_t;

/** @brief Internal trampoline context. */
typedef struct hpu_test_thread_start {
    hpu_test_thread_fn fn;  /*!< User entry */
    void*              arg; /*!< User argument */
} hpu_test_thread_start_t;

/**
 * @brief __stdcall trampoline adapting to the pthread-style entry.
 */
static unsigned __stdcall hpu_test_thread_trampoline(void* raw)
{
    hpu_test_thread_start_t start = *(hpu_test_thread_start_t*)raw;

    free(raw);
    start.fn(start.arg);
    return 0;
}

/**
 * @brief Start a thread.
 * @return 0 on success, -1 on failure.
 */
static int hpu_test_thread_create(hpu_test_thread_t* t,
                                  hpu_test_thread_fn fn, void* arg)
{
    hpu_test_thread_start_t* start =
        malloc(sizeof(hpu_test_thread_start_t));
    uintptr_t h;

    if (start == NULL) {
        return -1;
    }
    start->fn  = fn;
    start->arg = arg;
    h = _beginthreadex(NULL, 0, hpu_test_thread_trampoline, start, 0, NULL);
    if (h == 0) {
        free(start);
        return -1;
    }
    t->handle = (HANDLE)h;
    return 0;
}

/**
 * @brief Join a thread and release its handle.
 */
static void hpu_test_thread_join(hpu_test_thread_t* t)
{
    if (t->handle != NULL) {
        (void)WaitForSingleObject(t->handle, INFINITE);
        (void)CloseHandle(t->handle);
        t->handle = NULL;
    }
}

/**
 * @brief Initialize a barrier for @p total parties.
 * @return 0 on success, -1 on failure.
 */
static int hpu_test_barrier_init(hpu_test_barrier_t* b, unsigned total)
{
    b->total = (LONG)total;
    b->count = 0;
    InitializeCriticalSection(&b->mu);
    InitializeConditionVariable(&b->cond);
    return 0;
}

/**
 * @brief Wait at the barrier until all parties arrived.
 */
static void hpu_test_barrier_wait(hpu_test_barrier_t* b)
{
    EnterCriticalSection(&b->mu);
    if (++b->count == b->total) {
        b->count = 0;
        WakeAllConditionVariable(&b->cond);
    } else {
        SleepConditionVariableCS(&b->cond, &b->mu, INFINITE);
    }
    LeaveCriticalSection(&b->mu);
}

/**
 * @brief Destroy a barrier.
 */
static void hpu_test_barrier_destroy(hpu_test_barrier_t* b)
{
    DeleteCriticalSection(&b->mu);
    (void)b;
}

/** @brief Millisecond sleep. */
static void hpu_test_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

/** @brief Process id. */
static int hpu_test_getpid(void)
{
    return _getpid();
}

/**
 * @brief Fill @p buf with a writable scratch directory.
 */
static void hpu_test_tmpdir(char* buf, size_t sz)
{
    const char* tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = ".";
    }
    snprintf(buf, sz, "%s", tmp);
}

/** @brief Remove a file (best effort). */
static void hpu_test_unlink(const char* path)
{
    (void)DeleteFileA(path);
}

/** @brief Remove an empty directory (best effort). */
static void hpu_test_rmdir(const char* path)
{
    (void)_rmdir(path);
}

/**
 * @brief Recursively remove a directory tree (best effort).
 */
static void hpu_test_rmtree(const char* path)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE find;

    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    find = FindFirstFileA(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        for (;;) {
            if (strcmp(fd.cFileName, ".") != 0 &&
                strcmp(fd.cFileName, "..") != 0) {
                char full[MAX_PATH];

                snprintf(full, sizeof(full), "%s\\%s", path, fd.cFileName);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    hpu_test_rmtree(full);
                } else {
                    (void)DeleteFileA(full);
                }
            }
            if (!FindNextFileA(find, &fd)) {
                break;
            }
        }
        FindClose(find);
    }
    _rmdir(path);
}

/** @brief errno helper used by hpu_test_mkdir (avoid EEXIST include). */
static int errno_exists(const char* path)
{
    DWORD attrs = GetFileAttributesA(path);

    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/** @brief Directory creation (single). */
static int hpu_test_mkdir(const char* path)
{
    return _mkdir(path) == 0 || errno_exists(path) ? 0 : -1;
}

/** @brief popen/pclose aliases. */
#define hpu_test_popen _popen
#define hpu_test_pclose _pclose

/** @brief Monotonic nanoseconds (QPC; benchmark timing). */
static unsigned long long hpu_test_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;

    if (freq.QuadPart == 0) {
        (void)QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&c);
    return (unsigned long long)((c.QuadPart * 1000000000LL) /
                                (freq.QuadPart ? freq.QuadPart : 1));
}

/** @brief Truncate a file to @p size bytes. */
static void hpu_test_truncate_file(const char* path, long size)
{
    FILE* fp = fopen(path, "r+b");

    if (fp != NULL) {
        _chsize(_fileno(fp), size);
        fclose(fp);
    }
}

#else /* POSIX */

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

/** @brief Thread handle (POSIX). */
typedef struct hpu_test_thread {
    pthread_t handle; /*!< pthread handle */
} hpu_test_thread_t;

/** @brief Barrier (POSIX). */
typedef struct hpu_test_barrier {
    pthread_barrier_t barrier; /*!< pthread barrier */
} hpu_test_barrier_t;

/** @brief Thread entry trampoline signature (pthread shape). */
typedef void* (*hpu_test_thread_fn)(void*);

/** @brief Start a thread. @return 0 on success, -1 on failure. */
static int hpu_test_thread_create(hpu_test_thread_t* t,
                                  hpu_test_thread_fn fn, void* arg)
{
    return pthread_create(&t->handle, NULL, fn, arg) == 0 ? 0 : -1;
}

/** @brief Join a thread. */
static void hpu_test_thread_join(hpu_test_thread_t* t)
{
    (void)pthread_join(t->handle, NULL);
}

/** @brief Initialize a barrier. @return 0 on success. */
static int hpu_test_barrier_init(hpu_test_barrier_t* b, unsigned total)
{
    return pthread_barrier_init(&b->barrier, NULL, total) == 0 ? 0 : -1;
}

/** @brief Wait at the barrier. */
static void hpu_test_barrier_wait(hpu_test_barrier_t* b)
{
    pthread_barrier_wait(&b->barrier);
}

/** @brief Destroy a barrier. */
static void hpu_test_barrier_destroy(hpu_test_barrier_t* b)
{
    pthread_barrier_destroy(&b->barrier);
    (void)b;
}

/** @brief Millisecond sleep. */
static void hpu_test_sleep_ms(unsigned ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000U;
    ts.tv_nsec = (long)(ms % 1000U) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        /* retry with the remaining time */
    }
}

/** @brief Process id. */
static int hpu_test_getpid(void)
{
    return (int)getpid();
}

/** @brief Fill @p buf with a writable scratch directory. */
static void hpu_test_tmpdir(char* buf, size_t sz)
{
    snprintf(buf, sz, "/tmp");
}

/** @brief Remove a file (best effort). */
static void hpu_test_unlink(const char* path)
{
    (void)unlink(path);
}

/** @brief Remove an empty directory (best effort). */
static void hpu_test_rmdir(const char* path)
{
    (void)rmdir(path);
}

/** @brief Recursively remove a directory tree (best effort). */
static void hpu_test_rmtree(const char* path)
{
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
    (void)system(cmd);
}

/** @brief Directory creation (single). */
static int hpu_test_mkdir(const char* path)
{
    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    return errno == EEXIST ? 0 : -1;
}

/** @brief popen/pclose aliases. */
#define hpu_test_popen popen
#define hpu_test_pclose pclose

/** @brief Monotonic nanoseconds (CLOCK_MONOTONIC; benchmark timing). */
static unsigned long long hpu_test_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL +
           (unsigned long long)ts.tv_nsec;
}

/** @brief Truncate a file to @p size bytes. */
static void hpu_test_truncate_file(const char* path, long size)
{
    FILE* fp = fopen(path, "r+b");

    if (fp != NULL) {
        (void)ftruncate(fileno(fp), (off_t)size);
        fclose(fp);
    }
}

#endif /* !_WIN32 */

#endif /* HPU_TEST_PORTABILITY_H */
