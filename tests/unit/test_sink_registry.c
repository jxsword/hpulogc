/**
 * @file test_sink_registry.c
 * @brief Sink type registry, custom sink end-to-end and per-sink stats
 *        (rd_v0.6 §4.10).
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <string.h>

/* ---- A capturing custom sink used by several cases ---------------- */

/** @brief Capture state (priv of the "testcap" sink type). */
typedef struct testcap_priv {
    unsigned long long count;     /*!< Events seen */
    unsigned long long line_bytes;/*!< Sum of line lengths */
    size_t last_field_count;      /*!< Field count of the last event */
    char last_line[256];          /*!< Copy of the last line */
} testcap_priv_t;

static void testcap_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    testcap_priv_t* p = hpulogc_sink_priv(sink);

    p->count++;
    p->line_bytes += ev->line_len;
    p->last_field_count = ev->field_count;
    snprintf(p->last_line, sizeof(p->last_line), "%.*s",
             (int)(ev->line_len < sizeof(p->last_line) - 1 ? ev->line_len
                                                           : sizeof(p->last_line) - 1),
             ev->line);
}

static int testcap_batch(hpulogc_sink_t* sink, const hpulogc_event_t* const* evs,
                         size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        testcap_emit(sink, evs[i]);
    }
    return (int)n;
}

static const hpulogc_sink_ops_t g_testcap_ops = {
    "testcap",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC,
    sizeof(testcap_priv_t),
    NULL, /* configure */
    NULL, /* init */
    NULL, /* start */
    testcap_emit,
    testcap_batch,
    NULL, /* flush */
    NULL, /* sync */
    NULL, /* periodic */
    NULL, /* destroy */
    { NULL, NULL, NULL, NULL }
};

/** @brief One-time registration guard (duplicate types are config errors). */
static int register_testcap(void)
{
    static int done = 0;
    int rc = HPULOGC_OK;

    if (!done) {
        rc = hpulogc_sink_register(&g_testcap_ops);
        done = 1;
    }
    return rc;
}

TEST(sink_register_validation)
{
    hpulogc_sink_ops_t bad = g_testcap_ops;
    hpulogc_sink_ops_t noret = g_testcap_ops;
    hpulogc_sink_ops_t rsv = g_testcap_ops;

    CHECK_EQ(register_testcap(), HPULOGC_OK);

    /* duplicate type name */
    CHECK_EQ(hpulogc_sink_register(&g_testcap_ops), HPULOGC_ERR_CONFIG);
    /* bad ABI version */
    bad.type = "testbad_abi";
    bad.abi_version = HPULOGC_SINK_ABI_VERSION + 1;
    CHECK_EQ(hpulogc_sink_register(&bad), HPULOGC_ERR_INVALID_ARG);
    /* neither emit nor emit_batch */
    noret.type = "testbad_noret";
    noret.emit = NULL;
    noret.emit_batch = NULL;
    CHECK_EQ(hpulogc_sink_register(&noret), HPULOGC_ERR_INVALID_ARG);
    /* reserved slots must be NULL */
    rsv.type = "testbad_rsv";
    rsv.reserved[0] = (void (*)(void))testcap_emit;
    CHECK_EQ(hpulogc_sink_register(&rsv), HPULOGC_ERR_INVALID_ARG);
    /* NULL ops */
    CHECK_EQ(hpulogc_sink_register(NULL), HPULOGC_ERR_INVALID_ARG);
}

TEST(sink_custom_end_to_end)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    hpulogc_field_t fields[2];
    static const char* const keys[] = { NULL };
    static const char* const vals[] = { NULL };
    static const char* const names[] = { "cap0" };

    CHECK_EQ(register_testcap(), HPULOGC_OK);

    hpulogc_config_default(&cfg);
    sinks[0].name = "cap0";
    sinks[0].type = "testcap";
    sinks[0].keys = keys;
    sinks[0].vals = vals;
    sinks[0].count = 0;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;

    fields[0].key = "k1";
    fields[0].value.type = HPULOGC_FIELD_I64;
    fields[0].value.v.i64 = 42;
    fields[1].key = "k2";
    fields[1].value.type = HPULOGC_FIELD_STR;
    fields[1].value.v.str.s = "vv";
    fields[1].value.v.str.len = 2;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    HPULOGC_INFO_EX("cap", fields, 2, "hello %d", 7);
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    {
        hpulogc_sink_stats_t st;

        CHECK_EQ(hpulogc_get_sink_stats("cap0", &st), HPULOGC_OK);
        CHECK_EQ(st.written, 1);
        CHECK(st.bytes_written > 0);
        CHECK_EQ(st.dropped, 0);
        CHECK_EQ(st.failed, 0);
    }
    hpulogc_shutdown();
}

TEST(sink_stats_unknown_name)
{
    hpulogc_sink_stats_t st;

    CHECK_EQ(register_testcap(), HPULOGC_OK); /* idempotent guard */
    /* after a register/shutdown cycle the type stays registered */
    CHECK_EQ(hpulogc_sink_register(&g_testcap_ops), HPULOGC_ERR_CONFIG);
    CHECK_EQ(hpulogc_init_default(), HPULOGC_OK);
    /* the built-in stderr console is named "stderr" in the default config */
    CHECK_EQ(hpulogc_get_sink_stats("stderr", &st), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("nope", &st), HPULOGC_ERR_INVALID_ARG);
    CHECK_EQ(hpulogc_get_sink_stats(NULL, &st), HPULOGC_ERR_INVALID_ARG);
    hpulogc_shutdown();
    CHECK_EQ(hpulogc_get_sink_stats("stderr", &st), HPULOGC_ERR_STATE);
}
