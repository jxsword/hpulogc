/* -*- coding: utf-8 -*- */

/**
 * @file bench_memory.c
 * @brief Baseline memory measurement for the min build (spec 8: < 32KB
 *        excluding the buffer).
 *
 * Method: resident/committed memory delta around hpulogc_init with the
 * minimal 4KB buffer; the untouched 4KB ring pages are not resident
 * until used, so the delta approximates the structure footprint. The
 * allocator is pre-touched first so allocator arena initialization is
 * not attributed to the library. Per-platform measurement sources:
 *   - Windows: WorkingSetSize (reference) + PrivateUsage (commit, the
 *     metric reported for Phase 2; mallinfo2 has no Windows equivalent).
 *   - macOS (Phase 3): mach task_info(MACH_TASK_BASIC_INFO) resident
 *     size; no /proc and no mallinfo2 on Darwin.
 *   - POSIX: /proc/self/statm (Phase 1; Linux only).
 */

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
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "hpulogc.h"

#if defined(_WIN32)
/**
 * @brief Resident set size in KB (WorkingSetSize).
 * @return RSS in KB, -1 on failure.
 */
static long resident_kb(void)
{
    PROCESS_MEMORY_COUNTERS pmc;

    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return -1;
    }
    return (long)(pmc.WorkingSetSize / 1024);
}

/**
 * @brief Private commit charge in KB (secondary reference; the EX
 *        counter block carries PrivateUsage).
 */
static long private_kb(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;

    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS*)&pmc,
                              sizeof(pmc))) {
        return -1;
    }
    return (long)(pmc.PrivateUsage / 1024);
}
#elif defined(__APPLE__)
/**
 * @brief Resident set size in KB (mach task resident_size).
 * @return RSS in KB, -1 on failure.
 */
static long resident_kb(void)
{
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }
    return (long)(info.resident_size / 1024);
}

static long private_kb(void)
{
    return -1; /* no private-commit equivalent exposed by Mach here */
}
#else
/**
 * @brief Resident set size in KB from /proc/self/statm (Phase 1 path).
 */
static long resident_kb(void)
{
    FILE* fp = fopen("/proc/self/statm", "r");
    long total = 0;
    long resident = 0;

    if (fp == NULL) {
        return -1;
    }
    if (fscanf(fp, "%ld %ld", &total, &resident) != 2) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return resident * 4; /* pages of 4KB */
}

static long private_kb(void)
{
    return -1; /* not measured on POSIX here */
}
#endif

int main(void)
{
    hpulogc_config_t cfg;
    long before;
    long after;
    long delta;
    long priv_before;
    long priv_after;
    long priv_delta;

    /* Pre-touch the allocator so its arena initialization is not
     * attributed to the library (freed block keeps the arena). */
    {
        void* pretouch = malloc(1024 * 1024);

        if (pretouch != NULL) {
            memset(pretouch, 1, 1024 * 1024);
            free(pretouch);
        }
    }

    before     = resident_kb();
    priv_before = private_kb();

    hpulogc_config_default(&cfg);
    cfg.buffer_size = 4 * 1024; /* minimal allowed buffer */
    if (hpulogc_init(&cfg) != HPULOGC_OK) {
        fprintf(stderr, "bench_memory: init failed\n");
        return 1;
    }

    after      = resident_kb();
    priv_after = private_kb();
    delta = after - before;
    priv_delta = priv_after - priv_before;

    hpulogc_log(HPULOGC_LEVEL_INFO, "mem", "bench.c", 0, "main", "touch");
    hpulogc_shutdown();

    printf("memory: before=%ld KB after=%ld KB delta=%ld KB "
           "(buffer=4KB not included: untouched pages)\n",
           before, after, delta);
    printf("private commit delta=%ld KB\n", priv_delta);
    printf("memory_baseline_kb=%ld\n", delta);
    return 0;
}
