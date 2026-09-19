/* -*- coding: utf-8 -*- */

/**
 * @file core.c
 * @brief Lifecycle (init/shutdown/control APIs), statistics, build info
 *        and fork handling.
 *
 * Concurrency notes:
 *  - The init/shutdown thread owns the state transition; other APIs are
 *    not promised to be concurrent with it (spec 7.5), so the state field
 *    is a plain int (read-mostly).
 *  - Configuration readers/writers serialize on g_rt.conf_lock (plain
 *    mutex: the critical sections are tiny; Phase 1 defect A resolution).
 *  - Counters use the atomic backend (spec 9).
 */

#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

hpu_runtime_t g_rt;
/* g_reg (category registry) is defined in registry.c. */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Reset the runtime block to the all-zero uninitialized state.
 */
static void rt_zero(void)
{
    memset(&g_rt, 0, sizeof(g_rt));
    g_rt.ring = NULL;
    g_rt.conf = NULL;
}

/**
 * @brief Stored-string copy without deprecated CRT calls.
 */
static void pool_strcpy(char* dst, const char* src)
{
    memcpy(dst, src, strlen(src) + 1);
}

/* ------------------------------------------------------------------ */
/* Saved code configuration (fork-reinit path, spec 9)                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Owned deep copy of a code-internal configuration.
 *
 * All strings are copied into one pool; the output/rule arrays and the
 * per-rule output-name arrays are rebuilt so the copy stays valid after
 * the caller's storage disappears.
 */
typedef struct hpu_saved_cfg {
    int valid;                    /*!< Non-zero once copied */
    hpulogc_config_t cfg;         /*!< Public value copy (pool-backed) */
    hpulogc_output_t* outputs;    /*!< Output array copy */
    hpulogc_rule_t* rules;        /*!< Rule array copy */
    const char** rule_out_names;  /*!< Flattened per-rule name arrays */
    const char** default_out;     /*!< Default output name array copy */
    char* pool;                   /*!< Owned string storage */
} hpu_saved_cfg_t;

/**
 * @brief Release the saved code configuration.
 */
static void saved_cfg_free(hpu_saved_cfg_t* s)
{
    free(s->outputs);
    free(s->rules);
    free(s->rule_out_names);
    free(s->default_out);
    free(s->pool);
    memset(s, 0, sizeof(*s));
}

/**
 * @brief Compute the string pool size for a deep copy.
 */
static size_t saved_pool_size(const hpulogc_config_t* c)
{
    size_t n = 0;
    size_t i;
    size_t k;

    if (c->default_format != NULL) {
        n += strlen(c->default_format) + 1;
    }
    for (i = 0; i < c->default_output_count; i++) {
        if (c->default_outputs != NULL && c->default_outputs[i] != NULL) {
            n += strlen(c->default_outputs[i]) + 1;
        }
    }
    for (i = 0; i < c->output_count; i++) {
        if (c->outputs[i].path != NULL) {
            n += strlen(c->outputs[i].path) + 1;
        }
        if (c->outputs[i].rotate_naming != NULL) {
            n += strlen(c->outputs[i].rotate_naming) + 1;
        }
    }
    for (i = 0; i < c->rule_count; i++) {
        if (c->rules[i].category != NULL) {
            n += strlen(c->rules[i].category) + 1;
        }
        if (c->rules[i].format != NULL) {
            n += strlen(c->rules[i].format) + 1;
        }
        for (k = 0; k < c->rules[i].output_count; k++) {
            if (c->rules[i].outputs != NULL &&
                c->rules[i].outputs[k] != NULL) {
                n += strlen(c->rules[i].outputs[k]) + 1;
            }
        }
    }
    return n;
}

/**
 * @brief Store one string in the pool and bump the cursor.
 * @return Pool pointer, or NULL when @p src is NULL.
 */
static const char* pool_store(char** cursor, const char* src)
{
    char* p;

    if (src == NULL) {
        return NULL;
    }
    p = *cursor;
    pool_strcpy(p, src);
    *cursor += strlen(src) + 1;
    return p;
}

/**
 * @brief Deep-copy a code configuration for the fork-reinit path.
 * @return 0 on success, HPULOGC_ERR_NO_MEM on allocation failure.
 */
static int saved_cfg_copy(hpu_saved_cfg_t* dst, const hpulogc_config_t* src)
{
    size_t rule_names = 0;
    size_t pool_size;
    char* cursor;
    size_t i;
    size_t k;

    memset(dst, 0, sizeof(*dst));
    dst->cfg = *src;

    for (i = 0; i < src->rule_count; i++) {
        rule_names += src->rules[i].output_count;
    }
    pool_size = saved_pool_size(src);

    if (pool_size > 0) {
        dst->pool = malloc(pool_size);
        if (dst->pool == NULL) {
            return HPULOGC_ERR_NO_MEM;
        }
    }
    if (src->output_count > 0) {
        dst->outputs = calloc(src->output_count, sizeof(*dst->outputs));
        if (dst->outputs == NULL) {
            saved_cfg_free(dst);
            return HPULOGC_ERR_NO_MEM;
        }
    }
    if (src->rule_count > 0) {
        dst->rules = calloc(src->rule_count, sizeof(*dst->rules));
        if (dst->rules == NULL) {
            saved_cfg_free(dst);
            return HPULOGC_ERR_NO_MEM;
        }
        dst->rule_out_names = calloc(rule_names, sizeof(const char*));
        if (dst->rule_out_names == NULL) {
            saved_cfg_free(dst);
            return HPULOGC_ERR_NO_MEM;
        }
    }
    if (src->default_output_count > 0) {
        dst->default_out = calloc(src->default_output_count,
                                  sizeof(const char*));
        if (dst->default_out == NULL) {
            saved_cfg_free(dst);
            return HPULOGC_ERR_NO_MEM;
        }
    }

    cursor = dst->pool;
    dst->cfg.default_format = pool_store(&cursor, src->default_format);
    for (i = 0; i < src->default_output_count; i++) {
        dst->default_out[i] =
            pool_store(&cursor, src->default_outputs != NULL
                                    ? src->default_outputs[i]
                                    : NULL);
    }
    dst->cfg.default_outputs = dst->default_out;

    for (i = 0; i < src->output_count; i++) {
        dst->outputs[i] = src->outputs[i];
        dst->outputs[i].path = pool_store(&cursor, src->outputs[i].path);
        dst->outputs[i].rotate_naming =
            pool_store(&cursor, src->outputs[i].rotate_naming);
    }
    dst->cfg.outputs = dst->outputs;

    {
        const char** name_cursor = dst->rule_out_names;

        for (i = 0; i < src->rule_count; i++) {
            dst->rules[i] = src->rules[i];
            dst->rules[i].category =
                pool_store(&cursor, src->rules[i].category);
            dst->rules[i].format = pool_store(&cursor, src->rules[i].format);
            if (src->rules[i].output_count > 0) {
                dst->rules[i].outputs = name_cursor;
                for (k = 0; k < src->rules[i].output_count; k++) {
                    name_cursor[k] =
                        pool_store(&cursor, src->rules[i].outputs != NULL
                                                ? src->rules[i].outputs[k]
                                                : NULL);
                }
                name_cursor += src->rules[i].output_count;
            }
        }
    }
    dst->cfg.rules = dst->rules;
    dst->valid = 1;
    return 0;
}

/** @brief Saved code-configuration storage (init/fork paths). */
static hpu_saved_cfg_t g_saved_cfg;

/* ------------------------------------------------------------------ */
/* Lost-line accounting (decision 16)                                  */
/* ------------------------------------------------------------------ */

/**
 * @brief Sum the lost-line counters of the active outputs.
 * @return Lost lines (live outputs only; caller holds the conf lock).
 */
static unsigned long long live_lost_total(void)
{
    unsigned long long lost = 0;
    size_t i;

    for (i = 0; i < g_rt.conf->output_count; i++) {
        if (g_rt.conf->outputs[i].handle != NULL) {
            lost += hpu_output_lost_total(g_rt.conf->outputs[i].handle);
        }
    }
    return lost;
}

/**
 * @brief Fold the lost counters of open outputs into the retired total
 *        (call before closing outputs at reload/shutdown).
 */
void hpu_core_retire_output_lost(void)
{
    hpu_mutex_lock(&g_rt.conf_lock);
    if (g_rt.conf != NULL) {
        hpu_at_fetch_add_u64(&g_rt.st_retired_lost, live_lost_total(),
                             HPU_MO_ACQ_REL);
    }
    hpu_mutex_unlock(&g_rt.conf_lock);
}

/* ------------------------------------------------------------------ */
/* Fork handling                                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief atfork child handler (POSIX): reinitialize the inherited sync
 *        primitives and mark the runtime dirty (decisions 2/3).
 *
 * Only lock reinitialization and flag stores happen here; everything
 * else is deferred to the lazy rebuild on the next log call.
 */
static void hpu_atfork_child(void)
{
    if (g_rt.state != HPU_RT_RUNNING) {
        return;
    }
    g_rt.fork_child = 1;

    if (g_rt.fork_behavior == HPU_FORK_DISABLE) {
        g_rt.fork_disabled = 1;
        return;
    }
    /* Reinitialize the locks other parent threads may have held at fork
     * time (documented trade-off, decision 2). */
    hpu_mutex_destroy(&g_rt.conf_lock);
    hpu_mutex_init(&g_rt.conf_lock);
    hpu_registry_shutdown();
    hpu_registry_init();
    if (g_rt.ring != NULL) {
        hpu_ring_discard(g_rt.ring); /* skip cond destroy (decision 3) */
        g_rt.ring = NULL;
    }
    if (g_rt.fork_behavior == HPU_FORK_REINIT) {
        hpu_at_store_u32(&g_rt.fork_dirty, 1, HPU_MO_RELEASE);
    }
}

/**
 * @brief Lazy rebuild inside a forked child (reinit behavior).
 * @return 0 on success, -1 when the rebuild failed (logs dropped).
 */
static int fork_rebuild(void)
{
    hpu_conf_t* c = malloc(sizeof(*c));
    int ok = 0;

    if (c == NULL) {
        return -1;
    }
    if (hpu_conf_defaults(c) != 0) {
        free(c);
        return -1;
    }
    if (g_rt.source_path[0] != '\0') {
#if HPULOGC_ENABLE_INI
        ok = hpu_conf_load_file(c, g_rt.source_path, -1) == 0 &&
             hpu_conf_finalize(c) == 0;
#else
        (void)c;
        ok = 0; /* INI trimmed: file config cannot be re-read */
#endif
    } else if (g_saved_cfg.valid) {
        ok = hpu_conf_from_code(c, &g_saved_cfg.cfg) == 0 &&
             hpu_conf_finalize(c) == 0;
    }
    if (!ok) {
        hpu_conf_free(c);
        free(c);
        return -1;
    }
    g_rt.conf = c;
    g_rt.state = HPU_RT_RUNNING;
    if (hpu_core_start(c) != 0) {
        /* hpu_core_start tore the snapshot down on failure */
        g_rt.conf = NULL;
        g_rt.state = HPU_RT_UNINITIALIZED;
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Common tail of all init entries: finalize + go live.
 * @return 0 or a negative error code (snapshot freed on failure).
 */
static int init_common(hpu_conf_t* c, const char* source_path)
{
    int rc;

    rc = hpu_conf_finalize(c);
    if (rc != 0) {
        hpu_conf_free(c);
        free(c);
        return rc;
    }

    if (source_path != NULL) {
        snprintf(g_rt.source_path, sizeof(g_rt.source_path), "%s",
                 source_path);
    } else {
        g_rt.source_path[0] = '\0';
    }

    if (hpu_registry_init() != 0) {
        hpu_conf_free(c);
        free(c);
        return HPULOGC_ERR_NO_MEM;
    }
    if (hpu_mutex_init(&g_rt.conf_lock) != 0) {
        hpu_registry_shutdown();
        hpu_conf_free(c);
        free(c);
        return HPULOGC_ERR_NO_MEM;
    }

    if (hpu_core_start(c) != 0) {
        hpu_mutex_destroy(&g_rt.conf_lock);
        hpu_registry_shutdown();
        /* hpu_core_start freed the snapshot on failure */
        return HPULOGC_ERR_STATE;
    }
    return 0;
}

int hpu_core_start(hpu_conf_t* c)
{
    int policy = hpu_core_ring_policy();
#if defined(HPULOGC_CONCURRENCY_SPSC)
    enum { spsc_flag = 1 };
#else
    enum { spsc_flag = 0 };
#endif

    g_rt.conf = c;
    hpu_at_store_u32(&g_rt.conf_gen, 1, HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.level_atomic, (uint32_t)c->level, HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.ts_source, (uint32_t)c->timestamp_source,
                     HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.escape_injection, (uint32_t)c->escape_injection,
                     HPU_MO_RELAXED);
    hpu_at_store_u32(&g_rt.max_log_len_atomic, (uint32_t)c->max_log_length,
                     HPU_MO_RELAXED);

    if (hpu_pipeline_tls_init() != 0) {
        goto fail;
    }
    /* fork(2) child handler (no-op on platforms without fork). */
    (void)hpu_atfork_register(hpu_atfork_child);

#if HPULOGC_ENABLE_ASYNC
    g_rt.ring = hpu_ring_create(c->buffer_size, policy, spsc_flag);
    if (g_rt.ring == NULL) {
        goto fail;
    }
    if (hpu_consumer_start() != 0) {
        hpu_ring_destroy(g_rt.ring);
        g_rt.ring = NULL;
        goto fail;
    }
#else
    (void)policy;
    if (c->stats_interval > 0) {
        /* No background thread exists to emit periodic stats (decision 18). */
        fprintf(stderr,
                "hpulogc: warning: stats interval ignored in synchronous "
                "builds\n");
    }
#endif

#if defined(_WIN32)
    /* Probe the std streams once at startup (decision 23): the console
     * backend only calls hpu_fs_isatty when COLOR is enabled, but the
     * binary-mode switch inside it must happen for every build or the
     * CRT text mode translates the rendered \r\n into \r\r\n. */
    (void)hpu_fs_isatty(1);
    (void)hpu_fs_isatty(2);
#endif

    g_rt.stats_last_ns = (int64_t)(hpu_now_ns() / 1000000ULL);

#if HPULOGC_ENABLE_HOT_RELOAD
    if (c->source_path[0] != '\0') {
        if (hpu_hotreload_start() != 0) {
            fprintf(stderr,
                    "hpulogc: warning: config watcher unavailable; hot "
                    "reload disabled\n");
        }
    }
#endif

    if (c->signal_reload) {
        if (hpu_signal_install_hup() == 0) {
            g_rt.signal_reload_installed = 1;
        } else {
            /* Registration failure keeps polling-based reload alive
             * (spec 10.3); Windows has no SIGHUP and warns here. */
            fprintf(stderr,
                    "hpulogc: warning: signal reload registration failed; "
                    "relying on polling only\n");
        }
    }

    if (c->signal_safe) {
        hpu_signal_safe_enable();
    }

    g_rt.state = HPU_RT_RUNNING;
    return 0;

fail:
    hpu_core_retire_output_lost();
    hpu_conf_free(c);
    free(c);
    g_rt.conf = NULL;
    g_rt.state = HPU_RT_UNINITIALIZED;
    return -1;
}

int hpu_core_ring_policy(void)
{
    int policy = HPULOGC_OVERFLOW_DISCARD;

    if (g_rt.conf != NULL) {
        policy = g_rt.conf->overflow_policy;
    }
#if HPULOGC_LOCKFREE
    if (policy == HPULOGC_OVERFLOW_WAIT) {
        policy = HPULOGC_OVERFLOW_DISCARD; /* defensive; rejected at init */
    }
#if defined(HPULOGC_CONCURRENCY_MSPC)
    if (policy == HPULOGC_OVERFLOW_OVERWRITE) {
        policy = HPULOGC_OVERFLOW_DISCARD;
    }
#endif
#endif
    return policy;
}

/**
 * @brief Add the built-in stderr console output used by init_default.
 * @return 0 or HPULOGC_ERR_NO_MEM.
 */
static int conf_add_default_console(hpu_conf_t* c)
{
    c->outputs = calloc(1, sizeof(hpu_conf_output_t));
    if (c->outputs == NULL) {
        return HPULOGC_ERR_NO_MEM;
    }
    c->output_count = 1;
    snprintf(c->outputs[0].name_buf, sizeof(c->outputs[0].name_buf),
             "stderr");
    c->outputs[0].pub.type = HPULOGC_OUT_CONSOLE;
    c->outputs[0].pub.stream = 1;
    c->outputs[0].pub.color = 1;
    snprintf(c->default_output_names[0],
             sizeof(c->default_output_names[0]), "stderr");
    c->default_output_name_count = 1;
    return 0;
}

static int init_from_code(const hpulogc_config_t* cfg)
{
    hpu_conf_t* c;
    int rc;

    c = malloc(sizeof(*c));
    if (c == NULL) {
        return HPULOGC_ERR_NO_MEM;
    }
    rc = hpu_conf_defaults(c);
    if (rc != 0) {
        free(c);
        return rc;
    }
    rc = hpu_conf_from_code(c, cfg);
    if (rc != 0) {
        hpu_conf_free(c);
        free(c);
        return rc;
    }
    return init_common(c, NULL);
}

int hpulogc_init(const hpulogc_config_t* cfg)
{
    int rc;

    if (g_rt.state == HPU_RT_RUNNING || g_rt.state == HPU_RT_INITIALIZING) {
        return HPULOGC_ERR_STATE;
    }
    if (cfg == NULL) {
        return hpulogc_init_default();
    }
    rt_zero();
    g_rt.state = HPU_RT_INITIALIZING;

    rc = saved_cfg_copy(&g_saved_cfg, cfg);
    if (rc != 0) {
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    g_rt.fork_behavior = HPU_FORK_REINIT; /* default; file-only key */

    rc = init_from_code(cfg);
    if (rc != 0) {
        saved_cfg_free(&g_saved_cfg);
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    return 0;
}

int hpulogc_init_default(void)
{
    hpu_conf_t* c;
    int rc;

    if (g_rt.state == HPU_RT_RUNNING || g_rt.state == HPU_RT_INITIALIZING) {
        return HPULOGC_ERR_STATE;
    }
    rt_zero();
    g_rt.state = HPU_RT_INITIALIZING;

    c = malloc(sizeof(*c));
    if (c == NULL) {
        g_rt.state = HPU_RT_UNINITIALIZED;
        return HPULOGC_ERR_NO_MEM;
    }
    rc = hpu_conf_defaults(c);
    if (rc != 0) {
        free(c);
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    rc = conf_add_default_console(c);
    if (rc != 0) {
        hpu_conf_free(c);
        free(c);
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    rc = init_common(c, NULL);
    if (rc != 0) {
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    return 0;
}

int hpulogc_init_from_file(const char* config_path)
{
    hpu_conf_t* c;
    int rc;

    if (config_path == NULL) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (g_rt.state == HPU_RT_RUNNING || g_rt.state == HPU_RT_INITIALIZING) {
        return HPULOGC_ERR_STATE;
    }
    rt_zero();
    g_rt.state = HPU_RT_INITIALIZING;

#if !HPULOGC_ENABLE_INI
    (void)c;
    (void)rc;
    fprintf(stderr,
            "hpulogc: init_from_file: INI support is not compiled in\n");
    g_rt.state = HPU_RT_UNINITIALIZED;
    return HPULOGC_ERR_CONFIG;
#else
    c = malloc(sizeof(*c));
    if (c == NULL) {
        g_rt.state = HPU_RT_UNINITIALIZED;
        return HPULOGC_ERR_NO_MEM;
    }
    rc = hpu_conf_defaults(c);
    if (rc != 0) {
        free(c);
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    rc = hpu_conf_load_file(c, config_path, -1);
    if (rc != 0) {
        hpu_conf_free(c);
        free(c);
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    rc = init_common(c, config_path);
    if (rc != 0) {
        g_rt.state = HPU_RT_UNINITIALIZED;
        return rc;
    }
    return 0;
#endif
}

void hpulogc_config_default(hpulogc_config_t* cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = HPULOGC_LEVEL_INFO;
    cfg->default_format = NULL; /* NULL = "standard" */
    cfg->default_outputs = NULL;
    cfg->default_output_count = 0;
    cfg->outputs = NULL;
    cfg->output_count = 0;
    cfg->rules = NULL;
    cfg->rule_count = 0;
    cfg->buffer_size = 1024U * 1024U;
    cfg->overflow_policy = HPULOGC_OVERFLOW_DISCARD;
    cfg->batch_size = 64;
    cfg->flush_interval_ms = 100;
    cfg->shutdown_timeout_ms = 5000;
    cfg->escape_injection = 1;
    cfg->max_log_length = 4096;
    cfg->truncation_marker = NULL; /* NULL = "...[TRUNCATED]" */
    cfg->crash_safety = HPULOGC_CRASH_SHUTDOWN;
    cfg->signal_safe = 0;
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                            */
/* ------------------------------------------------------------------ */

void hpulogc_shutdown(void)
{
    if (g_rt.state != HPU_RT_RUNNING) {
        g_rt.state = HPU_RT_UNINITIALIZED; /* idempotent */
        return;
    }
    g_rt.state = HPU_RT_SHUTDOWN;
    hpu_core_stop();
    saved_cfg_free(&g_saved_cfg);
    hpu_mutex_destroy(&g_rt.conf_lock);
    hpu_registry_shutdown();
    g_rt.state = HPU_RT_UNINITIALIZED;
    rt_zero();
}

void hpu_core_stop(void)
{
    hpu_signal_safe_disable();

#if HPULOGC_ENABLE_HOT_RELOAD
    hpu_hotreload_stop();
#endif
#if HPULOGC_ENABLE_ASYNC
    if (g_rt.ring != NULL) {
        hpu_ring_close(g_rt.ring); /* unblocks wait producers + consumer */
        hpu_ring_kick_consumer(g_rt.ring);
    }
    hpu_consumer_stop();
    if (g_rt.ring != NULL) {
        hpu_ring_destroy(g_rt.ring);
        g_rt.ring = NULL;
    }
#endif

    hpu_core_retire_output_lost();
    if (g_rt.conf != NULL) {
        hpu_conf_free(g_rt.conf);
        free(g_rt.conf);
        g_rt.conf = NULL;
    }
    if (g_rt.signal_reload_installed) {
        g_rt.signal_reload_installed = 0;
        /* The SIGHUP handler stays installed; restoring SIG_DFL here
         * could clobber an application handler registered after ours. */
    }
    hpu_registry_reset();
    /* Must run after the consumer stopped: its render buffer is released
     * by the TLS destructor at thread exit; ours is freed here. */
    hpu_pipeline_tls_shutdown();
}

/* ------------------------------------------------------------------ */
/* Flush / sync                                                        */
/* ------------------------------------------------------------------ */

int hpulogc_flush(void)
{
    if (g_rt.state != HPU_RT_RUNNING) {
        return HPULOGC_ERR_STATE;
    }
#if HPULOGC_ENABLE_ASYNC
    return hpu_consumer_flush() == 0 ? HPULOGC_OK : HPULOGC_ERR_IO;
#else
    {
        /* Sync builds: flush is best effort (spec 7.3 "尽量写出"). Write
         * failures were already handled per spec 9 (reopen-once, then the
         * line is dropped and counted in stats), so they do not surface
         * through the return code — mirroring the async build where the
         * consumer absorbs them (decision 33). */
        size_t i;

        hpu_mutex_lock(&g_rt.conf_lock);
        for (i = 0; g_rt.conf != NULL && i < g_rt.conf->output_count; i++) {
            if (g_rt.conf->outputs[i].handle != NULL) {
                (void)hpu_output_flush(g_rt.conf->outputs[i].handle);
            }
        }
        hpu_mutex_unlock(&g_rt.conf_lock);
        return HPULOGC_OK;
    }
#endif
}

int hpulogc_sync(void)
{
    int rc = 0;
    size_t i;

    if (g_rt.state != HPU_RT_RUNNING) {
        return HPULOGC_ERR_STATE;
    }
#if HPULOGC_ENABLE_ASYNC
    if (hpu_consumer_flush() != 0) {
        rc = -1;
    }
#endif
    hpu_mutex_lock(&g_rt.conf_lock);
    for (i = 0; g_rt.conf != NULL && i < g_rt.conf->output_count; i++) {
        if (g_rt.conf->outputs[i].handle != NULL &&
            hpu_output_sync(g_rt.conf->outputs[i].handle) != 0) {
            rc = -1;
        }
    }
    hpu_mutex_unlock(&g_rt.conf_lock);
    return rc == 0 ? HPULOGC_OK : HPULOGC_ERR_IO;
}

/* ------------------------------------------------------------------ */
/* Runtime controls                                                    */
/* ------------------------------------------------------------------ */

int hpulogc_set_level(hpulogc_level_t level)
{
    if (g_rt.state != HPU_RT_RUNNING) {
        return HPULOGC_ERR_STATE;
    }
    if (level < HPULOGC_LEVEL_TRACE || level > HPULOGC_LEVEL_OFF) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    hpu_at_store_u32(&g_rt.level_atomic, (uint32_t)level, HPU_MO_RELEASE);
    hpu_mutex_lock(&g_rt.conf_lock);
    if (g_rt.conf != NULL) {
        g_rt.conf->level = (int)level;
    }
    hpu_mutex_unlock(&g_rt.conf_lock);
    return HPULOGC_OK;
}

int hpulogc_set_level_for_category(const char* category,
                                   hpulogc_level_t level)
{
#if !HPULOGC_ENABLE_CATEGORY
    (void)category;
    (void)level;
    return HPULOGC_ERR_CONFIG;
#else
    if (g_rt.state != HPU_RT_RUNNING) {
        return HPULOGC_ERR_STATE;
    }
    if (category == NULL) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (level != (hpulogc_level_t)-1 &&
        (level < HPULOGC_LEVEL_TRACE || level > HPULOGC_LEVEL_OFF)) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    return hpu_registry_set_override(category, level);
#endif
}

/* ------------------------------------------------------------------ */
/* Build info / stats                                                  */
/* ------------------------------------------------------------------ */

void hpulogc_get_build_info(hpulogc_build_info_t* info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->build_version = HPULOGC_BUILD_VERSION_NAME;
    info->has_async = HPULOGC_ENABLE_ASYNC;
    info->has_color = HPULOGC_ENABLE_COLOR;
    info->has_rotate = HPULOGC_ENABLE_ROTATE;
    info->has_hot_reload = HPULOGC_ENABLE_HOT_RELOAD;
    info->has_category = HPULOGC_ENABLE_CATEGORY;
    info->has_throttle = HPULOGC_ENABLE_THROTTLE;
    info->has_ini = HPULOGC_ENABLE_INI;
    info->lockfree = HPULOGC_LOCKFREE;
#if defined(HPULOGC_CONCURRENCY_SPSC)
    info->concurrency = "spsc";
#else
    info->concurrency = "mpsc";
#endif
}

int hpulogc_get_stats(hpulogc_stats_t* stats)
{
    unsigned long long ring_dropped = 0;
    unsigned long long ring_overwritten = 0;
    unsigned long long lost;
    unsigned long long written_counter;

    if (stats == NULL) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (g_rt.state != HPU_RT_RUNNING) {
        return HPULOGC_ERR_STATE;
    }

    if (g_rt.ring != NULL) {
        hpu_ring_counters(g_rt.ring, &ring_dropped, &ring_overwritten);
    }
    hpu_mutex_lock(&g_rt.conf_lock);
    lost = hpu_at_load_u64(&g_rt.st_retired_lost, HPU_MO_ACQUIRE);
    if (g_rt.conf != NULL) {
        lost += live_lost_total();
    }
    hpu_mutex_unlock(&g_rt.conf_lock);

    /* Written migrates lines lost at flush time into dropped (decision 16). */
    written_counter = hpu_at_load_u64(&g_rt.st_written, HPU_MO_ACQUIRE);
    if (lost > written_counter) {
        lost = written_counter; /* defensive: never report negative */
    }

    stats->accepted = hpu_at_load_u64(&g_rt.st_accepted, HPU_MO_ACQUIRE);
    stats->dropped = ring_dropped + lost;
    stats->overwritten = ring_overwritten;
    stats->throttled = hpu_at_load_u64(&g_rt.st_throttled, HPU_MO_ACQUIRE);
    stats->written = written_counter - lost;
    if (g_rt.ring != NULL) {
        stats->buffer_used = hpu_ring_used(g_rt.ring);
        stats->buffer_size = hpu_ring_capacity(g_rt.ring);
    } else {
        stats->buffer_used = 0;
        stats->buffer_size = 0;
    }
    return HPULOGC_OK;
}

/* ------------------------------------------------------------------ */
/* Fork rebuild entry (pipeline calls this on the first child log)     */
/* ------------------------------------------------------------------ */

int hpu_core_fork_check(void)
{
    if (!g_rt.fork_child) {
        return 0;
    }
    if (g_rt.fork_disabled) {
        return 1;
    }
    if (hpu_at_load_u32(&g_rt.fork_dirty, HPU_MO_ACQUIRE) != 0) {
        hpu_at_store_u32(&g_rt.fork_dirty, 0, HPU_MO_RELAXED);
        if (fork_rebuild() != 0) {
            g_rt.fork_disabled = 1;
            fprintf(stderr,
                    "hpulogc: fork child: rebuild failed; logging disabled "
                    "(rate limited)\n");
            return 1;
        }
    }
    return 0;
}
