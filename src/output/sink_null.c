/**
 * @file sink_null.c
 * @brief null sink: drops everything (testing/baseline; the routing target
 *        for retired audit/metric categories, rd_v0.6 §4.7.1).
 *
 * Delivery is counted as written by the core's deliver path; the sink has
 * no buffering, no failure modes and no private configuration keys.
 */

#include "output.h"

/**
 * @brief The null sink discards the event; delivery counting stays in the
 *        core's deliver path (§4.10.3).
 */
static void null_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    (void)sink;
    (void)ev;
}

/** @brief null sink type (SYNC|ASYNC; no private data, no buffering). */
static const hpulogc_sink_ops_t g_null_ops = {
    "null",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC,
    0, /* priv_size */
    NULL, /* configure */
    NULL, /* init */
    NULL, /* start */
    null_emit,
    NULL, /* emit_batch */
    NULL, /* flush */
    NULL, /* sync */
    NULL, /* periodic */
    NULL, /* destroy */
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_null_sink_ops(void)
{
    return &g_null_ops;
}
