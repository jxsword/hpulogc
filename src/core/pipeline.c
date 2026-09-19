/* -*- coding: utf-8 -*- */

/**
 * @file pipeline.c
 * @brief Producer-side pipeline (spec 4.9 steps 2-4) and the shared
 *        route/format/write path used by sync mode and the consumer
 *        (steps 6-8), plus the TLS render buffers and the throttle.
 *
 * The per-thread render storage is a TLS slot with a destructor (spec 9
 * memory rules: lazily allocated, grows to max_log_length, freed at
 * thread exit).
 */

#include "core_internal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Selector matching (spec 10.3 rules semantics)                       */
/* ------------------------------------------------------------------ */

int hpu_pipeline_selector_match(const char* rule_sel, const char* cat,
                                size_t cat_len)
{
    size_t sel_len = strlen(rule_sel);

    if (sel_len == 0) {
        return 1; /* empty selector behaves like "*" */
    }
    if (sel_len == 1 && rule_sel[0] == '*') {
        return 1;
    }
    if (sel_len >= 2 && rule_sel[sel_len - 2] == '.' &&
        rule_sel[sel_len - 1] == '*') {
        /* "name.*" matches name itself and any child hierarchy. */
        size_t prefix = sel_len - 2;

        if (cat_len < prefix || memcmp(rule_sel, cat, prefix) != 0) {
            return 0;
        }
        return cat_len == prefix || cat[prefix] == '.';
    }
    /* Exact match. */
    return sel_len == cat_len && memcmp(rule_sel, cat, cat_len) == 0;
}

/**
 * @brief Map a raw category (NULL/"" equals "*") to its match form.
 */
static const char* cat_match_form(const char* cat, size_t len)
{
    return (cat != NULL && len > 0) ? cat : "*";
}

/* ------------------------------------------------------------------ */
/* TLS render buffers                                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Per-thread render storage (message body + full line + time cache).
 */
typedef struct hpu_render_tls {
    char*            msg_buf;  /*!< Message body render buffer */
    size_t           msg_cap;  /*!< Message buffer capacity */
    char*            line_buf; /*!< Full-line render buffer */
    size_t           line_cap; /*!< Line buffer capacity */
    hpu_fmt_cache_t  cache;    /*!< Per-thread time rendering cache */
} hpu_render_tls_t;

static hpu_tls_key_t g_tls_key;
static int g_tls_ready;

/**
 * @brief TLS destructor: release the thread's render storage.
 */
static void render_tls_dtor(void* value)
{
    hpu_render_tls_t* t = value;

    if (t != NULL) {
        free(t->msg_buf);
        free(t->line_buf);
        free(t);
    }
}

int hpu_pipeline_tls_init(void)
{
    if (g_tls_ready) {
        return 0;
    }
    if (hpu_tls_create(&g_tls_key, render_tls_dtor) != 0) {
        return -1;
    }
    g_tls_ready = 1;
    return 0;
}

void hpu_pipeline_tls_shutdown(void)
{
    hpu_render_tls_t* t;

    if (!g_tls_ready) {
        return;
    }
    /* Free the calling thread's buffer explicitly: TLS destructors do
     * not run for the owning thread on key deletion. */
    t = hpu_tls_get(&g_tls_key);
    if (t != NULL) {
        hpu_tls_set(&g_tls_key, NULL);
        render_tls_dtor(t);
    }
    hpu_tls_destroy(&g_tls_key);
    g_tls_ready = 0;
}

/**
 * @brief Get the caller's render storage, allocating on first use.
 */
static hpu_render_tls_t* render_tls_get(void)
{
    hpu_render_tls_t* t = hpu_tls_get(&g_tls_key);

    if (t != NULL) {
        return t;
    }
    t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    hpu_fmt_cache_init(&t->cache);
    if (hpu_tls_set(&g_tls_key, t) != 0) {
        render_tls_dtor(t);
        return NULL;
    }
    return t;
}

/**
 * @brief Ensure a TLS buffer holds at least @p need bytes.
 * @return Buffer pointer or NULL on allocation failure.
 */
static char* tls_ensure(char** buf, size_t* cap, size_t need)
{
    char* grown;

    if (*buf != NULL && *cap >= need) {
        return *buf;
    }
    grown = realloc(*buf, need);
    if (grown == NULL) {
        return NULL;
    }
    *buf = grown;
    *cap = need;
    return grown;
}

/* ------------------------------------------------------------------ */
/* Throttle (HPULOGC_ENABLE_THROTTLE builds; spec 4.9 step 3)          */
/* ------------------------------------------------------------------ */

#if HPULOGC_ENABLE_THROTTLE
/**
 * @brief Refill a token bucket at the configured rate.
 * @return Non-zero when a token was consumed successfully.
 */
static int throttle_bucket(double* tokens, int64_t* last_ns, long rate,
                           long burst)
{
    int64_t now = (int64_t)hpu_now_ns();
    int64_t last = *last_ns;

    if (rate <= 0) {
        return 1; /* 0 = unlimited (spec 10.3) */
    }
    if (last == 0) {
        *last_ns = now;
        *tokens = (double)burst;
    } else if (now > last) {
        *tokens += (double)rate * (double)(now - last) / 1e9;
        if (*tokens > (double)burst) {
            *tokens = (double)burst;
        }
        *last_ns = now;
    }
    if (*tokens >= 1.0) {
        *tokens -= 1.0;
        return 1;
    }
    return 0;
}

/**
 * @brief Deterministic 1/N sampling over a global counter (spec 10.3).
 */
static hpu_atomic_u32 g_sample_seq;

/**
 * @brief Apply the throttle/sampling pipeline (global bucket, category
 *        bucket, then sampling).
 * @return Non-zero when the record may proceed.
 */
static int throttle_pass(hpu_reg_entry_t* slot, const char* match_cat,
                         size_t match_len)
{
    hpu_conf_t* conf = g_rt.conf;
    hpu_throttle_cfg_t* th;
    double tokens = 0;
    int64_t last = 0;
    int pass;

    if (conf == NULL) {
        return 1;
    }
    th = &conf->throttle;

    if (th->global_rate > 0) {
        if (!throttle_bucket(&tokens, &last, th->global_rate, th->burst)) {
            return 0;
        }
    }
    if (th->per_category_rate > 0) {
        if (slot == NULL) {
            slot = hpu_registry_lookup(match_cat, match_len);
        }
        if (slot != NULL) {
            /* Per-category buckets are guarded by the registry mutex;
             * throttle is opt-in and never on the hot default path. */
            hpu_mutex_lock(&g_reg.mu);
            pass = throttle_bucket(&slot->tokens, &slot->last_refill_ns,
                                   th->per_category_rate, th->burst);
            hpu_mutex_unlock(&g_reg.mu);
            if (!pass) {
                return 0;
            }
        }
    }
    if (th->sampling_rate <= 0.0) {
        return 0; /* 0.0 = drop everything */
    }
    if (th->sampling_n > 1) {
        uint32_t v = hpu_at_fetch_add_u32(&g_sample_seq, 1, HPU_MO_RELAXED);

        if ((v % (uint32_t)th->sampling_n) != 0) {
            return 0;
        }
    }
    return 1;
}
#endif /* HPULOGC_ENABLE_THROTTLE */

/* ------------------------------------------------------------------ */
/* Shared route + render + write path (steps 6-8)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Route one record to its rule index (first match, spec 4.9).
 *
 * Uses the registry per-level cache when the category is registered;
 * otherwise scans the rules in order. CATEGORY=OFF builds ignore the
 * selector content (spec 4.8).
 *
 * @param conf      Active configuration snapshot.
 * @param level     Record level.
 * @param cat       Raw category bytes (NULL/empty matches "*").
 * @param cat_len   Raw category byte count.
 * @return          Rule index or -1 when no rule matched.
 */
static int route_lookup(const hpu_conf_t* conf, int level, const char* cat,
                        size_t cat_len)
{
    int idx = -1;
#if !HPULOGC_ENABLE_CATEGORY
    (void)cat; /* CATEGORY=OFF: selectors are treated as "*" (spec 4.8) */
    (void)cat_len;
#endif
#if HPULOGC_ENABLE_CATEGORY
    const char* match_cat = cat_match_form(cat, cat_len);
    size_t match_len = (cat != NULL && cat_len > 0) ? cat_len : 1;
    hpu_reg_entry_t* slot;
    uint32_t gen = hpu_at_load_u32(&g_rt.conf_gen, HPU_MO_ACQUIRE);

    slot = hpu_registry_find(match_cat, match_len);
    if (slot != NULL) {
        hpu_registry_refresh_route(slot, conf, gen);
        return slot->rule_for_level[level];
    }
#endif
    {
        size_t i;

        for (i = 0; i < conf->rule_count; i++) {
            const hpu_conf_rule_t* r = &conf->rules[i];

            if (r->min_level <= level && level <= r->max_level) {
#if HPULOGC_ENABLE_CATEGORY
                if (!hpu_pipeline_selector_match(r->category, match_cat,
                                                 match_len)) {
                    continue;
                }
#endif
                idx = (int)i;
                break;
            }
        }
    }
    return idx;
}

int hpu_pipeline_process(const hpu_log_record_t* rec)
{
    hpu_conf_t* conf = g_rt.conf;
    const hpu_format_t* fmt;
    const int* out_idx;
    size_t out_count;
    int rule_idx;
    hpu_render_tls_t* tls;
    char* line;
    size_t line_len = 0;
    int written = 0;
    size_t k;

    if (conf == NULL) {
        return -1;
    }

    rule_idx = route_lookup(conf, rec->level, rec->category,
                            rec->category_len);
    if (rule_idx >= 0) {
        const hpu_conf_rule_t* rule = &conf->rules[rule_idx];

        fmt = hpu_conf_find_format(conf, rule->format_name);
        out_idx = rule->output_idx;
        out_count = rule->output_count;
    } else {
        /* Fallback: default format + default outputs (spec 4.9 step 6);
         * empty default outputs drop the record. */
        fmt = hpu_conf_find_format(conf, conf->default_format);
        out_idx = conf->default_output_idx;
        out_count = conf->default_output_count;
    }
    if (fmt == NULL) {
        return -1;
    }

    tls = render_tls_get();
    if (tls == NULL) {
        return -1;
    }
    line = tls_ensure(&tls->line_buf, &tls->line_cap,
                      conf->max_log_length + 1);
    if (line == NULL) {
        return -1;
    }

    if (hpu_format_render(fmt, rec, &conf->env, &tls->cache,
                          conf->escape_injection, conf->max_log_length,
                          conf->truncation_marker, line,
                          conf->max_log_length + 1, &line_len) != 0) {
        return -1;
    }

    for (k = 0; k < out_count; k++) {
        size_t i = (size_t)out_idx[k];

        if (i < conf->output_count && conf->outputs[i].handle != NULL) {
            if (hpu_output_write_line(conf->outputs[i].handle, line,
                                      line_len,
                                      rec->realtime_ns / 1000000000LL,
                                      rec->level) == 0) {
                written = 1;
            }
        }
    }

    if (written) {
        /* Entered at least one output buffer (decision 16); flush-time
         * failures migrate into dropped via the lost counters. */
        hpu_at_fetch_add_u64(&g_rt.st_written, 1, HPU_MO_ACQ_REL);
    } else {
        /* Routed to zero outputs (or format missing): discarded; counted
         * through the retired-lost channel so the stats identity stays
         * closed (decision 21). */
        hpu_at_fetch_add_u64(&g_rt.st_retired_lost, 1, HPU_MO_ACQ_REL);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Producer entry (steps 2-4)                                          */
/* ------------------------------------------------------------------ */

void hpu_pipeline_submit(hpulogc_level_t level, const char* category,
                         const char* file, int line, const char* func,
                         const char* fmt, va_list ap)
{
    int saved_errno = errno;
    const char* cat = category; /* raw: NULL/"" renders as empty */
    size_t cat_len = cat != NULL ? strlen(cat) : 0;
#if HPULOGC_ENABLE_CATEGORY || HPULOGC_ENABLE_THROTTLE
    const char* match_cat = cat_match_form(cat, cat_len);
    size_t match_len = (cat != NULL && cat_len > 0) ? cat_len : 1;
#endif
#if HPULOGC_ENABLE_CATEGORY
    hpu_reg_entry_t* slot = NULL;
#endif
    uint32_t threshold;
    hpu_render_tls_t* tls;
    va_list ap_copy;
    char* msg;
    size_t max_len;
    int n;
    size_t msg_len;
    uint64_t tid;
    int64_t realtime_ns;
    int64_t mono_us = 0;
#if HPULOGC_ENABLE_ASYNC
    hpu_ring_msg_t ring_msg;
#endif
    hpu_log_record_t rec;

    if (g_rt.state != HPU_RT_RUNNING) {
        errno = saved_errno;
        return; /* uninitialized/after shutdown: silent drop (spec 7.5) */
    }
    if (level < HPULOGC_LEVEL_TRACE || level > HPULOGC_LEVEL_FATAL) {
        errno = saved_errno;
        return; /* OFF or invalid as a record level: silent drop */
    }
    if (hpu_core_fork_check() != 0) {
        errno = saved_errno;
        return; /* fork child with disable/failed reinit: drop */
    }

    /* Step 2: level filter (per-category override wins, decision 19). */
    threshold = hpu_at_load_u32(&g_rt.level_atomic, HPU_MO_ACQUIRE);
#if HPULOGC_ENABLE_CATEGORY
    slot = hpu_registry_lookup(match_cat, match_len);
    if (slot != NULL && slot->level_override != HPU_REG_NO_OVERRIDE &&
        (int)slot->level_override > (int)level) {
        errno = saved_errno;
        return;
    }
#endif
    if ((int)level < (int)threshold) {
        errno = saved_errno;
        return;
    }

    /* Accepted passes the filter (identity: accepted covers throttled). */
    hpu_at_fetch_add_u64(&g_rt.st_accepted, 1, HPU_MO_RELAXED);

#if HPULOGC_ENABLE_THROTTLE
#  if HPULOGC_ENABLE_CATEGORY
    if (!throttle_pass(slot, match_cat, match_len)) {
#  else
    if (!throttle_pass(NULL, match_cat, match_len)) {
#  endif
        hpu_at_fetch_add_u64(&g_rt.st_throttled, 1, HPU_MO_RELAXED);
        errno = saved_errno;
        return;
    }
#endif

    /* Step 4: message body rendering into the TLS buffer (spec 4.9). */
    tls = render_tls_get();
    if (tls == NULL) {
        errno = saved_errno;
        return; /* allocation failure: dropped silently */
    }
    max_len = hpu_at_load_u32(&g_rt.max_log_len_atomic, HPU_MO_RELAXED);
    msg = tls_ensure(&tls->msg_buf, &tls->msg_cap, max_len + 1);
    if (msg == NULL) {
        errno = saved_errno;
        return;
    }
    va_copy(ap_copy, ap);
    n = vsnprintf(msg, max_len + 1, fmt, ap_copy);
    va_end(ap_copy);
    if (n < 0) {
        n = 0;
    }
    msg_len = (size_t)n > max_len ? max_len : (size_t)n; /* silent trim */

    tid = hpu_thread_id();
    realtime_ns = hpu_realtime_ns();
    if ((int)hpu_at_load_u32(&g_rt.ts_source, HPU_MO_RELAXED) ==
        HPULOGC_TS_MONOTONIC) {
        mono_us = (int64_t)(hpu_now_ns() / 1000ULL); /* decision 8 */
    }

    if (cat_len > 0xFFFFu) {
        cat_len = 0xFFFFu;
    }

#if HPULOGC_ENABLE_ASYNC
    if (g_rt.ring != NULL) {
        int rc;

        memset(&ring_msg, 0, sizeof(ring_msg));
        ring_msg.level = (uint8_t)level;
        ring_msg.rec_flags = 0;
        ring_msg.category = cat;
        ring_msg.category_len = (uint32_t)cat_len;
#if HPULOGC_ENABLE_SOURCE_LOC
        ring_msg.file = file;
        ring_msg.file_len = file != NULL ? (uint32_t)strlen(file) : 0;
        ring_msg.func = func;
        ring_msg.func_len = func != NULL ? (uint32_t)strlen(func) : 0;
        ring_msg.line = (uint32_t)(line > 0 ? line : 0);
#endif
        ring_msg.tid = tid;
        ring_msg.realtime_ns = realtime_ns;
        ring_msg.mono_us = mono_us;
        ring_msg.msg = msg;
        ring_msg.msg_len = (uint32_t)msg_len;

        rc = hpu_ring_put(g_rt.ring, &ring_msg);
        if (rc != HPU_RING_OK) {
            /* Discard drops are counted inside the ring; nothing to do. */
        }
        errno = saved_errno;
        return;
    }
#endif

    /* Synchronous pipeline: route + render + write on the caller thread. */
    memset(&rec, 0, sizeof(rec));
    rec.level = (int)level;
    rec.category = cat;
    rec.category_len = cat_len;
#if HPULOGC_ENABLE_SOURCE_LOC
    rec.file = file;
    rec.file_len = file != NULL ? strlen(file) : 0;
    rec.func = func;
    rec.func_len = func != NULL ? strlen(func) : 0;
    rec.line = line;
#endif
    rec.msg = msg;
    rec.msg_len = msg_len;
    rec.realtime_ns = realtime_ns;
    rec.mono_us = mono_us;
    rec.tid = tid;

    hpu_mutex_lock(&g_rt.conf_lock);
    (void)hpu_pipeline_process(&rec);
    hpu_mutex_unlock(&g_rt.conf_lock);
    errno = saved_errno;
}
