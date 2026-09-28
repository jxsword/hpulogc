/**
 * @file test_mpmc.c
 * @brief End-to-end tests for the MPMC pipeline (rd_v0.3): N consumer
 *        threads, exactly-once delivery through the whole stack, flush
 *        semantics and build info.
 *
 * This test binary is registered in MPMC builds only (the multi-consumer
 * pipeline does not exist in SPSC/MPSC builds).
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"
#include "atomic/hpulogc_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define E2E_PRODUCERS 4U
#define E2E_PER_PRODUCER 500U
#define E2E_CONSUMERS 4U

/** @brief Producer thread context. */
typedef struct e2e_arg {
    unsigned id;              /*!< Producer id encoded into every message */
    unsigned count;           /*!< Records to emit */
    hpu_atomic_u32 finished;  /*!< Set right before thread exit */
} e2e_arg_t;

/**
 * @brief Producer thread: emit fixed-width parseable lines.
 *
 * The message is "MPMC P<u> S<9-digit seq>x": the trailing x terminator
 * keeps substring counting exact (S17 cannot match S170).
 */
static void* e2e_producer_thread(void* raw)
{
    e2e_arg_t* a = raw;
    unsigned i;

    for (i = 0; i < a->count; i++) {
        HPULOGC_INFO("mpmc", "MPMC P%u S%09lux", a->id,
                     (unsigned long)i);
    }
    hpu_at_store_u32(&a->finished, 1, HPU_MO_RELEASE);
    return NULL;
}

/**
 * @brief Read a whole file into a malloc'ed buffer.
 * @return Buffer (caller frees) or NULL.
 */
static char* read_file_dyn(const char* path, size_t* out_len)
{
    FILE* fp = fopen(path, "rb");
    size_t cap = 65536, len = 0;
    char* buf;

    if (fp == NULL) {
        return NULL;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    for (;;) {
        size_t n;

        if (len + 4096 > cap) {
            char* grown;

            cap *= 2;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = grown;
        }
        n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) {
            break;
        }
        len += n;
    }
    fclose(fp);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/**
 * @brief Count non-overlapping occurrences of @p needle in @p haystack.
 */
static unsigned long count_substring(const char* haystack, const char* needle)
{
    unsigned long n = 0;
    const char* p = haystack;
    size_t needle_len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += needle_len;
    }
    return n;
}

TEST(mpmc_build_info_reports_mpmc)
{
    hpulogc_build_info_t info;

    hpulogc_get_build_info(&info);
    CHECK(info.concurrency != NULL);
    CHECK(strcmp(info.concurrency, "mpmc") == 0);
}

TEST(mpmc_e2e_exactly_once)
{
    char logpath[300];
    char tmpdir[128];
    char needle[64];
    static const char* names[] = { "out0" };
    hpulogc_config_t cfg;
    hpulogc_output_t outs[1];
    hpulogc_stats_t st;
    e2e_arg_t args[E2E_PRODUCERS];
    hpu_test_thread_t threads[E2E_PRODUCERS];
    char* content;
    size_t content_len;
    unsigned long total = 0;
    unsigned p, i;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(logpath, sizeof(logpath), "%s/hpu_mpmc_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    cfg.level = HPULOGC_LEVEL_INFO;
    cfg.buffer_size = 4U * 1024U * 1024U; /* no drops possible */
    cfg.consumer_threads = E2E_CONSUMERS;
    cfg.shutdown_timeout_ms = 30000;
    memset(outs, 0, sizeof(outs));
    outs[0].type = HPULOGC_OUT_FILE;
    outs[0].path = logpath;
    cfg.outputs = outs;
    cfg.output_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    for (i = 0; i < E2E_PRODUCERS; i++) {
        args[i].id = i;
        args[i].count = E2E_PER_PRODUCER;
        hpu_at_store_u32(&args[i].finished, 0, HPU_MO_RELAXED);
        CHECK_EQ(hpu_test_thread_create(&threads[i], e2e_producer_thread,
                                        &args[i]), 0);
    }
    for (i = 0; i < E2E_PRODUCERS; i++) {
        hpu_test_thread_join(&threads[i]);
    }

    /* Flush: everything enqueued must be on disk while still running. */
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.accepted, E2E_PRODUCERS * E2E_PER_PRODUCER);
    CHECK_EQ(st.written, E2E_PRODUCERS * E2E_PER_PRODUCER);
    CHECK_EQ(st.dropped, 0);
    CHECK_EQ(st.overwritten, 0);

    hpulogc_shutdown();

    content = read_file_dyn(logpath, &content_len);
    CHECK(content != NULL);
    for (p = 0; p < E2E_PRODUCERS; p++) {
        for (i = 0; i < E2E_PER_PRODUCER; i++) {
            unsigned long n;

            snprintf(needle, sizeof(needle), "MPMC P%u S%09lux", p,
                     (unsigned long)i);
            n = count_substring(content, needle);
            if (n != 1) {
                CHECK_EQ(n, 1); /* reports the actual count on failure */
            }
            total += n;
        }
    }
    CHECK_EQ(total, E2E_PRODUCERS * E2E_PER_PRODUCER);
    free(content);
    hpu_test_unlink(logpath);
}

TEST(mpmc_flush_visible_before_shutdown)
{
    char logpath[300];
    char tmpdir[128];
    static const char* names[] = { "out0" };
    hpulogc_config_t cfg;
    hpulogc_output_t outs[1];
    char* content;
    size_t content_len;
    unsigned i;
    unsigned long found = 0;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(logpath, sizeof(logpath), "%s/hpu_mpmc_flush_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    cfg.buffer_size = 1024U * 1024U;
    cfg.consumer_threads = 2;
    memset(outs, 0, sizeof(outs));
    outs[0].type = HPULOGC_OUT_FILE;
    outs[0].path = logpath;
    cfg.outputs = outs;
    cfg.output_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    for (i = 0; i < 100; i++) {
        HPULOGC_INFO("flush", "FLUSH MARK %09u", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    content = read_file_dyn(logpath, &content_len);
    CHECK(content != NULL);
    for (i = 0; i < 100; i++) {
        char needle[64];

        snprintf(needle, sizeof(needle), "FLUSH MARK %09u", i);
        found += count_substring(content, needle);
    }
    CHECK_EQ(found, 100);
    free(content);

    hpulogc_shutdown();
    hpu_test_unlink(logpath);
}

TEST(mpmc_config_range)
{
    hpulogc_config_t cfg;

    /* Range is validated in every MPMC build (1..16). */
    hpulogc_config_default(&cfg);
    cfg.consumer_threads = 0;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_INVALID_ARG);

    hpulogc_config_default(&cfg);
    cfg.consumer_threads = 17;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_INVALID_ARG);
}
