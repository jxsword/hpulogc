/**
 * @file bench_memory.c
 * @brief Baseline memory measurement for the min build (spec 8: < 32KB
 *        excluding the buffer).
 *
 * Method (reported in docs/perf_report.md): /proc/self/statm resident
 * page difference around hpulogc_init with the minimal 4KB buffer; the
 * untouched 4KB ring pages are not resident until used, so the resident
 * delta approximates the structure footprint.
 */

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hpulogc.h"

/**
 * @brief Resident set size in KB from /proc/self/statm.
 */
static long statm_rss_kb(void)
{
    FILE* fp = fopen("/proc/self/statm", "r");
    long total = 0, resident = 0;

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

/** @brief Heap in-use snapshot taken before init (mallinfo2). */
static struct mallinfo2 hpu_mallinfo_before;

int main(void)
{
    hpulogc_config_t cfg;
    long before;
    long after;
    long delta;
    long heap_used_kb = 0;

    /* Pre-touch the allocator: glibc initializes its heap arena on the
     * first malloc, which would otherwise be attributed to the library.
     * The freed block keeps the arena; the measured delta then reflects
     * the library structures only. */
    {
        void* pretouch = malloc(1024 * 1024);

        memset(pretouch, 1, 1024 * 1024);
        free(pretouch);
    }

    hpu_mallinfo_before = mallinfo2();
    before = statm_rss_kb();

    hpulogc_config_default(&cfg);
    cfg.buffer_size = 4 * 1024; /* minimal allowed buffer */
    if (hpulogc_init(&cfg) != HPULOGC_OK) {
        fprintf(stderr, "bench_memory: init failed\n");
        return 1;
    }

    after = statm_rss_kb();
    delta = after - before;

    {
        struct mallinfo2 mi_before = hpu_mallinfo_before;
        struct mallinfo2 mi_after = mallinfo2();

        heap_used_kb = (mi_after.uordblks - mi_before.uordblks) / 1024;
    }

    hpulogc_log(HPULOGC_LEVEL_INFO, "mem", "bench.c", 0, "main", "touch");
    hpulogc_shutdown();

    printf("memory: before=%ld KB after=%ld KB delta=%ld KB "
           "(buffer=4KB not included: untouched pages)\n",
           before, after, delta);
    printf("heap in-use delta=%ld KB (mallinfo2 uordblks; library "
           "allocations only)\n", heap_used_kb);
    printf("memory_baseline_kb=%ld\n", delta);
    printf("memory_baseline_heap_kb=%ld\n", heap_used_kb);
    return 0;
}
