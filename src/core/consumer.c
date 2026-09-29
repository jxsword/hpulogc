/* -*- coding: utf-8 -*- */

/**
 * @file consumer.c
 * @brief Async consumer thread(s): batched retrieval -> route/format/write,
 *        flush handshake, periodic fsync and periodic stats reporting
 *        (spec 4.4/4.9/7.5/10.3; MPMC extension spec rd_v0.3).
 *
 * Batching model: each record is rendered and appended to the outputs'
 * write buffers individually (the file backend coalesces bytes); a
 * physical submit (buffer flush) happens every batch_size records or
 * when the ring runs empty at the flush interval.
 *
 * MPMC builds run N symmetric consumer threads (configuration key
 * `consumer threads`); every record is claimed by exactly one of them at
 * the ring level. Processing stays serialized under the conf lock, so
 * output content is identical to a single consumer up to inter-record
 * ordering (rd_v0.3 1.1).
 */

#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HPULOGC_ENABLE_ASYNC

/**
 * @brief Idle-path tunables refreshed under the conf lock in idle tasks
 *        so hot-reloaded [async] values take effect promptly (spec 10.5).
 */
typedef struct consumer_tunables {
    uint32_t batch_size;        /*!< Records per physical submit */
    uint32_t flush_interval_ms; /*!< Force-submit interval */
    uint32_t stats_interval;    /*!< Stats report interval (0 = off) */
    int      stats_file_mode;   /*!< 0 = stderr, 1 = stats file */
    char     stats_file[HPULOGC_MAX_PATH_LEN];
} consumer_tunables_t;

/**
 * @brief Per-consumer-thread context (one element per consumer thread).
 */
typedef struct hpu_consumer_ctx {
    uint32_t idx;      /*!< Consumer slot (index into g_rt arrays) */
    char*    staging;  /*!< Private record staging buffer */
    size_t   staging_len; /*!< Staging size in bytes */
} hpu_consumer_ctx_t;

/** @brief Consumer thread table (consumer_count elements, start..stop). */
static hpu_consumer_ctx_t* g_consumer_ctx = NULL;

/* Sleep uses the shared hpu_core_sleep_ms() helper (core.c). */

/**
 * @brief Print one stats report line (format is an implementation choice,
 *        recorded in docs/implementation_notes.md).
 */
static void consumer_stats_report(const consumer_tunables_t* tun)
{
    hpulogc_stats_t st;
    FILE* out = stderr;

    if (hpulogc_get_stats(&st) != HPULOGC_OK) {
        return;
    }
    if (tun->stats_file_mode && tun->stats_file[0] != '\0') {
        out = fopen(tun->stats_file, "a");
        if (out == NULL) {
            return;
        }
    }
    fprintf(out,
            "[hpulogc][stats] accepted=%llu written=%llu dropped=%llu "
            "overwritten=%llu throttled=%llu buffer_used=%zu/%zu\n",
            (unsigned long long)st.accepted, (unsigned long long)st.written,
            (unsigned long long)st.dropped,
            (unsigned long long)st.overwritten,
            (unsigned long long)st.throttled, st.buffer_used,
            st.buffer_size);
    if (out != stderr) {
        fclose(out);
    }
}

/**
 * @brief Submit batched output bytes and run idle bookkeeping: buffer
 *        flush, periodic fsync, tunable refresh and the flush handshake.
 *
 * Also refreshes @p tun from the active snapshot so hot-reloaded [async]
 * and stats values apply from the next idle cycle on.
 */
static void consumer_idle_tasks(hpu_consumer_ctx_t* ctx,
                                consumer_tunables_t* tun, size_t* last_ms)
{
    uint64_t now_ns = hpu_now_ns();
    uint64_t req;
    uint32_t stats_iv;
    size_t i;

    hpu_mutex_lock(&g_rt.conf_lock);
    if (g_rt.conf == NULL) {
        hpu_mutex_unlock(&g_rt.conf_lock);
        return;
    }
    for (i = 0; i < g_rt.conf->output_count; i++) {
        hpu_output_t* o = g_rt.conf->outputs[i].handle;

        if (o == NULL) {
            continue;
        }
        (void)hpu_output_flush(o);
        (void)hpu_output_periodic(o, now_ns,
                                  (uint64_t)g_rt.conf->flush_interval_ms *
                                      1000000ULL);
    }
    tun->batch_size = g_rt.conf->batch_size > 0 ? g_rt.conf->batch_size : 64;
    tun->flush_interval_ms = g_rt.conf->flush_interval_ms > 0
                                 ? g_rt.conf->flush_interval_ms
                                 : 100;
    stats_iv = (uint32_t)g_rt.conf->stats_interval;
    tun->stats_file_mode = g_rt.conf->stats_output_file;
    memcpy(tun->stats_file, g_rt.conf->stats_file, sizeof(tun->stats_file));
    hpu_mutex_unlock(&g_rt.conf_lock);

    /* Flush handshake (defect P-6 fix): the ack must certify "this
     * consumer's hand is empty", not merely "this consumer ran its idle
     * tasks". idle_tasks runs after processing, so combining the
     * ring-empty observation with the ack means: every record this
     * consumer claimed has been dispatched (its written counter bumped)
     * before the ack. Without the guard, a consumer that claimed the
     * last record (head advanced, ring now empty) and was preempted
     * before processing let the flusher — seeing ring-empty plus acks
     * from the OTHER consumers, whose ack counters permanently satisfy
     * the current seq — return early while records were still in
     * flight (observed as written < accepted with dropped == 0 under
     * CPU congestion; MPMC only: with a single consumer the acking
     * consumer is the claiming consumer, so the window cannot open). */
    req = hpu_at_load_u64(&g_rt.flush_req, HPU_MO_ACQUIRE);
    if (g_rt.ring != NULL && hpu_ring_used(g_rt.ring) == 0) {
        hpu_at_store_u64(&g_rt.flush_ack[ctx->idx], req, HPU_MO_RELEASE);
    }

    if (stats_iv > 0 && ctx->idx == 0) {
        /* One stats report per interval, emitted by the first consumer
         * only (the report aggregates global counters). */
        uint64_t now_ms = now_ns / 1000000ULL;

        if (now_ms >= *last_ms &&
            now_ms - *last_ms >= (uint64_t)stats_iv * 1000ULL) {
            consumer_stats_report(tun);
            *last_ms = (size_t)now_ms;
        }
    }
}

/**
 * @brief Build a log record from a ring view (header + payload pointers).
 */
static void consumer_view_to_rec(const hpu_ring_view_t* view,
                                 hpu_log_record_t* rec)
{
    const hpu_ring_meta_t* m = &view->meta;

    memset(rec, 0, sizeof(*rec));
    rec->level = HPU_REC_LEVEL(m->level_flags);
    rec->category = view->category;
    rec->category_len = m->category_len;
    rec->file = view->file;
    rec->file_len = m->file_len;
    rec->func = view->func;
    rec->func_len = m->func_len;
    rec->line = (int)m->line;
    rec->msg = view->msg;
    rec->msg_len = m->msg_len;
    rec->realtime_ns = m->realtime_ns;
    rec->mono_us = m->mono_us;
    rec->tid = m->tid;
    rec->fields_wire = view->fields_wire;
    rec->fields_len = (uint32_t)view->fields_len;
    rec->field_count = m->field_count;
}

/**
 * @brief Consumer thread main loop (one instance per consumer thread).
 */
static void consumer_loop(void* arg)
{
    hpu_consumer_ctx_t* ctx = arg;
    hpu_ring_view_t view;
    hpu_log_record_t rec;
    consumer_tunables_t tun;
    size_t stats_last_ms;
    size_t processed = 0;

    memset(&tun, 0, sizeof(tun));
    tun.batch_size = 64;
    tun.flush_interval_ms = 100;
    stats_last_ms = (size_t)(hpu_now_ns() / 1000000ULL);

    hpu_at_fetch_add_u32(&g_rt.consumer_alive, 1, HPU_MO_ACQ_REL);

    hpu_mutex_lock(&g_rt.conf_lock);
    ctx->staging_len = g_rt.ring != NULL ? hpu_ring_capacity(g_rt.ring) : 0;
    hpu_mutex_unlock(&g_rt.conf_lock);

    ctx->staging = malloc(ctx->staging_len);
    if (ctx->staging == NULL) {
        /* Without staging the consumer cannot copy records out; exit and
         * let shutdown reap the thread. */
        goto exit_thread;
    }

    for (;;) {
        int rc;

        if (hpu_at_load_u32(&g_rt.consumer_exit, HPU_MO_ACQUIRE) != 0) {
            break;
        }
        rc = hpu_ring_get(g_rt.ring, &view, ctx->staging, ctx->staging_len,
                          tun.flush_interval_ms);
        if (rc == HPU_RING_OK) {
            consumer_view_to_rec(&view, &rec);

            hpu_mutex_lock(&g_rt.conf_lock);
            (void)hpu_pipeline_process(&rec);
            hpu_mutex_unlock(&g_rt.conf_lock);

            processed++;
            if (processed >= tun.batch_size) {
                consumer_idle_tasks(ctx, &tun, &stats_last_ms);
                processed = 0;
            }
            continue;
        }

        /* Ring empty or timed out: submit the batch + idle bookkeeping. */
        consumer_idle_tasks(ctx, &tun, &stats_last_ms);
        processed = 0;
    }

    /* Drain whatever remains (the ring is closed by shutdown). Records
     * are distributed among the consumers by the ring's claim protocol. */
    while (hpu_ring_get(g_rt.ring, &view, ctx->staging, ctx->staging_len,
                        0) == HPU_RING_OK) {
        consumer_view_to_rec(&view, &rec);
        hpu_mutex_lock(&g_rt.conf_lock);
        (void)hpu_pipeline_process(&rec);
        hpu_mutex_unlock(&g_rt.conf_lock);
    }
    consumer_idle_tasks(ctx, &tun, &stats_last_ms);
    free(ctx->staging);
    ctx->staging = NULL;

exit_thread:
    hpu_mutex_lock(&g_rt.consumer_mu);
    hpu_at_fetch_sub_u32(&g_rt.consumer_alive, 1, HPU_MO_ACQ_REL);
    hpu_cond_broadcast(&g_rt.consumer_cond);
    hpu_mutex_unlock(&g_rt.consumer_mu);
}

/**
 * @brief Read the configured consumer thread count (1 when unset).
 */
static uint32_t consumer_thread_count(void)
{
    uint32_t count;

    hpu_mutex_lock(&g_rt.conf_lock);
    count = (g_rt.conf != NULL && g_rt.conf->consumer_threads > 0)
                ? g_rt.conf->consumer_threads
                : 1;
    hpu_mutex_unlock(&g_rt.conf_lock);
    return count;
}

int hpu_consumer_start(void)
{
    uint32_t count = consumer_thread_count();
    uint32_t started;
    uint64_t deadline;

    if (hpu_mutex_init(&g_rt.consumer_mu) != 0) {
        return -1;
    }
    if (hpu_cond_init(&g_rt.consumer_cond) != 0) {
        hpu_mutex_destroy(&g_rt.consumer_mu);
        return -1;
    }
    g_rt.consumer_threads = calloc(count, sizeof(hpu_thread_t));
    g_rt.flush_ack = calloc(count, sizeof(hpu_atomic_u64));
    g_consumer_ctx = calloc(count, sizeof(hpu_consumer_ctx_t));
    if (g_rt.consumer_threads == NULL || g_rt.flush_ack == NULL ||
        g_consumer_ctx == NULL) {
        goto fail_alloc;
    }
    g_rt.consumer_count = count;
    hpu_at_store_u32(&g_rt.consumer_exit, 0, HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.consumer_alive, 0, HPU_MO_RELAXED);

    for (started = 0; started < count; started++) {
        g_consumer_ctx[started].idx = started;
        g_consumer_ctx[started].staging = NULL;
        g_consumer_ctx[started].staging_len = 0;
        if (hpu_thread_create(&g_rt.consumer_threads[started], consumer_loop,
                              &g_consumer_ctx[started]) != 0) {
            goto fail_threads;
        }
    }
    return 0;

fail_threads:
    /* Ask whatever already started to leave, then reap it. */
    hpu_at_store_u32(&g_rt.consumer_exit, 1, HPU_MO_RELEASE);
    if (g_rt.ring != NULL) {
        hpu_ring_kick_consumer(g_rt.ring);
    }
    deadline = hpu_now_ns() / 1000000ULL + 5000;
    hpu_mutex_lock(&g_rt.consumer_mu);
    while (hpu_at_load_u32(&g_rt.consumer_alive, HPU_MO_ACQUIRE) != 0) {
        uint64_t now = hpu_now_ns() / 1000000ULL;

        if (now >= deadline) {
            break;
        }
        (void)hpu_cond_timedwait_ms(&g_rt.consumer_cond, &g_rt.consumer_mu,
                                    50);
    }
    hpu_mutex_unlock(&g_rt.consumer_mu);

fail_alloc:
    {
        uint32_t i;

        for (i = 0; i < g_rt.consumer_count; i++) {
            hpu_thread_join(&g_rt.consumer_threads[i]);
        }
    }
    free(g_rt.consumer_threads);
    g_rt.consumer_threads = NULL;
    free(g_rt.flush_ack);
    g_rt.flush_ack = NULL;
    free(g_consumer_ctx);
    g_consumer_ctx = NULL;
    g_rt.consumer_count = 0;
    hpu_cond_destroy(&g_rt.consumer_cond);
    hpu_mutex_destroy(&g_rt.consumer_mu);
    return -1;
}

void hpu_consumer_stop(void)
{
    uint32_t timeout_ms;
    uint64_t deadline;
    uint32_t i;

    hpu_mutex_lock(&g_rt.conf_lock);
    timeout_ms = g_rt.conf != NULL ? g_rt.conf->shutdown_timeout_ms : 5000;
    hpu_mutex_unlock(&g_rt.conf_lock);

    hpu_at_store_u32(&g_rt.consumer_exit, 1, HPU_MO_RELEASE);
    if (g_rt.ring != NULL) {
        hpu_ring_kick_consumer(g_rt.ring);
    }

    /* Bounded wait for every consumer to finish draining (spec 7.5). */
    deadline = hpu_now_ns() / 1000000ULL +
               (timeout_ms == 0 ? ~0ULL : (uint64_t)timeout_ms);
    hpu_mutex_lock(&g_rt.consumer_mu);
    while (hpu_at_load_u32(&g_rt.consumer_alive, HPU_MO_ACQUIRE) != 0) {
        uint64_t now = hpu_now_ns() / 1000000ULL;

        if (now >= deadline) {
            break; /* leave the threads; they exit at the next check */
        }
        (void)hpu_cond_timedwait_ms(&g_rt.consumer_cond, &g_rt.consumer_mu,
                                    50);
    }
    hpu_mutex_unlock(&g_rt.consumer_mu);

    for (i = 0; i < g_rt.consumer_count; i++) {
        hpu_thread_join(&g_rt.consumer_threads[i]);
    }
    free(g_rt.consumer_threads);
    g_rt.consumer_threads = NULL;
    free(g_rt.flush_ack);
    g_rt.flush_ack = NULL;
    free(g_consumer_ctx);
    g_consumer_ctx = NULL;
    g_rt.consumer_count = 0;
    hpu_cond_destroy(&g_rt.consumer_cond);
    hpu_mutex_destroy(&g_rt.consumer_mu);
}

int hpu_consumer_flush(void)
{
    uint64_t seq;
    uint64_t deadline;
    uint32_t timeout_ms;

    if (g_rt.ring == NULL) {
        return 0;
    }
    seq = hpu_at_fetch_add_u64(&g_rt.flush_req, 1, HPU_MO_ACQ_REL) + 1;
    hpu_ring_kick_consumer(g_rt.ring);

    /* The flush wait bound follows the configured shutdown timeout so a
     * congested machine (CI runners under parallel load) cannot make a
     * healthy pipeline look like it failed to drain. */
    hpu_mutex_lock(&g_rt.conf_lock);
    timeout_ms = g_rt.conf != NULL && g_rt.conf->shutdown_timeout_ms > 0
                     ? g_rt.conf->shutdown_timeout_ms
                     : 5000;
    hpu_mutex_unlock(&g_rt.conf_lock);
    deadline = hpu_now_ns() / 1000000ULL + (uint64_t)timeout_ms;
    for (;;) {
        if (hpu_ring_used(g_rt.ring) == 0) {
            /* Every consumer must have run its idle tasks at least once
             * after the request (per-consumer ack counters). */
            uint32_t acked = 1;
            uint32_t i;

            for (i = 0; i < g_rt.consumer_count; i++) {
                if (hpu_at_load_u64(&g_rt.flush_ack[i], HPU_MO_ACQUIRE) <
                    seq) {
                    acked = 0;
                    break;
                }
            }
            if (acked) {
                return 0;
            }
        }
        if (hpu_now_ns() / 1000000ULL >= deadline) {
            return -1; /* best effort: not fully serviced in time */
        }
        hpu_core_sleep_ms(1);
        hpu_ring_kick_consumer(g_rt.ring);
    }
}

#endif /* HPULOGC_ENABLE_ASYNC */
