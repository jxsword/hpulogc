/* -*- coding: utf-8 -*- */

/**
 * @file hotreload.c
 * @brief Configuration hot reload: watcher thread (platform watcher +
 *        optional signal trigger) and the reload swap (spec 10.5).
 *
 * Reload protocol (decision 17, adapted to the plain conf mutex): the new
 * snapshot is parsed and validated first; the swap (handle move, pointer
 * exchange, generation bump, old-output close) then happens under the
 * conf lock, which empties all readers, so no half-new/half-old state is
 * observable. Any validation failure rolls back cleanly (the fresh
 * snapshot is freed, the old one stays live).
 */

#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HPULOGC_ENABLE_HOT_RELOAD

/** @brief Reload serialization flag (watcher thread vs explicit trigger). */
static hpu_atomic_u32 g_reload_busy;

/**
 * @brief Try to claim the reload slot.
 * @return Non-zero when claimed.
 */
static int reload_try_enter(void)
{
    uint32_t expected = 0;

    return hpu_at_cas_u32(&g_reload_busy, &expected, 1, HPU_MO_ACQ_REL,
                          HPU_MO_ACQUIRE);
}

/**
 * @brief Apply fast-path copies of hot config values (conf lock held).
 */
static void reload_sync_fast_path(hpu_conf_t* c)
{
    hpu_at_store_u32(&g_rt.level_atomic, (uint32_t)c->level, HPU_MO_RELEASE);
    hpu_at_store_u32(&g_rt.ts_source, (uint32_t)c->timestamp_source,
                     HPU_MO_RELEASE);
    hpu_at_store_u32(&g_rt.escape_injection, (uint32_t)c->escape_injection,
                     HPU_MO_RELEASE);
    hpu_at_store_u32(&g_rt.max_log_len_atomic, (uint32_t)c->max_log_length,
                     HPU_MO_RELEASE);
}

int hpu_core_trigger_reload(void)
{
    hpu_conf_t* fresh;
    hpu_conf_t* old;
    int reuse_old_idx[HPULOGC_MAX_OUTPUTS];
    int strict;
    int rc;
    size_t i;

    if (g_rt.state != HPU_RT_RUNNING || g_rt.conf == NULL) {
        return -1;
    }
    if (g_rt.source_path[0] == '\0') {
        return -1; /* code-config instances have nothing to re-read */
    }
    if (!reload_try_enter()) {
        return -1; /* another reload is already in progress */
    }

    fresh = malloc(sizeof(*fresh));
    if (fresh == NULL) {
        hpu_at_store_u32(&g_reload_busy, 0, HPU_MO_RELEASE);
        return -1;
    }
    if (hpu_conf_defaults(fresh) != 0) {
        free(fresh);
        hpu_at_store_u32(&g_reload_busy, 0, HPU_MO_RELEASE);
        return -1;
    }

    hpu_mutex_lock(&g_rt.conf_lock);
    strict = g_rt.conf != NULL ? g_rt.conf->strict_init : 1;
    hpu_mutex_unlock(&g_rt.conf_lock);

    /* Phase 1: parse + validate the new snapshot (fail -> keep old). */
    rc = hpu_conf_load_file(fresh, g_rt.source_path, strict);
    if (rc == 0) {
        rc = hpu_conf_finalize_reload(fresh, g_rt.conf, reuse_old_idx);
    }
    if (rc != 0) {
        fprintf(stderr,
                "hpulogc: reload error: keeping the previous configuration "
                "(%s)\n",
                g_rt.source_path);
        hpu_conf_free(fresh);
        free(fresh);
        hpu_at_store_u32(&g_reload_busy, 0, HPU_MO_RELEASE);
        return -1;
    }

    /* [buffer] changes are ignored (init-time only, spec 10.5). */
    if (fresh->buffer_size != g_rt.conf->buffer_size ||
        fresh->overflow_policy != g_rt.conf->overflow_policy) {
        fprintf(stderr,
                "hpulogc: reload: [buffer] changes are ignored (buffer is "
                "fixed at init)\n");
    }

    /* Phases 2/3 under the conf lock: move reused handles, swap, retire
     * and close the replaced outputs. */
    hpu_mutex_lock(&g_rt.conf_lock);
    old = g_rt.conf;
    for (i = 0; i < fresh->output_count; i++) {
        if (reuse_old_idx[i] >= 0 &&
            (size_t)reuse_old_idx[i] < old->output_count) {
            fresh->outputs[i].handle = old->outputs[reuse_old_idx[i]].handle;
            old->outputs[reuse_old_idx[i]].handle = NULL;
        }
    }
    /* Fold the replaced outputs' lost lines into the retired total. */
    for (i = 0; i < old->output_count; i++) {
        if (old->outputs[i].handle != NULL) {
            hpu_at_fetch_add_u64(&g_rt.st_retired_lost,
                                 hpu_output_lost_total(old->outputs[i].handle),
                                 HPU_MO_ACQ_REL);
        }
    }
    g_rt.conf = fresh;
    hpu_at_fetch_add_u32(&g_rt.conf_gen, 1, HPU_MO_ACQ_REL);
    reload_sync_fast_path(fresh);
    if (fresh->signal_reload && !g_rt.signal_reload_installed) {
        if (hpu_signal_install_hup() == 0) {
            g_rt.signal_reload_installed = 1;
        }
    }
    hpu_conf_free(old);
    free(old);
    hpu_mutex_unlock(&g_rt.conf_lock);

    hpu_at_store_u32(&g_reload_busy, 0, HPU_MO_RELEASE);
    return 0;
}

/**
 * @brief Watcher thread: waits for file changes or the SIGHUP flag and
 *        triggers a reload (spec 10.3/10.5).
 *
 * The wait runs in 200 ms slices so shutdown joins quickly; the poll
 * interval only bounds how often a change can be noticed.
 */
static void watcher_loop(void* arg)
{
    (void)arg;

    for (;;) {
        int interval;
        int interval_ms;
        int waited = 0;
        int changed = 0;

        if (hpu_at_load_u32(&g_rt.watcher_exit, HPU_MO_ACQUIRE) != 0) {
            break;
        }
        hpu_mutex_lock(&g_rt.conf_lock);
        interval = g_rt.conf != NULL ? g_rt.conf->hot_reload_interval : 5;
        hpu_mutex_unlock(&g_rt.conf_lock);

        /* interval == 0 disables polling; the signal trigger stays live
         * on the slow cadence (spec 10.3). */
        interval_ms = interval > 0 ? interval * 1000 : 250;
        while (waited < interval_ms) {
            int step = interval_ms - waited > 200 ? 200 : interval_ms - waited;
            int rc;

            if (hpu_at_load_u32(&g_rt.watcher_exit, HPU_MO_ACQUIRE) != 0) {
                return;
            }
            rc = hpu_watcher_wait(&g_rt.watcher, (uint32_t)step);
            if (rc == 1) {
                changed = 1;
                break;
            }
            if (hpu_signal_take_hup()) {
                changed = 1;
                break;
            }
            waited += step;
        }

        if (hpu_at_load_u32(&g_rt.watcher_exit, HPU_MO_ACQUIRE) != 0) {
            break;
        }
        if (changed) {
            (void)hpu_core_trigger_reload();
        }
    }
}

int hpu_hotreload_start(void)
{
    if (g_rt.watcher_started) {
        return 0;
    }
    if (hpu_watcher_start(&g_rt.watcher, g_rt.source_path) != 0) {
        return -1;
    }
    hpu_at_store_u32(&g_rt.watcher_exit, 0, HPU_MO_RELAXED);
    if (hpu_thread_create(&g_rt.watcher_thread, watcher_loop, NULL) != 0) {
        hpu_watcher_stop(&g_rt.watcher);
        return -1;
    }
    g_rt.watcher_started = 1;
    return 0;
}

void hpu_hotreload_stop(void)
{
    if (!g_rt.watcher_started) {
        return;
    }
    hpu_at_store_u32(&g_rt.watcher_exit, 1, HPU_MO_RELEASE);
    hpu_thread_join(&g_rt.watcher_thread);
    hpu_watcher_stop(&g_rt.watcher);
    g_rt.watcher_started = 0;
}

#endif /* HPULOGC_ENABLE_HOT_RELOAD */
