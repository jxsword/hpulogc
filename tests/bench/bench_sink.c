/**
 * @file bench_sink.c
 * @brief Sink-dispatch overhead benchmark (Phase 5, rd_v0.6 §13.3).
 *
 * Scenarios isolate different parts of the delivery pipeline (all with
 * TRACE level, batch 256, flush interval 10 ms, crash safety none):
 *   - null1     : one null sink            (render + route + deliver base)
 *   - null4     : four null sinks          (fan-out scaling)
 *   - null8     : eight null sinks         (fan-out scaling)
 *   - file      : one rollingfile          (sync buffered writes to disk)
 *   - file_async: one rollingfile, async=on (second-level queue + worker)
 *
 * Modes: `amortized` (default; one clock pair around N ops) and
 * `throughput [seconds]` (accepted delta from hpulogc_get_stats).
 * Not part of the default CTest run (spec 13.3: manual/perf reports).
 */

#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Amortized iteration count per scenario. */
#define BENCH_OPS 200000
/** @brief Warmup logs before measuring. */
#define WARMUP 10000
/** @brief Throughput measurement seconds. */
#define BENCH_SECONDS 3
/** @brief Message bytes (64B messages, spec 8). */
#define MSG_TEXT "x"

#define bench_now_ns() hpu_test_now_ns()

/**
 * @brief Build the sink declarations for a scenario.
 * @param sc       Scenario index (see header comment).
 * @param sinks    Output declaration array (caller-sized: >= 8).
 * @param keys     Output key scratch (scenario-owned static arrays).
 * @param vals     Output value scratch.
 * @param logpath  Log file path for file scenarios.
 * @param count    Filled with the number of sinks.
 * @return         0 on success, -1 on an unknown scenario.
 */
static int build_sinks(int sc, hpulogc_sink_decl_t* sinks,
                       const char** keys[8], const char** vals[8],
                       char (*keybuf)[32], char (*valbuf)[512],
                       const char* logpath, size_t* count)
{
    int i;

    switch (sc) {
    case 0: /* null1 */
        sinks[0].name = "s0";
        sinks[0].type = "null";
        sinks[0].keys = NULL;
        sinks[0].vals = NULL;
        sinks[0].count = 0;
        *count = 1;
        return 0;
    case 1: /* null4 */
    case 2: /* null8 */
        *count = (size_t)(sc == 1 ? 4 : 8);
        for (i = 0; i < (int)*count; i++) {
            snprintf(sinks[i].name, HPULOGC_MAX_NAME_LEN, "s%d", i);
            sinks[i].type = "null";
            sinks[i].keys = NULL;
            sinks[i].vals = NULL;
            sinks[i].count = 0;
        }
        return 0;
    case 3: /* file (sync) */
    case 4: /* file_async */
        snprintf(keybuf[0], 32, "path");
        snprintf(valbuf[0], 512, "%s", logpath);
        sinks[0].name = "s0";
        sinks[0].type = "rollingfile";
        sinks[0].keys = keys[0];
        sinks[0].vals = vals[0];
        sinks[0].count = 1;
        if (sc == 4) {
            snprintf(keybuf[1], 32, "async");
            snprintf(valbuf[1], 512, "on");
            snprintf(keybuf[2], 32, "queue size");
            snprintf(valbuf[2], 512, "4mb");
            sinks[0].keys = keys[0];
            sinks[0].vals = vals[0];
            sinks[0].count = 1;
        }
        *count = 1;
        return 0;
    default:
        return -1;
    }
}

/**
 * @brief Wire the common keys into the scenario's sink declarations.
 *
 * The async/queue-size keys for scenario 4 are appended into dedicated
 * scratch arrays (decls borrow the pointers until init returns).
 */
static int build_sinks_full(int sc, hpulogc_sink_decl_t* sinks,
                            const char* logpath, size_t* count)
{
    static char keybuf[8][32];
    static char valbuf[8][512];
    static const char* keys[8][4];
    static const char* vals[8][4];
    int i;
    int rc = build_sinks(sc, sinks, (const char***)keys,
                         (const char***)vals, keybuf, valbuf, logpath, count);

    for (i = 0; i < (int)*count; i++) {
        if (sc == 4 && i == 0) {
            keys[i][0] = keybuf[0];
            vals[i][0] = valbuf[0];
            keys[i][1] = keybuf[1];
            vals[i][1] = valbuf[1];
            keys[i][2] = keybuf[2];
            vals[i][2] = valbuf[2];
            sinks[i].keys = keys[i];
            sinks[i].vals = vals[i];
            sinks[i].count = 3;
        }
    }
    return rc;
}

/**
 * @brief Init the library for a scenario (sinks + default routing).
 */
static int scenario_init(int sc, hpulogc_sink_decl_t* sinks,
                         const char* logpath, size_t* count)
{
    hpulogc_config_t cfg;
    static const char* names[8];
    size_t i;
    int n;
    int rc = build_sinks_full(sc, sinks, logpath, count);

    if (rc != 0) {
        return rc;
    }
    n = (int)*count;
    for (i = 0; i < n; i++) {
        names[i] = sinks[i].name;
    }
    hpulogc_config_default(&cfg);
    cfg.level = HPULOGC_LEVEL_TRACE;
    cfg.buffer_size = 4U * 1024U * 1024U;
    cfg.batch_size = 256;
    cfg.flush_interval_ms = 10;
    cfg.crash_safety = HPULOGC_CRASH_NONE;
    cfg.sinks = sinks;
    cfg.sink_count = (size_t)n;
    cfg.default_outputs = names;
    cfg.default_output_count = (size_t)n;
    return hpulogc_init(&cfg);
}

/**
 * @brief Scenario display name.
 */
static const char* scenario_name(int sc)
{
    static const char* const names[] = {
        "null1", "null4", "null8", "file", "file_async"
    };
    return names[sc];
}

/**
 * @brief Amortized per-op cost for a scenario.
 */
static void run_amortized(int sc)
{
    hpulogc_sink_decl_t sinks[8];
    char logpath[256];
    size_t count = 0;
    unsigned long long t0;
    unsigned long long t1;
    int i;

    snprintf(logpath, sizeof(logpath), "build/bench_sink_s%d.log", sc);
    if (scenario_init(sc, sinks, logpath, &count) != 0) {
        printf("  %s: INIT FAILED\n", scenario_name(sc));
        return;
    }
    for (i = 0; i < WARMUP; i++) {
        HPULOGC_INFO("bench", "warmup %d", i);
    }
    t0 = bench_now_ns();
    for (i = 0; i < BENCH_OPS; i++) {
        HPULOGC_INFO("bench", "amortized message %d", i);
    }
    t1 = bench_now_ns();
    printf("  %s ops=%d avg=%.0f ns/op\n", scenario_name(sc), BENCH_OPS,
           (double)(t1 - t0) / (double)BENCH_OPS);
    (void)hpulogc_flush();
    hpulogc_shutdown();
    remove(logpath);
}

/**
 * @brief Throughput for a scenario (accepted delta over fixed seconds).
 */
static void run_throughput(int sc, int seconds)
{
    hpulogc_sink_decl_t sinks[8];
    char logpath[256];
    size_t count = 0;
    hpulogc_stats_t st;
    unsigned long long t0;
    unsigned long long accepted0;
    unsigned long long elapsed_ms = 0;
    int i;

    snprintf(logpath, sizeof(logpath), "build/bench_sink_s%d.log", sc);
    if (scenario_init(sc, sinks, logpath, &count) != 0) {
        printf("  %s: INIT FAILED\n", scenario_name(sc));
        return;
    }
    for (i = 0; i < WARMUP; i++) {
        HPULOGC_INFO("bench", "warmup %d", i);
    }
    (void)hpulogc_flush();
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    accepted0 = st.accepted;
    t0 = bench_now_ns();
    i = 0;
    for (;;) {
        HPULOGC_INFO("bench", "throughput message %d", i);
        i++;
        if ((i % 10000) == 0) {
            elapsed_ms = (bench_now_ns() - t0) / 1000000ULL;

            if (elapsed_ms >= (unsigned long long)seconds * 1000ULL) {
                break;
            }
        }
    }
    (void)hpulogc_flush();
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    printf("  %s seconds=%d accepted=%llu -> %.0f logs/sec\n",
           scenario_name(sc), seconds, st.accepted - accepted0,
           (double)(st.accepted - accepted0) /
               ((double)(bench_now_ns() - t0) / 1e9));
    hpulogc_shutdown();
    remove(logpath);
}

int main(int argc, char** argv)
{
    int sc;

    if (argc > 1 && strcmp(argv[1], "throughput") == 0) {
        int seconds = argc > 2 ? atoi(argv[2]) : BENCH_SECONDS;

        printf("== sink bench (throughput, 64B) ==\n");
        for (sc = 0; sc <= 4; sc++) {
            run_throughput(sc, seconds);
        }
    } else {
        printf("== sink bench (amortized, 64B) ==\n");
        for (sc = 0; sc <= 4; sc++) {
            run_amortized(sc);
        }
    }
    return 0;
}
