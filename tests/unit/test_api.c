/**
 * @file test_api.c
 * @brief Level-enablement query API and gate macros (spec rd_v0.6 §4.1,
 *        decision D-R4): level-filter semantics, per-category overrides,
 *        hot-reload immediacy, side-effect freedom and thread safety.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <string.h>

/** @brief Shared file-output config state (one output, one catch-all rule). */
static hpulogc_config_t g_cfg;
static hpulogc_output_t g_out;
static hpulogc_rule_t g_rule;
static const char* g_out_names[] = { "out0" };
static char g_logpath[256];

/**
 * @brief Build a minimal file-output config writing to a per-pid scratch
 *        path (keeps console output out of the ctest log).
 */
static hpulogc_config_t* file_cfg(void)
{
    char tmpdir[128];

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_logpath, sizeof(g_logpath), "%s/hpu_api_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(g_logpath);

    hpulogc_config_default(&g_cfg);
    g_cfg.level = HPULOGC_LEVEL_TRACE; /* default is INFO; tests below
                                        * manipulate the threshold from a
                                        * known TRACE base */
    memset(&g_out, 0, sizeof(g_out));
    g_out.type = HPULOGC_OUT_FILE;
    g_out.path = g_logpath;
    g_cfg.outputs = &g_out;
    g_cfg.output_count = 1;
    memset(&g_rule, 0, sizeof(g_rule));
    g_rule.category = "*";
    g_rule.min_level = HPULOGC_LEVEL_TRACE;
    g_rule.max_level = HPULOGC_LEVEL_FATAL;
    g_rule.format = "standard";
    g_rule.outputs = g_out_names;
    g_rule.output_count = 1;
    g_cfg.rules = &g_rule;
    g_cfg.rule_count = 1;
    return &g_cfg;
}

TEST(level_enabled_uninitialized)
{
    /* Idempotent shutdown puts us in the uninitialized state regardless of
     * test execution order. */
    hpulogc_shutdown();
    CHECK_EQ(hpulogc_level_enabled(NULL, HPULOGC_LEVEL_TRACE), 0);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_INFO), 0);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_FATAL), 0);
    CHECK_EQ(hpulogc_level_enabled(NULL, HPULOGC_LEVEL_OFF), 0);
}

TEST(level_enabled_global_threshold)
{
    CHECK_EQ(hpulogc_init(file_cfg()), HPULOGC_OK);

    /* Default threshold is TRACE: everything valid is enabled. */
    CHECK_EQ(hpulogc_level_enabled(NULL, HPULOGC_LEVEL_TRACE), 1);
    CHECK_EQ(hpulogc_level_enabled("", HPULOGC_LEVEL_INFO), 1);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_FATAL), 1);

    /* OFF / out-of-range as record levels: dropped (spec 4.1). */
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_OFF), 0);
    CHECK_EQ(hpulogc_level_enabled("cat", (hpulogc_level_t)77), 0);
    CHECK_EQ(hpulogc_level_enabled("cat", (hpulogc_level_t)-2), 0);

    /* Runtime threshold flip takes effect immediately. */
    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_ERROR), HPULOGC_OK);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_WARN), 0);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_ERROR), 1);
    CHECK_EQ(hpulogc_level_enabled(NULL, HPULOGC_LEVEL_INFO), 0);

    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_OFF), HPULOGC_OK);
    CHECK_EQ(hpulogc_level_enabled("cat", HPULOGC_LEVEL_FATAL), 0);

    hpulogc_shutdown();
    hpu_test_unlink(g_logpath);
}

#if HPULOGC_ENABLE_CATEGORY
TEST(level_enabled_per_category_override)
{
    CHECK_EQ(hpulogc_init(file_cfg()), HPULOGC_OK);
    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_TRACE), HPULOGC_OK);
    CHECK_EQ(hpulogc_set_level_for_category("net", HPULOGC_LEVEL_ERROR),
             HPULOGC_OK);

    /* Override wins over the global threshold (decision 19 semantics). */
    CHECK_EQ(hpulogc_level_enabled("net", HPULOGC_LEVEL_INFO), 0);
    CHECK_EQ(hpulogc_level_enabled("net", HPULOGC_LEVEL_ERROR), 1);
    CHECK_EQ(hpulogc_level_enabled("other", HPULOGC_LEVEL_INFO), 1);

    /* Clearing the override restores the global threshold. */
    CHECK_EQ(hpulogc_set_level_for_category("net", (hpulogc_level_t)-1),
             HPULOGC_OK);
    CHECK_EQ(hpulogc_level_enabled("net", HPULOGC_LEVEL_INFO), 1);

    hpulogc_shutdown();
    hpu_test_unlink(g_logpath);
}
#endif /* HPULOGC_ENABLE_CATEGORY */

TEST(level_enabled_unregistered_category)
{
    /* A never-logged category is judged by the global threshold alone and
     * the query must not register it (side-effect free, decision D-R4). */
    CHECK_EQ(hpulogc_init(file_cfg()), HPULOGC_OK);
    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_WARN), HPULOGC_OK);
    CHECK_EQ(hpulogc_level_enabled("never_logged_xyz", HPULOGC_LEVEL_INFO),
             0);
    CHECK_EQ(hpulogc_level_enabled("never_logged_xyz", HPULOGC_LEVEL_WARN),
             1);
    hpulogc_shutdown();
    hpu_test_unlink(g_logpath);
}

TEST(enabled_macros_match_pipeline)
{
    hpulogc_stats_t st;

    CHECK_EQ(hpulogc_init(file_cfg()), HPULOGC_OK);
    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_INFO), HPULOGC_OK);

    CHECK(HPULOGC_INFO_ENABLED("m"));
    if (HPULOGC_INFO_ENABLED("m")) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "m", "f.c", 1, "fn", "shown %d", 1);
    }
    CHECK(!HPULOGC_DEBUG_ENABLED("m"));
    if (HPULOGC_DEBUG_ENABLED("m")) {
        hpulogc_log(HPULOGC_LEVEL_DEBUG, "m", "f.c", 1, "fn", "hidden");
    }
    /* Format-attribute sanity: variadic calls are checked at compile time
     * (GCC/Clang), here just exercise a literal-format path. */
    HPULOGC_INFO("m", "literal %s", "fmt");

    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    /* The gated-out DEBUG record must not have been accepted. */
    CHECK_EQ(st.accepted, 2);
    CHECK_EQ(st.throttled, 0);

    hpulogc_shutdown();
    hpu_test_unlink(g_logpath);
}

#if HPULOGC_ENABLE_INI
#include "core/core_internal.h"

TEST(level_enabled_hot_reload)
{
    char tmpdir[128];
    char cfgfile[300];
    char logpath[300];
    FILE* fp;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(cfgfile, sizeof(cfgfile), "%s/hpu_api_hot_%d.ini", tmpdir,
             hpu_test_getpid());
    snprintf(logpath, sizeof(logpath), "%s/hpu_api_hot_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(logpath);

    fp = fopen(cfgfile, "w");
    CHECK(fp != NULL);
    fprintf(fp,
            "[global]\n"
            "strict init = true\n"
            "level = INFO\n"
            "default format = standard\n"
            "default outputs = f\n"
            "[outputs]\n"
            "f = file, path=%s\n"
            "[rules]\n"
            "*.* = standard, f\n",
            logpath);
    fclose(fp);

    CHECK_EQ(hpulogc_init_from_file(cfgfile), HPULOGC_OK);
    CHECK_EQ(hpulogc_level_enabled("c", HPULOGC_LEVEL_INFO), 1);
    CHECK_EQ(hpulogc_level_enabled("c", HPULOGC_LEVEL_DEBUG), 0);

    /* Flip the threshold in the file and hot-reload: the query must
     * reflect the new level immediately (no restart, no logging in
     * between). */
    fp = fopen(cfgfile, "w");
    CHECK(fp != NULL);
    fprintf(fp,
            "[global]\n"
            "strict init = true\n"
            "level = ERROR\n"
            "default format = standard\n"
            "default outputs = f\n"
            "[outputs]\n"
            "f = file, path=%s\n"
            "[rules]\n"
            "*.* = standard, f\n",
            logpath);
    fclose(fp);

    CHECK_EQ(hpu_core_trigger_reload(), 0);
    CHECK_EQ(hpulogc_level_enabled("c", HPULOGC_LEVEL_INFO), 0);
    CHECK_EQ(hpulogc_level_enabled("c", HPULOGC_LEVEL_ERROR), 1);

    hpulogc_shutdown();
    hpu_test_unlink(cfgfile);
    hpu_test_unlink(logpath);
}
#endif /* HPULOGC_ENABLE_INI */

/* ---- Thread safety: gate queries and logging from several threads ---- */

/* SPSC builds support exactly one producer (spec 4.2); multi-producer
 * submission is only valid in MPSC/MPMC builds. */
#if defined(HPULOGC_CONCURRENCY_SPSC)
#define API_TEST_THREADS 1
#else
#define API_TEST_THREADS 4
#endif
#define API_TEST_ITERS 2000

struct api_thr_arg {
    long info_enabled; /*!< Count of INFO queries that returned non-zero */
    long debug_enabled; /*!< Count of DEBUG queries that returned non-zero */
};

static void* api_thr_fn(void* raw)
{
    struct api_thr_arg* arg = raw;
    int i;

    for (i = 0; i < API_TEST_ITERS; i++) {
        if (hpulogc_level_enabled("thr", HPULOGC_LEVEL_INFO)) {
            arg->info_enabled++;
            hpulogc_log(HPULOGC_LEVEL_INFO, "thr", "f.c", 1, "fn", "i=%d",
                        i);
        }
        if (hpulogc_level_enabled("thr", HPULOGC_LEVEL_DEBUG)) {
            arg->debug_enabled++;
            hpulogc_log(HPULOGC_LEVEL_DEBUG, "thr", "f.c", 1, "fn", "d=%d",
                        i);
        }
        /* Query a second, per-thread-ish category to widen the registry
         * scan surface. */
        (void)hpulogc_level_enabled("thr2", HPULOGC_LEVEL_WARN);
    }
    return NULL;
}

TEST(level_enabled_multithreaded)
{
    hpu_test_thread_t th[API_TEST_THREADS];
    struct api_thr_arg args[API_TEST_THREADS];
    hpulogc_stats_t st;
    long total_info = 0;
    int i;

    CHECK_EQ(hpulogc_init(file_cfg()), HPULOGC_OK);
    /* Fixed threshold for the whole run: answers are deterministic. */
    CHECK_EQ(hpulogc_set_level(HPULOGC_LEVEL_INFO), HPULOGC_OK);

    memset(args, 0, sizeof(args));
    for (i = 0; i < API_TEST_THREADS; i++) {
        CHECK_EQ(hpu_test_thread_create(&th[i], api_thr_fn, &args[i]), 0);
    }
    for (i = 0; i < API_TEST_THREADS; i++) {
        hpu_test_thread_join(&th[i]);
        total_info += args[i].info_enabled;
        /* Threshold never flipped: zero DEBUG queries may pass. */
        CHECK_EQ(args[i].debug_enabled, 0);
    }

    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(total_info, (long)API_TEST_THREADS * API_TEST_ITERS);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    /* Every gated-in record entered the pipeline (throttle disabled). */
    CHECK_EQ(st.accepted, (unsigned long long)total_info);
    /* Accounting identity closes after flush (spec 4.10.3). */
    CHECK_EQ(st.accepted,
             st.written + st.dropped + st.overwritten + st.throttled);

    hpulogc_shutdown();
    hpu_test_unlink(g_logpath);
}
