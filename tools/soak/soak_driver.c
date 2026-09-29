/* -*- coding: utf-8 -*- */

/**
 * @file soak_driver.c
 * @brief 24h high-concurrency soak test driver (spec rd_v0.6 §13.3,
 *        decision D-R6).
 *
 * Load shape (decision D-R6, non-negotiable parts from todo.md):
 *   - MPMC build, 4 producer threads, 4 consumer threads (config).
 *   - Sink fan-out: rollingfile (size rotation) + null (per-sink async
 *     queue, exercises the second-level queue and its drop accounting)
 *     + console (stderr, redirected to a file by the harness script).
 *   - Periodic config-file rewrites (level flip INFO<->DEBUG, queue-size
 *     toggle) trigger hot reload via the watcher while producers run.
 *   - Periodic hpulogc_flush() from the maintenance thread.
 *
 * Pass criteria (sampled continuously, spec §13.3):
 *   - No deadlock: the global `written` counter strictly increases between
 *     samples (stalls counted; any stall fails the run).
 *   - No accounting violation: written + dropped + overwritten + throttled
 *     <= accepted at every sample, and full equality after the final
 *     flush/sync drain.
 *   - No leak: RSS trend reported per sample; the harness report compares
 *     first/last quarter averages (driver only records it).
 *
 * Exit codes: 0 = pass, 2 = heartbeat stall, 3 = accounting violation,
 * 4 = usage/IO error.
 */

#include "hpulogc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** @brief Seconds between stats samples (heartbeat cadence). */
#define SOAK_SAMPLE_INTERVAL_S 5
/** @brief Seconds between config rewrites (hot reload trigger). */
#define SOAK_RELOAD_INTERVAL_S 60

/** @brief glibc printf lacks %llu portability guarantees we need; cast to
 *         unsigned long long and use %llu (C99 printf is fine). */

static volatile sig_atomic_t g_stop = 0;   /*!< Stop flag for producers */
static unsigned long long g_stalls = 0;    /*!< Heartbeat stall count */
static unsigned long long g_violations = 0; /*!< Accounting violations */

/**
 * @brief Monotonic seconds since an arbitrary epoch.
 */
static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/**
 * @brief Current resident set size in KiB (Linux; -1 when unavailable).
 */
static long rss_kib(void)
{
    FILE* fp;
    long pages;
    long total;

    fp = fopen("/proc/self/statm", "r");
    if (fp == NULL) {
        return -1;
    }
    if (fscanf(fp, "%*s %ld", &pages) != 1) {
        pages = -1;
    }
    fclose(fp);
    if (pages < 0) {
        return -1;
    }
    total = pages * (long)sysconf(_SC_PAGESIZE) / 1024;
    return total;
}

/**
 * @brief Thread payload: producer index.
 */
struct producer_ctx {
    int idx;                       /*!< Producer index (name suffix) */
    long rate_us;                  /*!< Base inter-record sleep in us */
    unsigned long long count;      /*!< Records submitted by this thread */
};

/**
 * @brief Producer: mixed-level/category/field log stream.
 */
static void* producer_main(void* raw)
{
    struct producer_ctx* ctx = raw;
    char cat[64];
    hpulogc_field_t fields[2];
    char valbuf[2][32];
    unsigned seed = (unsigned)(ctx->idx * 7919u + 13u);

    fields[0].key = "iter";
    fields[1].key = "producer";
    fields[0].value.type = HPULOGC_FIELD_U64;
    fields[1].value.type = HPULOGC_FIELD_STR;

    while (!g_stop) {
        unsigned long long i = ctx->count;
        int r = (int)(rand_r(&seed) & 63u);

        fields[0].value.v.u64 = i;
        snprintf(valbuf[1], sizeof(valbuf[1]), "p%d", ctx->idx);
        fields[1].value.v.str.s = valbuf[1];
        fields[1].value.v.str.len = strlen(valbuf[1]);

        snprintf(cat, sizeof(cat), "soak.t%d", ctx->idx);
        hpulogc_log_ex(HPULOGC_LEVEL_INFO, cat, "soak_driver.c", 1,
                       "producer_main", fields, 2, "record %llu op=%d",
                       (unsigned long long)i, r);
        ctx->count++;

        if (r == 0) {
            /* DEBUG: dropped whenever the hot-reload flip sets INFO. */
            hpulogc_log(HPULOGC_LEVEL_DEBUG, cat, "soak_driver.c", 2,
                        "producer_main", "debug %llu",
                        (unsigned long long)i);
        }
        if (r == 7) {
            hpulogc_log(HPULOGC_LEVEL_WARN, cat, "soak_driver.c", 3,
                        "producer_main", "warn %llu",
                        (unsigned long long)i);
        }
        if (r == 31) {
            /* Second category widens the registry scan surface. */
            hpulogc_log(HPULOGC_LEVEL_INFO, "soak.other", "soak_driver.c",
                        4, "producer_main", "other %llu",
                        (unsigned long long)i);
        }

        if (ctx->rate_us > 0) {
            long jitter = ctx->rate_us > 1 ? rand_r(&seed) % ctx->rate_us : 0;
            usleep((useconds_t)(ctx->rate_us / 2 + jitter));
        }
    }
    return NULL;
}

/**
 * @brief Rewrite the config file with the level flipped (and the null
 *        sink's queue size toggled on every other flip).
 *
 * Reads the original text once; each rewrite does an equal-length textual
 * replacement of "level = INFO"/"level = DEBUG" and "queue size =
 * 256kb"/"queue size = 512kb", writes a temp file and renames it into
 * place (atomic; the watcher never sees a half-written file). The watcher
 * thread picks the change up and hot reloads.
 *
 * @return 0 on success, -1 on IO failure.
 */
static int rewrite_config_flip(const char* path, int flip, FILE* log,
                               double elapsed)
{
    static char orig[16384];
    static char cur[16384];
    static int have_orig = 0;
    char tmp[560];
    char* p;
    FILE* fp;

    if (!have_orig) {
        size_t n;

        fp = fopen(path, "r");
        if (fp == NULL) {
            return -1;
        }
        n = fread(orig, 1, sizeof(orig) - 1, fp);
        orig[n] = '\0';
        fclose(fp);
        have_orig = 1;
    }
    snprintf(cur, sizeof(cur), "%s", orig);
    /* Equal-length in-place swaps (13 bytes); the template keeps a
     * trailing space on the level line so both forms round-trip. */
    p = strstr(cur, flip ? "level = INFO " : "level = DEBUG");
    if (p != NULL) {
        memcpy(p, flip ? "level = DEBUG" : "level = INFO ", 13);
    }
    p = strstr(cur, "queue size = 256kb");
    if (p != NULL) {
        memcpy(p, "queue size = 512kb", 18);
    } else {
        p = strstr(cur, "queue size = 512kb");
        if (p != NULL && flip) {
            memcpy(p, "queue size = 256kb", 18);
        }
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (fp == NULL) {
        return -1;
    }
    fputs(cur, fp);
    fclose(fp);
    if (rename(tmp, path) != 0) {
        return -1;
    }
    fprintf(log, "RELOAD t=%.0f level=%s\n", elapsed,
            flip ? "DEBUG" : "INFO");
    return 0;
}

int main(int argc, char** argv)
{
    const char* config = NULL;
    long duration_s = 0;
    long producers = 4;
    long rate_us = 5000; /* ~200 rec/s per producer (see report method) */
    long reload_s = SOAK_RELOAD_INTERVAL_S;
    int i;
    pthread_t* th;
    struct producer_ctx* ctx;
    FILE* evlog;
    char evpath[512];
    double t0;
    unsigned long long last_written = 0;
    unsigned long long first_quarter_rss = 0;
    unsigned long long first_quarter_n = 0;
    unsigned long long last_quarter_rss = 0;
    unsigned long long last_quarter_n = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = atol(argv[++i]);
        } else if (strcmp(argv[i], "--producers") == 0 && i + 1 < argc) {
            producers = atol(argv[++i]);
        } else if (strcmp(argv[i], "--rate-us") == 0 && i + 1 < argc) {
            rate_us = atol(argv[++i]);
        } else if (strcmp(argv[i], "--reload-s") == 0 && i + 1 < argc) {
            reload_s = atol(argv[++i]);
        } else {
            fprintf(stderr, "unknown arg %s\n", argv[i]);
            return 4;
        }
    }
    if (config == NULL || duration_s <= 0 || producers <= 0 ||
        producers > 64) {
        fprintf(stderr,
                "usage: soak_driver --config PATH --duration SEC "
                "[--producers N] [--rate-us USEC] [--reload-s SEC]\n");
        return 4;
    }

    /* Event/sample log next to the config file. */
    snprintf(evpath, sizeof(evpath), "%s.samples.log", config);
    evlog = fopen(evpath, "w");
    if (evlog == NULL) {
        fprintf(stderr, "cannot open %s\n", evpath);
        return 4;
    }

    if (hpulogc_init_from_file(config) != HPULOGC_OK) {
        fprintf(stderr, "init from %s failed\n", config);
        fclose(evlog);
        return 4;
    }

    signal(SIGPIPE, SIG_IGN);
    th = calloc((size_t)producers, sizeof(*th));
    ctx = calloc((size_t)producers, sizeof(*ctx));
    for (i = 0; i < producers; i++) {
        ctx[i].idx = i;
        ctx[i].rate_us = rate_us;
        pthread_create(&th[i], NULL, producer_main, &ctx[i]);
    }

    t0 = now_s();
    {
        double t;
        double quarter = (double)duration_s / 4.0;
        int reload_flip = 0;
        double next_reload = t0 + (double)reload_s;

        for (t = now_s(); t - t0 < (double)duration_s; t = now_s()) {
            hpulogc_stats_t st;
            long rss;
            double elapsed = t - t0;

            sleep(SOAK_SAMPLE_INTERVAL_S);

            if (hpulogc_get_stats(&st) != HPULOGC_OK) {
                fprintf(evlog, "FATAL t=%.0f get_stats failed\n",
                        now_s() - t0);
                g_violations++;
                break;
            }
            rss = rss_kib();

            /* Accounting upper bound: deliveries can never exceed
             * accepted (spec 4.10.3). */
            if (st.written + st.dropped + st.overwritten + st.throttled >
                st.accepted) {
                g_violations++;
                fprintf(evlog,
                        "VIOLATION t=%.0f acc=%llu w=%llu d=%llu o=%llu "
                        "th=%llu\n",
                        elapsed, st.accepted, st.written, st.dropped,
                        st.overwritten, st.throttled);
            }
            /* Heartbeat: producers alive and consumer draining. */
            if (st.written <= last_written && elapsed > 10.0) {
                g_stalls++;
                fprintf(evlog, "STALL t=%.0f written=%llu\n", elapsed,
                        st.written);
            }
            last_written = st.written;

            fprintf(evlog,
                    "SAMPLE t=%.0f acc=%llu written=%llu dropped=%llu "
                    "overwritten=%llu throttled=%llu used=%zu rss_kib=%ld\n",
                    elapsed, st.accepted, st.written, st.dropped,
                    st.overwritten, st.throttled, st.buffer_used, rss);
            fflush(evlog);
            if (elapsed < quarter) {
                first_quarter_rss += (unsigned long long)(rss > 0 ? rss : 0);
                first_quarter_n++;
            } else if (elapsed > 3.0 * quarter) {
                last_quarter_rss += (unsigned long long)(rss > 0 ? rss : 0);
                last_quarter_n++;
            }

            if (t >= next_reload) {
                if (rewrite_config_flip(config, reload_flip, evlog,
                                        elapsed) != 0) {
                    fprintf(evlog, "RELOAD t=%.0f FAILED (io)\n", elapsed);
                }
                reload_flip = !reload_flip;
                next_reload += (double)reload_s;
            }
        }
    }

    /* Stop producers, drain everything, final identity check. */
    g_stop = 1;
    for (i = 0; i < producers; i++) {
        pthread_join(th[i], NULL);
    }
    {
        hpulogc_stats_t st;
        unsigned long long submitted = 0;

        hpulogc_flush();
        hpulogc_sync();
        for (i = 0; i < producers; i++) {
            submitted += ctx[i].count;
        }
        if (hpulogc_get_stats(&st) == HPULOGC_OK) {
            fprintf(evlog,
                    "FINAL submitted=%llu accepted=%llu written=%llu "
                    "dropped=%llu overwritten=%llu throttled=%llu "
                    "fields_dropped=%llu stalls=%llu violations=%llu "
                    "rss_first_avg=%llu rss_last_avg=%llu\n",
                    submitted, st.accepted, st.written, st.dropped,
                    st.overwritten, st.throttled, st.fields_dropped,
                    g_stalls, g_violations,
                    first_quarter_n ? first_quarter_rss / first_quarter_n : 0,
                    last_quarter_n ? last_quarter_rss / last_quarter_n : 0);
            if (st.written + st.dropped + st.overwritten + st.throttled !=
                st.accepted) {
                g_violations++;
            }
        }
    }
    fprintf(evlog, "VERDICT %s stalls=%llu violations=%llu\n",
            (g_stalls == 0 && g_violations == 0) ? "PASS" : "FAIL",
            g_stalls, g_violations);
    fclose(evlog);
    free(th);
    free(ctx);
    hpulogc_shutdown();

    if (g_violations > 0) {
        return 3;
    }
    if (g_stalls > 0) {
        return 2;
    }
    return 0;
}
