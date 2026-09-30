/**
 * @file test_sink_filter.c
 * @brief Per-sink `filter keys` whitelist end-to-end (§4.7.3, D-R14):
 *        sync + async filtering, per-sink fields_dropped accounting
 *        orthogonal to the global three-cause statistic (§4.11.2), and
 *        strict-init validation of malformed whitelist values.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <string.h>

/* ---- A capturing custom sink (records field keys verbatim) -------- */

#define FILTCAP_MAX_FIELDS 8
/** @brief All keys used by this test are 2 bytes long; decoded keys are
 *         NOT NUL-terminated (they point into the wire region), so the
 *         capture compares them by fixed length instead of strlen. */
#define FILTCAP_KEY_LEN 2

/** @brief Capture sink state (file-scope: there is no public handle
 *        lookup, so the emit callback stashes observations here; the
 *        flush handshake orders the async worker's writes). */
static unsigned long long g_cap_count;    /*!< Events seen */
static size_t g_cap_last_field_count;     /*!< Field count of the last event */
static char g_cap_last_keys[FILTCAP_MAX_FIELDS][FILTCAP_KEY_LEN + 1];
                                                /*!< Key copies (NUL-ended) */
static int g_cap_last_keys_valid;         /*!< Key copies populated flag */

static void filtcap_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    size_t i;

    (void)sink;
    g_cap_count++;
    g_cap_last_field_count = ev->field_count;
    g_cap_last_keys_valid = ev->field_count > 0;
    for (i = 0; i < ev->field_count && i < FILTCAP_MAX_FIELDS; i++) {
        memcpy(g_cap_last_keys[i], ev->fields[i].key, FILTCAP_KEY_LEN);
        g_cap_last_keys[i][FILTCAP_KEY_LEN] = '\0';
    }
}

static int filtcap_batch(hpulogc_sink_t* sink,
                         const hpulogc_event_t* const* evs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        filtcap_emit(sink, evs[i]);
    }
    return (int)n;
}

static const hpulogc_sink_ops_t g_filtcap_ops = {
    "filtcap",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC,
    0,    /* priv_size (state lives in the file-scope capture globals) */
    NULL, /* configure */
    NULL, /* init */
    NULL, /* start */
    filtcap_emit,
    filtcap_batch,
    NULL, /* flush */
    NULL, /* sync */
    NULL, /* periodic */
    NULL, /* destroy */
    { NULL, NULL, NULL, NULL }
};

/** @brief One-time registration guard (duplicate types are config errors). */
static int register_filtcap(void)
{
    static int done = 0;
    int rc = HPULOGC_OK;

    if (!done) {
        rc = hpulogc_sink_register(&g_filtcap_ops);
        done = 1;
    }
    return rc;
}

/** @brief Build a one-sink code config with the given filter value
 *        (NULL val = key omitted) and an optional async=on. */
static void build_cfg(hpulogc_config_t* cfg, hpulogc_sink_decl_t* sinks,
                      const char* filter_val, int async)
{
    static const char* const names[] = { "cap0" };
    static const char* const k_none[] = { NULL };
    static const char* const v_none[] = { NULL };
    static const char* const k_async[] = { "async" };
    static const char* const v_async[] = { "on" };
    static const char* const k_filter[] = { "filter keys" };
    static const char* const k_both[] = { "filter keys", "async" };
    /* The value arrays are read by hpulogc_init AFTER this function
     * returns, so the filter value must live in static storage (a
     * pointer to the local parameter would dangle). */
    static char v_filter[1][128];
    static const char* v_filter_ptr[1];
    static const char* v_both[2];

    hpulogc_config_default(cfg);
    if (filter_val != NULL) {
        snprintf(v_filter[0], sizeof(v_filter[0]), "%s", filter_val);
        v_filter_ptr[0] = v_filter[0];
    }
    if (filter_val != NULL && async) {
        v_both[0] = v_filter[0];
        v_both[1] = "on";
        sinks->keys = k_both;
        sinks->vals = v_both;
        sinks->count = 2;
    } else if (filter_val != NULL) {
        sinks->keys = k_filter;
        sinks->vals = v_filter_ptr;
        sinks->count = 1;
    } else if (async) {
        sinks->keys = k_async;
        sinks->vals = v_async;
        sinks->count = 1;
    } else {
        sinks->keys = k_none;
        sinks->vals = v_none;
        sinks->count = 0;
    }
    sinks->name = "cap0";
    sinks->type = "filtcap";
    cfg->sinks = sinks;
    cfg->sink_count = 1;
    cfg->default_outputs = names;
    cfg->default_output_count = 1;
}

/** @brief Log one event carrying k1 (i64) and k2 (str). */
static void log_two_fields(void)
{
    hpulogc_field_t fields[2];

    fields[0].key = "k1";
    fields[0].value.type = HPULOGC_FIELD_I64;
    fields[0].value.v.i64 = 42;
    fields[1].key = "k2";
    fields[1].value.type = HPULOGC_FIELD_STR;
    fields[1].value.v.str.s = "vv";
    fields[1].value.v.str.len = 2;
    HPULOGC_INFO_EX("cap", fields, 2, "hello %d", 7);
}

TEST(filter_sync_whitelist)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    hpulogc_sink_stats_t st;

    CHECK_EQ(register_filtcap(), HPULOGC_OK);
    g_cap_count = 0;
    g_cap_last_field_count = 0;
    g_cap_last_keys_valid = 0;
    build_cfg(&cfg, sinks, "k1", 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    log_two_fields();
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1);
    CHECK_EQ(st.fields_dropped, 1); /* k2 removed by the whitelist */
    CHECK_EQ(g_cap_count, 1);
    CHECK_EQ(g_cap_last_field_count, 1);
    CHECK(g_cap_last_keys_valid);
    CHECK(memcmp(g_cap_last_keys[0], "k1", FILTCAP_KEY_LEN) == 0);
    {
        /* Orthogonality (§4.11.2): filter removals never enter the
         * global three-cause fields_dropped statistic. */
        hpulogc_stats_t g;

        CHECK_EQ(hpulogc_get_stats(&g), HPULOGC_OK);
        CHECK_EQ(g.fields_dropped, 0);
    }
    hpulogc_shutdown();
}

TEST(filter_sync_no_match)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    hpulogc_sink_stats_t st;

    CHECK_EQ(register_filtcap(), HPULOGC_OK);
    g_cap_count = 0;
    g_cap_last_field_count = 0;
    g_cap_last_keys_valid = 0;
    build_cfg(&cfg, sinks, "zzz", 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    log_two_fields();
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1); /* the line still ships, fieldless */
    CHECK_EQ(st.fields_dropped, 2);
    CHECK_EQ(g_cap_last_field_count, 0);
    CHECK_EQ(g_cap_last_keys_valid, 0);
    hpulogc_shutdown();
}

TEST(filter_disabled_when_absent_or_empty)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    hpulogc_sink_stats_t st;

    CHECK_EQ(register_filtcap(), HPULOGC_OK);

    /* key absent: full pass-through, fields_dropped stays 0 */
    g_cap_last_field_count = 0;
    build_cfg(&cfg, sinks, NULL, 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    log_two_fields();
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1);
    CHECK_EQ(st.fields_dropped, 0);
    CHECK_EQ(g_cap_last_field_count, 2);
    hpulogc_shutdown();

    /* empty value = no filter (same as unset) */
    g_cap_last_field_count = 0;
    build_cfg(&cfg, sinks, "", 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    log_two_fields();
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
    CHECK_EQ(st.fields_dropped, 0);
    CHECK_EQ(g_cap_last_field_count, 2);
    hpulogc_shutdown();
}

TEST(filter_async)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    hpulogc_sink_stats_t st;

    CHECK_EQ(register_filtcap(), HPULOGC_OK);
    g_cap_count = 0;
    g_cap_last_field_count = 0;
    g_cap_last_keys_valid = 0;
    build_cfg(&cfg, sinks, "k1", 1);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    log_two_fields();
    /* hpulogc_flush only drains the first-level ring; the second-level
     * queue worker runs asynchronously, so poll (bounded) until the
     * worker's filtered accounting has landed. */
    {
        int i;

        for (i = 0; i < 2000; i++) {
            CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
            if (st.written == 1 && st.fields_dropped == 1) {
                break;
            }
            hpu_test_sleep_ms(1);
        }
    }

    CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1);
    CHECK_EQ(st.fields_dropped, 1);
    /* The g_cap_* capture state is written by the sink worker thread
     * (plain, non-atomic globals). hpulogc_shutdown() joins that thread,
     * which provides the happens-before needed to read them here —
     * polling the atomic stats alone does not (TSan-flagged race). */
    hpulogc_shutdown();
    CHECK_EQ(g_cap_count, 1);
    CHECK_EQ(g_cap_last_field_count, 1);
    CHECK(memcmp(g_cap_last_keys[0], "k1", FILTCAP_KEY_LEN) == 0);
}

TEST(filter_strict_init_errors)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];

    CHECK_EQ(register_filtcap(), HPULOGC_OK);

    /* 9 keys: over the HPULOGC_MAX_SINK_FILTER_KEYS (8) limit */
    build_cfg(&cfg, sinks, "a1,a2,a3,a4,a5,a6,a7,a8,a9", 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);

    /* a key longer than HPULOGC_MAX_FIELD_KEY_LEN (64): 65 a's */
    build_cfg(&cfg, sinks,
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
              "aaaaaaaaaaaaaaaa", 0); /* 49 + 16 = 65 */
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);

    /* empty token in the list */
    build_cfg(&cfg, sinks, "k1,,k2", 0);
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);
}
