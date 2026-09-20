/* -*- coding: utf-8 -*- */

/**
 * @file consumer.c
 * @brief Async consumer thread: batched retrieval -> route/format/write,
 *        flush handshake, periodic fsync and periodic stats reporting
 *        (spec 4.4/4.9/7.5/10.3).
 *
 * Batching model: each record is rendered and appended to the outputs'
 * write buffers individually (the file backend coalesces bytes); a
 * physical submit (buffer flush) happens every batch_size records or
 * when the ring runs empty at the flush interval.
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
static void consumer_idle_tasks(consumer_tunables_t* tun, size_t* last_ms)
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

    /* Flush handshake: everything enqueued so far has been submitted. */
    req = hpu_at_load_u64(&g_rt.flush_req, HPU_MO_ACQUIRE);
    hpu_at_store_u64(&g_rt.flush_done, req, HPU_MO_RELEASE);

    if (stats_iv > 0) {
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
}

/**
 * @brief Consumer thread main loop.
 */
static void consumer_loop(void* arg)
{
    hpu_ring_view_t view;
    hpu_log_record_t rec;
    char* staging;
    size_t staging_len;
    consumer_tunables_t tun;
    size_t stats_last_ms;
    size_t processed = 0;

    (void)arg;
    memset(&tun, 0, sizeof(tun));
    tun.batch_size = 64;
    tun.flush_interval_ms = 100;
    stats_last_ms = (size_t)(hpu_now_ns() / 1000000ULL);

    hpu_mutex_lock(&g_rt.conf_lock);
    staging_len = g_rt.ring != NULL ? hpu_ring_capacity(g_rt.ring) : 0;
    hpu_mutex_unlock(&g_rt.conf_lock);

    staging = malloc(staging_len);
    if (staging == NULL) {
        /* Without staging the consumer cannot copy records out; exit and
         * let shutdown reap the thread. */
        hpu_at_store_u32(&g_rt.consumer_alive, 0, HPU_MO_RELEASE);
        return;
    }

    for (;;) {
        int rc;

        if (hpu_at_load_u32(&g_rt.consumer_exit, HPU_MO_ACQUIRE) != 0) {
            break;
        }
        rc = hpu_ring_get(g_rt.ring, &view, staging, staging_len,
                          tun.flush_interval_ms);
        if (rc == HPU_RING_OK) {
            consumer_view_to_rec(&view, &rec);

            hpu_mutex_lock(&g_rt.conf_lock);
            (void)hpu_pipeline_process(&rec);
            hpu_mutex_unlock(&g_rt.conf_lock);

            processed++;
            if (processed >= tun.batch_size) {
                consumer_idle_tasks(&tun, &stats_last_ms);
                processed = 0;
            }
            continue;
        }

        /* Ring empty or timed out: submit the batch + idle bookkeeping. */
        consumer_idle_tasks(&tun, &stats_last_ms);
        processed = 0;
    }

    /* Drain whatever remains (the ring is closed by shutdown). */
    while (hpu_ring_get(g_rt.ring, &view, staging, staging_len, 0) ==
           HPU_RING_OK) {
        consumer_view_to_rec(&view, &rec);
        hpu_mutex_lock(&g_rt.conf_lock);
        (void)hpu_pipeline_process(&rec);
        hpu_mutex_unlock(&g_rt.conf_lock);
    }
    consumer_idle_tasks(&tun, &stats_last_ms);
    free(staging);

    hpu_mutex_lock(&g_rt.consumer_mu);
    hpu_at_store_u32(&g_rt.consumer_alive, 0, HPU_MO_RELEASE);
    hpu_cond_broadcast(&g_rt.consumer_cond);
    hpu_mutex_unlock(&g_rt.consumer_mu);
}

int hpu_consumer_start(void)
{
    if (hpu_mutex_init(&g_rt.consumer_mu) != 0) {
        return -1;
    }
    if (hpu_cond_init(&g_rt.consumer_cond) != 0) {
        hpu_mutex_destroy(&g_rt.consumer_mu);
        return -1;
    }
    hpu_at_store_u32(&g_rt.consumer_exit, 0, HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.consumer_alive, 1, HPU_MO_RELAXED);
    if (hpu_thread_create(&g_rt.consumer_thread, consumer_loop, NULL) != 0) {
        hpu_cond_destroy(&g_rt.consumer_cond);
        hpu_mutex_destroy(&g_rt.consumer_mu);
        hpu_at_store_u32(&g_rt.consumer_alive, 0, HPU_MO_RELAXED);
        return -1;
    }
    return 0;
}

void hpu_consumer_stop(void)
{
    uint32_t timeout_ms;
    uint64_t deadline;

    hpu_mutex_lock(&g_rt.conf_lock);
    timeout_ms = g_rt.conf != NULL ? g_rt.conf->shutdown_timeout_ms : 5000;
    hpu_mutex_unlock(&g_rt.conf_lock);

    hpu_at_store_u32(&g_rt.consumer_exit, 1, HPU_MO_RELEASE);
    if (g_rt.ring != NULL) {
        hpu_ring_kick_consumer(g_rt.ring);
    }

    /* Bounded wait for the consumer to finish draining (spec 7.5). */
    deadline = hpu_now_ns() / 1000000ULL +
               (timeout_ms == 0 ? ~0ULL : (uint64_t)timeout_ms);
    hpu_mutex_lock(&g_rt.consumer_mu);
    while (hpu_at_load_u32(&g_rt.consumer_alive, HPU_MO_ACQUIRE) != 0) {
        uint64_t now = hpu_now_ns() / 1000000ULL;

        if (now >= deadline) {
            break; /* leave the thread; it exits at the next check */
        }
        (void)hpu_cond_timedwait_ms(&g_rt.consumer_cond, &g_rt.consumer_mu,
                                    50);
    }
    hpu_mutex_unlock(&g_rt.consumer_mu);

    hpu_thread_join(&g_rt.consumer_thread);
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
        if (hpu_ring_used(g_rt.ring) == 0 &&
            hpu_at_load_u64(&g_rt.flush_done, HPU_MO_ACQUIRE) >= seq) {
            return 0;
        }
        if (hpu_now_ns() / 1000000ULL >= deadline) {
            return -1; /* best effort: not fully serviced in time */
        }
        hpu_core_sleep_ms(1);
        hpu_ring_kick_consumer(g_rt.ring);
    }
}

#endif /* HPULOGC_ENABLE_ASYNC */
