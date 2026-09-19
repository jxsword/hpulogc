/**
 * @file test_lifecycle.c
 * @brief Lifecycle semantics (spec 7.2/7.5), stats semantics (7.3),
 *        build info, signal-safe channel and runtime controls.
 */

#include "test_util.h"
#include "hpulogc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/** @brief Static config helper shared by several tests. */
static hpulogc_config_t g_cfg;

static hpulogc_config_t* cfg_static(void)
{
    hpulogc_config_default(&g_cfg);
    return &g_cfg;
}

TEST(init_default_and_shutdown)
{
    CHECK_EQ(hpulogc_init_default(), HPULOGC_OK);
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    hpulogc_shutdown();
    /* idempotent */
    hpulogc_shutdown();
}

TEST(uninitialized_apis)
{
    hpulogc_stats_t st;

    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_DEBUG), HPULOGC_ERR_STATE);
    CHECK_EQ(hpulogc_flush(), HPULOGC_ERR_STATE);
    CHECK_EQ(hpulogc_sync(), HPULOGC_ERR_STATE);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_ERR_STATE);
    /* write APIs silently drop */
    hpulogc_log(HPULOGC_LEVEL_INFO, "cat", "f.c", 1, "f", "hello");
}

TEST(double_init_rejected)
{
    CHECK_EQ(hpulogc_init_default(), HPULOGC_OK);
    CHECK_EQ(hpulogc_init_default(), HPULOGC_ERR_STATE);
    hpulogc_shutdown();
    /* safe retry after shutdown */
    CHECK_EQ(hpulogc_init_default(), HPULOGC_OK);
    hpulogc_shutdown();
}

TEST(init_null_equals_default)
{
    CHECK_EQ(hpulogc_init(NULL), HPULOGC_OK);
    hpulogc_shutdown();
}

TEST(init_from_file_null_rejected)
{
    CHECK_EQ(hpulogc_init_from_file(NULL), HPULOGC_ERR_INVALID_ARG);
}

TEST(init_from_file_missing_rejected)
{
    CHECK_EQ(hpulogc_init_from_file("/nonexistent/hpulogc_42.ini"),
             HPULOGC_ERR_CONFIG);
    /* library stays uninitialized after failure (spec 7.5) */
    CHECK_EQ(hpulogc_flush(), HPULOGC_ERR_STATE);
}

TEST(config_default_fills_values)
{
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg);
    CHECK_EQ(cfg.level, HPULOGC_LEVEL_INFO);
    CHECK_EQ(cfg.buffer_size, 1024U * 1024U);
    CHECK_EQ(cfg.overflow_policy, HPULOGC_OVERFLOW_DISCARD);
    CHECK_EQ(cfg.batch_size, 64);
    CHECK_EQ(cfg.flush_interval_ms, 100);
    CHECK_EQ(cfg.shutdown_timeout_ms, 5000);
    CHECK_EQ(cfg.escape_injection, 1);
    CHECK_EQ(cfg.max_log_length, 4096);
    CHECK_EQ(cfg.crash_safety, HPULOGC_CRASH_SHUTDOWN);
    CHECK_EQ(cfg.signal_safe, 0);
    hpulogc_config_default(NULL); /* safe ignore */
}

TEST(code_config_invalid_values_rejected)
{
    hpulogc_config_t cfg;
    hpulogc_output_t out;

    hpulogc_config_default(&cfg);
    cfg.level = (hpulogc_level_t)99;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_INVALID_ARG);
    hpulogc_config_default(&cfg);

    cfg.max_log_length = 10; /* below 256 */
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_INVALID_ARG);
    hpulogc_config_default(&cfg);

    cfg.buffer_size = 100; /* tiny buffer is auto-raised, not rejected */
    /* but an invalid overflow policy is rejected */
    cfg.overflow_policy = (hpulogc_overflow_policy_t)42;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_INVALID_ARG);
    hpulogc_config_default(&cfg);

    /* out-of-range rule level */
    {
        hpulogc_rule_t rule;

        memset(&rule, 0, sizeof(rule));
        rule.category = "app";
        rule.min_level = HPULOGC_LEVEL_FATAL;
        rule.max_level = HPULOGC_LEVEL_ERROR; /* min > max */
        rule.format = "standard";
        rule.outputs = NULL;
        rule.output_count = 0;
        cfg.rules = &rule;
        cfg.rule_count = 1;
        CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);
    }
    hpulogc_config_default(&cfg);

    /* unknown output reference */
    {
        const char* outs[] = { "does_not_exist" };
        hpulogc_rule_t rule;

        memset(&rule, 0, sizeof(rule));
        rule.category = "app";
        rule.min_level = HPULOGC_LEVEL_TRACE;
        rule.max_level = HPULOGC_LEVEL_FATAL;
        rule.format = "standard";
        rule.outputs = outs;
        rule.output_count = 1;
        cfg.rules = &rule;
        cfg.rule_count = 1;
        CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);
    }
    hpulogc_config_default(&cfg);

    /* file output without a path */
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = NULL;
    cfg.outputs = &out;
    cfg.output_count = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);
}

TEST(stats_counting_and_buffer_report)
{
    hpulogc_stats_t st;
    static const char* names[] = { "out0" };
    hpulogc_rule_t rule;
    hpulogc_output_t out;
    char logpath[256];

    snprintf(logpath, sizeof(logpath), "/tmp/hpu_stats_%d.log", (int)getpid());
    unlink(logpath);

    hpulogc_config_default(cfg_static());
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    g_cfg.outputs = &out;
    g_cfg.output_count = 1;
    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    g_cfg.rules = &rule;
    g_cfg.rule_count = 1;

    CHECK_EQ(hpulogc_init(&g_cfg), HPULOGC_OK);
    {
        /* log 10 records */
        int i;

        for (i = 0; i < 10; i++) {
            hpulogc_log(HPULOGC_LEVEL_INFO, "cat", "f.c", i, "fn", "n=%d",
                        i);
        }
        CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
        CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
        CHECK_EQ(st.accepted, 10);
        CHECK_EQ(st.written, 10);
        CHECK_EQ(st.dropped, 0);
        CHECK_EQ(st.throttled, 0);
#if HPULOGC_ENABLE_ASYNC
        CHECK_EQ(st.buffer_size, 1024U * 1024U);
#else
        CHECK_EQ(st.buffer_size, 0U); /* no ring in sync builds */
#endif
        /* per-level API */
        CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_ERROR), HPULOGC_OK);
        hpulogc_log(HPULOGC_LEVEL_INFO, "cat", "f.c", 1, "fn", "hidden");
        hpulogc_log(HPULOGC_LEVEL_ERROR, "cat", "f.c", 1, "fn", "shown");
        CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
        CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
        CHECK_EQ(st.accepted, 11);
        CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_OFF), HPULOGC_OK);
        hpulogc_log(HPULOGC_LEVEL_FATAL, "cat", "f.c", 1, "fn", "off");
        CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_INFO), HPULOGC_OK);
        CHECK_EQ(hpulogc_set_level((hpulogc_level_t)77),
                 HPULOGC_ERR_INVALID_ARG);
    }
    hpulogc_shutdown();
    unlink(logpath);
}

TEST(errno_preserved_by_log_apis)
{
    hpulogc_config_default(&g_cfg);
    CHECK_EQ(hpulogc_init(&g_cfg), HPULOGC_OK);
    {
        errno = 77;
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", "f.c", 1, "f", "x");
        hpulogc_log_signal_safe(HPULOGC_LEVEL_FATAL, "signal");
        CHECK_EQ(errno, 77);
    }
    hpulogc_shutdown();
}

TEST(build_info_matches_build)
{
    hpulogc_build_info_t info;

    hpulogc_get_build_info(&info);
    CHECK(info.build_version != NULL);
    CHECK(info.concurrency != NULL);
    CHECK(info.has_async == 0 || info.has_async == 1);
    hpulogc_get_build_info(NULL); /* safe ignore */
}
