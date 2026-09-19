/**
 * @file test_diskfail.c
 * @brief Disk failure handling via the test-only I/O injection hook
 *      (spec 9): reopen-once, per-line loss accounting and rate-limited
 *      warnings; init-time open failure is fail-fast.
 */

#include "test_util.h"
#include "hpulogc.h"
#include "output/output.h"

#include <stdio.h>
#include <string.h>
#include "portability.h"

/** @brief Injection plan: which ops fail and how often. */
static int g_fail_open;
static int g_fail_write;
static int g_fail_remaining;

/**
 * @brief Hook implementation: fails the configured operations.
 */
static int fail_hook(int op, const char* path)
{
    (void)path;
    fprintf(stderr, "HOOK op=%d remaining=%d\n", op, g_fail_remaining);
    if (g_fail_remaining <= 0) {
        return 0;
    }
    g_fail_remaining--;
    if (op == 0) {
        return g_fail_open;
    }
    if (op == 1) {
        return g_fail_write;
    }
    return 0;
}

TEST(disk_write_failure_counts_dropped)
{
    char logpath[256];
    char tmpdir[128];
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    static const char* names[] = { "out0" };
    hpulogc_rule_t rule;
    hpulogc_stats_t st;
    int i;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(logpath, sizeof(logpath), "%s/hpu_disk_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;
    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpu_io_fail_hook = fail_hook;

    /* first flush: write fails, reopen fails, retry write fails
     * (three injected operations, spec 9 reopen-once semantics) */
    g_fail_open = 1;
    g_fail_write = 1;
    g_fail_remaining = 3;
    hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "lost line");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK(st.dropped >= 1); /* write failure became a drop (spec 9) */

    /* recovery: hook exhausted, normal writes succeed again */
    g_fail_remaining = 0;
    hpu_io_fail_hook = NULL;
    for (i = 0; i < 5; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "recovered %d",
                    i);
    }
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);

    hpulogc_shutdown();
    hpu_test_unlink(logpath);
}

TEST(disk_open_failure_fails_init)
{
    char logpath[256];
    char tmpdir[128];
    hpulogc_config_t cfg;
    hpulogc_output_t out;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(logpath, sizeof(logpath), "%s/hpu_disk_%d_b.log", tmpdir,
             hpu_test_getpid());

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;

    hpu_io_fail_hook = fail_hook;
    g_fail_open = 1;
    g_fail_write = 0;
    g_fail_remaining = 10;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_IO); /* fail-fast (spec 9) */
    g_fail_remaining = 0;
    hpu_io_fail_hook = NULL;

    /* library stays uninitialized and can retry safely */
    CHECK_EQ(hpulogc_flush(), HPULOGC_ERR_STATE);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpulogc_shutdown();
    hpu_test_unlink(logpath);
}
