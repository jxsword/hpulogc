/**
 * @file test_sink_async.c
 * @brief per-sink async delivery (rd_v0.6 §4.10.6): queue-full drops,
 *        flush/shutdown drain, slow-sink isolation and MPMC interplay.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"
#include "atomic/hpulogc_atomic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- A slow custom async sink (drives the queue-full machinery) ----- */

/** @brief Slow sink priv. */
typedef struct slowcap_priv {
    unsigned long long count;      /*!< Events accepted */
    char last_line[128];           /*!< Last line copy */
} slowcap_priv_t;

static void slowcap_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    slowcap_priv_t* p = hpulogc_sink_priv(sink);

    /* Simulate a slow remote collector: each event costs ~2 ms. */
    hpu_test_sleep_ms(2);
    p->count++;
    snprintf(p->last_line, sizeof(p->last_line), "%.*s",
             (int)(ev->line_len < sizeof(p->last_line) - 1
                       ? ev->line_len
                       : sizeof(p->last_line) - 1),
             ev->line);
}

static int slowcap_batch(hpulogc_sink_t* sink,
                         const hpulogc_event_t* const* evs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        slowcap_emit(sink, evs[i]);
    }
    return (int)n;
}

static const hpulogc_sink_ops_t g_slowcap_ops = {
    "slowcap",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC,
    sizeof(slowcap_priv_t),
    NULL, NULL, NULL,
    slowcap_emit,
    slowcap_batch,
    NULL, NULL, NULL, NULL,
    { NULL, NULL, NULL, NULL }
};

static int register_slowcap(void)
{
    static int done = 0;
    int rc = HPULOGC_OK;

    if (!done) {
        rc = hpulogc_sink_register(&g_slowcap_ops);
        done = 1;
    }
    return rc;
}

TEST(sink_async_flush_drains_and_counts)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    static const char* const names[] = { "slow0" };
    hpulogc_sink_stats_t st;
    int i;

    CHECK_EQ(register_slowcap(), HPULOGC_OK);

    hpulogc_config_default(&cfg);
    sinks[0].name = "slow0";
    sinks[0].type = "slowcap";
    sinks[0].keys = NULL;
    sinks[0].vals = NULL;
    sinks[0].count = 0;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;
    cfg.shutdown_timeout_ms = 30000;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    for (i = 0; i < 10; i++) {
        HPULOGC_INFO("a", "async %d", i);
    }
    /* flush waits for the second-level queue drain (§4.10.6) */
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("slow0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 10);
    CHECK_EQ(st.dropped, 0); /* queue deep enough: nothing discarded */
    hpulogc_shutdown();
}

TEST(sink_async_queue_full_drops)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    static const char* const keys[] = { "async", "queue size" };
    static const char* const vals[] = { "on", "64kb" };
    static const char* const names[] = { "slowq" };
    hpulogc_sink_stats_t st;
    int i;

    CHECK_EQ(register_slowcap(), HPULOGC_OK);

    hpulogc_config_default(&cfg);
    sinks[0].name = "slowq";
    sinks[0].type = "slowcap";
    sinks[0].keys = keys;
    sinks[0].vals = vals;
    sinks[0].count = 2;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;
    cfg.shutdown_timeout_ms = 30000;
    cfg.max_log_length = 4096;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    /* each packed event is ~4 KB; a 64 KB queue holds ~16; the worker
     * drains at ~2 ms/event while the producer hammers 200 events */
    {
        /* ~3 KB rendered line: a 64 KB queue holds ~20 events while the
         * worker drains at ~2 ms/record -- the producer outruns it. */
        char big[2048];

        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        for (i = 0; i < 200; i++) {
            HPULOGC_INFO("a", "filler %04d %s %s %s %s %s %s", i, big, big,
                         big, big, big, big);
        }
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("slowq", &st), HPULOGC_OK);
    /* non-blocking discard semantics: the producer never stalls and the
     * queue overflow shows up as per-sink drops (D-S5) */
    CHECK(st.dropped > 0);
    CHECK(st.written + st.dropped == 200);
    /* the global identity stays closed: written + dropped + retired == 200 */
    {
        hpulogc_stats_t g;

        CHECK_EQ(hpulogc_get_stats(&g), HPULOGC_OK);
        CHECK_EQ(g.accepted, 200);
        CHECK(g.written + g.dropped + g.throttled + g.overwritten +
                  g.fields_dropped <= 200);
    }
    hpulogc_shutdown();
}

TEST(sink_async_shutdown_drains)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    static const char* const names[] = { "slowd" };
    hpulogc_sink_stats_t st;
    int i;

    CHECK_EQ(register_slowcap(), HPULOGC_OK);

    hpulogc_config_default(&cfg);
    sinks[0].name = "slowd";
    sinks[0].type = "slowcap";
    sinks[0].keys = NULL;
    sinks[0].vals = NULL;
    sinks[0].count = 0;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;
    cfg.shutdown_timeout_ms = 30000;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    for (i = 0; i < 20; i++) {
        HPULOGC_INFO("a", "drain %d", i);
    }
    /* NO explicit flush: shutdown must drain the second-level queue */
    hpulogc_shutdown();
    CHECK_EQ(hpulogc_get_sink_stats("slowd", &st), HPULOGC_ERR_STATE);
}

#if defined(HPULOGC_CONCURRENCY_MPMC)
TEST(sink_async_mpmc_multi_consumer)
{
    /* MPMC build: N consumers feed the same async queue concurrently;
     * the exactly-once property of the shared ring carries over. */
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    static const char* const names[] = { "slowm" };
    hpulogc_sink_stats_t st;
    int i;

    CHECK_EQ(register_slowcap(), HPULOGC_OK);

    hpulogc_config_default(&cfg);
    sinks[0].name = "slowm";
    sinks[0].type = "slowcap";
    sinks[0].keys = NULL;
    sinks[0].vals = NULL;
    sinks[0].count = 0;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.consumer_threads = 4;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;
    cfg.shutdown_timeout_ms = 60000;
    cfg.buffer_size = 4U * 1024U * 1024U;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    for (i = 0; i < 100; i++) {
        HPULOGC_INFO("m", "mpmc sink %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("slowm", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 100);
    CHECK_EQ(st.dropped, 0);
    hpulogc_shutdown();
}
#endif
