/**
 * @file bench_log.c
 * @brief Latency and throughput benchmark (spec 8).
 *
 * Built per combination via scripts/run_matrix.sh (lockfree/SPSC,
 * lockfree/MPSC, locked/MPSC). Scenarios:
 *   - latency: per-op CLOCK_MONOTONIC samples around hpulogc_log,
 *     P50/P99/P999 reported; barrier-synchronized start; warmup first.
 *   - throughput: fixed seconds, accepted/written counters from
 *     hpulogc_get_stats.
 *
 * Not part of the default CTest run (spec 13.3: manual/nightly).
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hpulogc.h"

/** @brief Per-call samples kept for the percentile computation. */
#define SAMPLES 200000
/** @brief Warmup logs before measuring. */
#define WARMUP 10000
/** @brief Message bytes (64B messages, spec 8). */
#define MSG_BYTES 64

/** @brief Producer argument. */
typedef struct bench_prod {
    pthread_barrier_t* barrier; /*!< Start barrier */
    unsigned long* samples;     /*!< Per-op latency samples (ns) */
    size_t sample_count;        /*!< Filled sample count */
    unsigned id;                /*!< Producer id */
} bench_prod_t;

/** @brief Monotonic ns via the platform vDSO path. */
static uint64_t bench_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Percentile of a sorted sample array.
 */
static unsigned long percentile(unsigned long* v, size_t n, double p)
{
    size_t idx = (size_t)(p * (double)n);

    if (idx >= n) {
        idx = n - 1;
    }
    return v[idx];
}

static int cmp_ulong(const void* a, const void* b)
{
    unsigned long va = *(const unsigned long*)a;
    unsigned long vb = *(const unsigned long*)b;

    return va < vb ? -1 : (va > vb ? 1 : 0);
}

/**
 * @brief Producer main: warmup, then per-op sampled logging.
 */
static void* latency_producer(void* raw)
{
    bench_prod_t* a = raw;
    char msg[128];
    unsigned long* local = malloc(SAMPLES * sizeof(unsigned long));
    size_t n = 0;
    int i;

    if (local == NULL) {
        return NULL;
    }
    memset(msg, 'x', MSG_BYTES);
    msg[MSG_BYTES - 1] = '\0';

    /* warmup */
    for (i = 0; i < WARMUP; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                    "%s", msg);
    }

    pthread_barrier_wait(a->barrier);

    for (i = 0; i < (int)SAMPLES; i++) {
        uint64_t t0 = bench_now_ns();

        hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                    "%s", msg);
        local[n++] = bench_now_ns() - t0;
    }

    a->samples = local;
    a->sample_count = n;
    return NULL;
}

/**
 * @brief Run one latency scenario.
 * @return 0 on success.
 */
static int run_latency(int producers)
{
    pthread_barrier_t barrier;
    bench_prod_t args[16];
    pthread_t threads[16];
    unsigned long* all = NULL;
    size_t total = 0;
    int i;

    if (producers > 16) {
        producers = 16;
    }
    pthread_barrier_init(&barrier, NULL, (unsigned)producers);

    for (i = 0; i < producers; i++) {
        args[i].barrier = &barrier;
        args[i].samples = NULL;
        args[i].sample_count = 0;
        args[i].id = (unsigned)i;
        if (producers == 1) {
            latency_producer(&args[i]);
        } else {
            pthread_create(&threads[i], NULL, latency_producer, &args[i]);
        }
    }
    if (producers > 1) {
        for (i = 0; i < producers; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    for (i = 0; i < producers; i++) {
        total += args[i].sample_count;
    }
    all = malloc(total * sizeof(unsigned long));
    if (all == NULL) {
        return -1;
    }
    total = 0;
    for (i = 0; i < producers; i++) {
        if (args[i].samples != NULL) {
            memcpy(all + total, args[i].samples,
                   args[i].sample_count * sizeof(unsigned long));
            total += args[i].sample_count;
            free(args[i].samples);
        }
    }
    qsort(all, total, sizeof(unsigned long), cmp_ulong);

    printf("  latency producers=%d samples=%zu P50=%lu ns P99=%lu ns "
           "P999=%lu ns\n",
           producers, total, percentile(all, total, 0.50),
           percentile(all, total, 0.99), percentile(all, total, 0.999));
    free(all);
    pthread_barrier_destroy(&barrier);
    return 0;
}

/**
 * @brief Amortized latency: one clock pair around a K-log loop (no
 *        per-op sampling overhead).
 */
static void run_amortized(void)
{
    char msg[128];
    uint64_t t0;
    uint64_t t1;
    int i;
    int k;
    const int outer = 20;
    const int inner = 10000;

    memset(msg, 'x', MSG_BYTES);
    msg[MSG_BYTES - 1] = '\0';

    for (i = 0; i < WARMUP; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                    "%s", msg);
    }
    t0 = bench_now_ns();
    for (i = 0; i < outer; i++) {
        for (k = 0; k < inner; k++) {
            hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                        "%s", msg);
        }
    }
    t1 = bench_now_ns();
    printf("  amortized logs=%d avg=%.0f ns\n", outer * inner,
           (double)(t1 - t0) / (double)(outer * inner));
}

/**
 * @brief Throughput scenario: fixed seconds, stats-based count.
 * @return 0 on success.
 */
static int run_throughput(int seconds)
{
    hpulogc_stats_t st;
    unsigned long long before;
    unsigned long long after;
    time_t end;
    int i = 0;
    char msg[128];

    memset(msg, 'x', MSG_BYTES);
    msg[MSG_BYTES - 1] = '\0';

    /* warmup */
    for (i = 0; i < WARMUP; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                    "%s", msg);
    }
    hpulogc_flush();
    if (hpulogc_get_stats(&st) != HPULOGC_OK) {
        fprintf(stderr, "bench: stats failed\n");
        return -1;
    }
    before = st.accepted;

    end = time(NULL) + seconds;
    i = 0;
    while (time(NULL) < end) {
        int k;

        for (k = 0; k < 1000; k++) {
            hpulogc_log(HPULOGC_LEVEL_INFO, "bench", "bench.c", 0, "prod",
                        "%s", msg);
            i++;
        }
    }
    /* drain */
    hpulogc_flush();
    {
        time_t drain_deadline = time(NULL) + 10;

        for (;;) {
            hpulogc_get_stats(&st);
            if (st.written + st.dropped + st.overwritten >=
                (unsigned long long)i + before) {
                break;
            }
            if (time(NULL) > drain_deadline) {
                break;
            }
            usleep(1000);
        }
    }
    after = st.accepted;
    printf("  throughput seconds=%d accepted=%llu -> %.0f logs/sec\n",
           seconds, after - before,
           (double)(after - before) / (double)seconds);
    return 0;
}

int main(int argc, char** argv)
{
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    hpulogc_rule_t rule;
    static const char* names[] = { "out0" };
    int mode_latency = 1;
    int seconds = 3;

    if (argc > 1 && strcmp(argv[1], "throughput") == 0) {
        mode_latency = 0;
    }
    if (!mode_latency && argc > 2) {
        seconds = atoi(argv[2]);
        if (seconds <= 0) {
            seconds = 3;
        }
    }

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = "/dev/null"; /* fast consumer; no disk influence */
    cfg.outputs = &out;
    cfg.output_count = 1;
    cfg.level = HPULOGC_LEVEL_TRACE;
    cfg.crash_safety = HPULOGC_CRASH_NONE;
    cfg.batch_size = 256;
    cfg.flush_interval_ms = 10;
    cfg.buffer_size = 1024 * 1024;
    cfg.default_format = "standard";
    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    if (hpulogc_init(&cfg) != HPULOGC_OK) {
        fprintf(stderr, "bench: init failed\n");
        return 1;
    }

    if (mode_latency) {
        printf("== latency (SPSC/single producer, 64B) ==\n");
        run_latency(1);
        printf("== latency (8 producers, 64B) ==\n");
        run_latency(8);
        printf("== amortized (single producer, no per-op sampling) ==\n");
        run_amortized();
    } else {
        printf("== throughput (64B, async batched) ==\n");
        run_throughput(seconds);
    }

    hpulogc_shutdown();
    return 0;
}
