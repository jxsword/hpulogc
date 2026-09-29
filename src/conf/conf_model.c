/**
 * @file conf_model.c
 * @brief Configuration snapshot defaults, code-config application and
 *        finalization (validation + output opening).
 *
 * Decision (docs/implementation_notes.md): hpulogc_output_t has no name
 * field while rules reference outputs by name (spec 7.6). Code-config
 * outputs therefore receive the implicit names "out0".."outN-1" by array
 * position, and rules/default_outputs reference those names.
 */

#include "conf_model.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Phase 1 defect C fix (docs/implementation_notes.md): <unistd.h> is not
 * available under MSVC; _getpid() provides the same value. */
#if defined(_MSC_VER)
#include <process.h>
#define hpu_getpid() ((long)_getpid())
#else
#include <unistd.h>
#define hpu_getpid() ((long)getpid())
#endif

#include "../platform/platform.h"

/** @brief Buffer size bounds (spec 7.6). */
#define CONF_BUFFER_MIN (4U * 1024U)
#define CONF_BUFFER_MAX (1024U * 1024U * 1024U)

/* ---- Diagnostics sink (consumed by hpulogc_conf_validate) ---- */

hpu_conf_diag_fn g_conf_diag_sink = NULL;
void* g_conf_diag_user = NULL;

void hpu_conf_diag(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_conf_diag_sink != NULL) {
        g_conf_diag_sink(g_conf_diag_user, buf);
    } else {
        fputs(buf, stderr);
    }
}

/**
 * @brief Compute the max single-record size used for the buffer raise.
 */
static size_t max_record_size(size_t max_log_length)
{
    return sizeof(uint64_t) * 4 + sizeof(uint32_t) * 4 + sizeof(uint16_t) * 4 +
           max_log_length + 256 + 512 + 256 + 8;
}

int hpu_conf_defaults(hpu_conf_t* c)
{
    size_t i;

    memset(c, 0, sizeof(*c));

    c->level = HPULOGC_LEVEL_INFO;
    snprintf(c->default_format, sizeof(c->default_format), "standard");
    c->use_utc = 0;
    c->timestamp_source = HPULOGC_TS_REALTIME;
    snprintf(c->time_format, sizeof(c->time_format),
             "%%Y-%%m-%%d %%H:%%M:%%S.%%f");
    c->newline_style = HPULOGC_NEWLINE_AUTO;
    c->pid_fmt = HPU_ID_FMT_DECIMAL;
    c->tid_fmt = HPU_ID_FMT_DECIMAL;
    c->capture_source_loc = 1;
    c->strict_init = 1;
    c->hot_reload_interval = 5;
    c->signal_reload = 0;

    c->buffer_size = 1024 * 1024;
    c->overflow_policy = HPULOGC_OVERFLOW_DISCARD;

    c->batch_size = 64;
    c->flush_interval_ms = 100;
    c->shutdown_timeout_ms = 5000;
    c->consumer_threads = 1;

    c->throttle.global_rate = 0;
    c->throttle.per_category_rate = 0;
    c->throttle.burst = 100;
    c->throttle.sampling_rate = 1.0;
    c->throttle.sampling_n = 1;

    c->escape_injection = 1;
    c->max_log_length = 4096;
    snprintf(c->truncation_marker, sizeof(c->truncation_marker),
             "...[TRUNCATED]");
    c->fork_behavior = HPU_FORK_REINIT;
    c->signal_safe = 0;
    c->crash_safety = HPULOGC_CRASH_SHUTDOWN;
    c->stats_interval = 0;

    /* Five built-in formats (spec 10.3). */
    c->formats = calloc(5, sizeof(hpu_format_t));
    if (c->formats == NULL) {
        return HPULOGC_ERR_NO_MEM;
    }
    c->format_count = 5;
    for (i = 0; i < 5; i++) {
        if (hpu_format_compile(&c->formats[i], hpu_builtin_format_names[i],
                               hpu_builtin_format_templates[i]) != 0) {
            c->format_count = i;
            hpu_conf_free(c);
            return HPULOGC_ERR_NO_MEM;
        }
    }
    return 0;
}

int hpu_conf_from_code(hpu_conf_t* c, const hpulogc_config_t* cfg)
{
    size_t i;

    if (cfg == NULL) {
        return 0;
    }

    if (cfg->level < HPULOGC_LEVEL_TRACE || cfg->level > HPULOGC_LEVEL_OFF) {
        hpu_conf_diag(
                "hpulogc: config error: level out of range\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    c->level = cfg->level;

    if (cfg->default_format != NULL) {
        if (strlen(cfg->default_format) >= HPULOGC_MAX_NAME_LEN) {
            hpu_conf_diag(
                    "hpulogc: config error: default format name too long\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
        snprintf(c->default_format, sizeof(c->default_format), "%s",
                 cfg->default_format);
    }

    if (cfg->default_output_count > HPULOGC_MAX_OUTPUTS) {
        hpu_conf_diag( "hpulogc: config error: too many default outputs\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    for (i = 0; i < cfg->default_output_count; i++) {
        const char* name = cfg->default_outputs != NULL
                               ? cfg->default_outputs[i]
                               : NULL;

        if (name == NULL || name[0] == '\0' ||
            strlen(name) >= HPULOGC_MAX_NAME_LEN) {
            hpu_conf_diag(
                    "hpulogc: config error: bad default output name\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
        snprintf(c->default_output_names[i],
                 sizeof(c->default_output_names[i]), "%s", name);
    }
    c->default_output_name_count = cfg->default_output_count;

    /* outputs (implicit names out0..outN-1, see file header) */
    if (cfg->output_count > HPULOGC_MAX_OUTPUTS) {
        hpu_conf_diag( "hpulogc: config error: too many outputs (%d max)\n",
                HPULOGC_MAX_OUTPUTS);
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (cfg->output_count > 0) {
        c->outputs = calloc(cfg->output_count, sizeof(hpu_conf_output_t));
        if (c->outputs == NULL) {
            return HPULOGC_ERR_NO_MEM;
        }
        c->output_count = cfg->output_count;
        for (i = 0; i < cfg->output_count; i++) {
            const hpulogc_output_t* src = &cfg->outputs[i];
            hpu_conf_output_t* dst = &c->outputs[i];

            dst->pub = *src;
        dst->enabled = 1; /* common-key default (rd_v0.6 §4.7.3) */
            snprintf(dst->name_buf, sizeof(dst->name_buf), "out%zu", i);
            dst->pub.rotate_naming = NULL;

            if (src->type == HPULOGC_OUT_FILE) {
                if (src->path == NULL ||
                    strlen(src->path) >= HPULOGC_MAX_PATH_LEN) {
                    hpu_conf_diag(
                            "hpulogc: config error: output path missing or "
                            "too long\n");
                    return HPULOGC_ERR_CONFIG;
                }
                snprintf(dst->path_buf, sizeof(dst->path_buf), "%s",
                         src->path);
                dst->pub.path = dst->path_buf;
                if (src->rotate_naming != NULL) {
                    if (strlen(src->rotate_naming) >= HPULOGC_MAX_FMT_LEN) {
                        hpu_conf_diag(
                                "hpulogc: config error: rotate naming too "
                                "long\n");
                        return HPULOGC_ERR_CONFIG;
                    }
                    snprintf(dst->naming_buf, sizeof(dst->naming_buf), "%s",
                             src->rotate_naming);
                    dst->pub.rotate_naming = dst->naming_buf;
                }
                if ((src->rotate == HPULOGC_ROTATE_SIZE ||
                     src->rotate == HPULOGC_ROTATE_BOTH) &&
                    src->max_size == 0) {
                    hpu_conf_diag(
                            "hpulogc: config error: max size must be > 0\n");
                    return HPULOGC_ERR_CONFIG;
                }
                if (src->time_unit < HPULOGC_TU_HOUR ||
                    src->time_unit > HPULOGC_TU_MONTH) {
                    hpu_conf_diag(
                            "hpulogc: config error: bad time unit\n");
                    return HPULOGC_ERR_CONFIG;
                }
            } else if (src->type == HPULOGC_OUT_CONSOLE) {
                if (src->stream != 0 && src->stream != 1) {
                    hpu_conf_diag(
                            "hpulogc: config error: console stream must be "
                            "0/1\n");
                    return HPULOGC_ERR_CONFIG;
                }
            } else {
                hpu_conf_diag( "hpulogc: config error: bad output type\n");
                return HPULOGC_ERR_CONFIG;
            }
        }
    }

    /* rules */
    if (cfg->rule_count > HPULOGC_MAX_RULES) {
        hpu_conf_diag( "hpulogc: config error: too many rules (%d max)\n",
                HPULOGC_MAX_RULES);
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (cfg->rule_count > 0) {
        c->rules = calloc(cfg->rule_count, sizeof(hpu_conf_rule_t));
        if (c->rules == NULL) {
            return HPULOGC_ERR_NO_MEM;
        }
        c->rule_count = cfg->rule_count;
        for (i = 0; i < cfg->rule_count; i++) {
            const hpulogc_rule_t* src = &cfg->rules[i];
            hpu_conf_rule_t* dst = &c->rules[i];
            size_t k;

            if (src->min_level > src->max_level ||
                src->min_level < HPULOGC_LEVEL_TRACE ||
                src->max_level > HPULOGC_LEVEL_OFF) {
                hpu_conf_diag(
                        "hpulogc: config error: rule level range invalid\n");
                return HPULOGC_ERR_CONFIG;
            }
            if (src->format == NULL) {
                hpu_conf_diag(
                        "hpulogc: config error: rule format missing\n");
                return HPULOGC_ERR_CONFIG;
            }
            {
                int builtin = 0;
                size_t j;

                for (j = 0; j < 5; j++) {
                    if (strcmp(src->format, hpu_builtin_format_names[j]) ==
                        0) {
                        builtin = 1;
                    }
                }
                if (!builtin) {
                    hpu_conf_diag(
                            "hpulogc: config error: rule format '%s' is not "
                            "a built-in format\n",
                            src->format);
                    return HPULOGC_ERR_CONFIG;
                }
            }
            snprintf(dst->format_name, sizeof(dst->format_name), "%s",
                     src->format);
            if (src->category == NULL || src->category[0] == '\0') {
                snprintf(dst->category, sizeof(dst->category), "*");
            } else {
                if (strlen(src->category) >= HPULOGC_MAX_NAME_LEN) {
                    hpu_conf_diag(
                            "hpulogc: config error: rule category too long\n");
                    return HPULOGC_ERR_CONFIG;
                }
                snprintf(dst->category, sizeof(dst->category), "%s",
                         src->category);
            }
            dst->min_level = src->min_level;
            dst->max_level = src->max_level;

            if (src->output_count > HPULOGC_MAX_OUTPUTS) {
                hpu_conf_diag(
                        "hpulogc: config error: too many rule outputs\n");
                return HPULOGC_ERR_CONFIG;
            }
            dst->output_count = 0;
            for (k = 0; k < src->output_count; k++) {
                const char* name =
                    src->outputs != NULL ? src->outputs[k] : NULL;
                size_t m;
                int found = -1;

                if (name == NULL) {
                    hpu_conf_diag(
                            "hpulogc: config error: rule output name "
                            "missing\n");
                    return HPULOGC_ERR_CONFIG;
                }
                for (m = 0; m < c->output_count; m++) {
                    if (strcmp(c->outputs[m].name_buf, name) == 0) {
                        found = (int)m;
                    }
                }
                if (found < 0) {
                    hpu_conf_diag(
                            "hpulogc: config error: rule references "
                            "undefined output '%s'\n",
                            name);
                    return HPULOGC_ERR_CONFIG;
                }
                dst->output_idx[dst->output_count++] = found;
            }
        }
    }

    /* buffer (code config validates strictly; auto-raise still applies) */
    if (cfg->buffer_size != 0) {
        c->buffer_size = cfg->buffer_size;
    }
    {
        size_t need = max_record_size(c->max_log_length) * 2;

        if (c->buffer_size < need || c->buffer_size < CONF_BUFFER_MIN) {
            c->buffer_size = CONF_BUFFER_MIN;
            if (need > c->buffer_size) {
                c->buffer_size = need;
            }
            hpu_conf_diag(
                    "hpulogc: warning: buffer size raised to %zu bytes\n",
                    c->buffer_size);
        }
        if (c->buffer_size > CONF_BUFFER_MAX) {
            hpu_conf_diag(
                    "hpulogc: config error: buffer size above 1GB\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
    }
    switch (cfg->overflow_policy) {
    case HPULOGC_OVERFLOW_DISCARD:
    case HPULOGC_OVERFLOW_OVERWRITE:
    case HPULOGC_OVERFLOW_WAIT:
        c->overflow_policy = (int)cfg->overflow_policy;
        break;
    default:
        hpu_conf_diag(
                "hpulogc: config error: invalid overflow policy\n");
        return HPULOGC_ERR_INVALID_ARG;
    }

    /* async */
    if (cfg->batch_size < 1 || cfg->batch_size > 65535) {
        hpu_conf_diag( "hpulogc: config error: batch size out of range\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    if (cfg->flush_interval_ms < 1 || cfg->flush_interval_ms > 60000) {
        hpu_conf_diag(
                "hpulogc: config error: flush interval out of range\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    /* Generic sink declarations (rd_v0.6 §4.10.5): appended to the same
     * outputs namespace; key/value strings are borrowed from the caller's
     * config and consumed by create during finalize (within the documented
     * "strings valid until init returns" window). */
    if (cfg->sink_count > 0) {
        hpu_conf_output_t* grown;

        if (cfg->sink_count > HPULOGC_MAX_OUTPUTS ||
            cfg->output_count + cfg->sink_count > HPULOGC_MAX_OUTPUTS) {
            hpu_conf_diag( "hpulogc: config error: too many sinks\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
        grown = realloc(c->outputs, (cfg->output_count + cfg->sink_count) *
                                        sizeof(hpu_conf_output_t));
        if (grown == NULL) {
            return HPULOGC_ERR_NO_MEM;
        }
        c->outputs = grown;
        for (i = 0; i < cfg->sink_count; i++) {
            const hpulogc_sink_decl_t* d = &cfg->sinks[i];
            hpu_conf_output_t* o =
                &c->outputs[c->output_count + i];

            memset(o, 0, sizeof(*o));
            if (d->name == NULL || d->name[0] == '\0' ||
                strlen(d->name) >= HPULOGC_MAX_NAME_LEN ||
                d->type == NULL || d->type[0] == '\0' ||
                strlen(d->type) >= HPULOGC_MAX_NAME_LEN) {
                hpu_conf_diag(
                        "hpulogc: config error: invalid sink name/type\n");
                return HPULOGC_ERR_INVALID_ARG;
            }
            snprintf(o->name_buf, sizeof(o->name_buf), "%s", d->name);
            o->is_generic = 1;
            snprintf(o->type_name, sizeof(o->type_name), "%s", d->type);
            o->enabled = 1;
            o->async = 0;
            if (d->count > HPU_CONF_MAX_SINK_KV) {
                hpu_conf_diag(
                        "hpulogc: config error: too many sink "
                        "parameters\n");
                return HPULOGC_ERR_INVALID_ARG;
            }
            {
                size_t k;

                for (k = 0; k < d->count; k++) {
                    if (d->keys == NULL || d->vals == NULL ||
                        d->keys[k] == NULL || d->vals[k] == NULL) {
                        hpu_conf_diag(
                                "hpulogc: config error: sink key/value "
                                "missing\n");
                        return HPULOGC_ERR_INVALID_ARG;
                    }
                    if (strcmp(d->keys[k], "enabled") == 0) {
                        o->enabled = strcmp(d->vals[k], "true") == 0 ||
                                     strcmp(d->vals[k], "on") == 0;
                        continue;
                    }
                    if (strcmp(d->keys[k], "async") == 0) {
                        o->async = strcmp(d->vals[k], "on") == 0 ||
                                   strcmp(d->vals[k], "true") == 0;
                        continue;
                    }
                    if (strcmp(d->keys[k], "queue size") == 0) {
                        unsigned long long sz;

                        if (hpu_parse_size_str(d->vals[k], &sz) != 0) {
                            hpu_conf_diag(
                                    "hpulogc: config error: invalid queue "
                                    "size\n");
                            return HPULOGC_ERR_INVALID_ARG;
                        }
                        o->queue_size = (size_t)sz;
                        continue;
                    }
                    {
                        size_t klen = strlen(d->keys[k]);
                        size_t vlen = strlen(d->vals[k]);

                        if (o->kv_pool_len + klen + vlen + 2 >
                            sizeof(o->kv_pool)) {
                            hpu_conf_diag(
                                    "hpulogc: config error: sink "
                                    "parameters too long\n");
                            return HPULOGC_ERR_INVALID_ARG;
                        }
                        memcpy(o->kv_pool + o->kv_pool_len, d->keys[k],
                               klen + 1);
                        o->kv_key_off[o->kv_count] = o->kv_pool_len;
                        o->kv_pool_len += klen + 1;
                        memcpy(o->kv_pool + o->kv_pool_len, d->vals[k],
                               vlen + 1);
                        o->kv_val_off[o->kv_count] = o->kv_pool_len;
                        o->kv_pool_len += vlen + 1;
                    }
                    o->kv_count++;
                }
            }
        }
        c->output_count += cfg->sink_count;
    }

    c->batch_size = cfg->batch_size;
    c->flush_interval_ms = cfg->flush_interval_ms;
    c->shutdown_timeout_ms = cfg->shutdown_timeout_ms;
    if (cfg->consumer_threads < 1 ||
        cfg->consumer_threads > HPU_MAX_CONSUMERS) {
        hpu_conf_diag(
                "hpulogc: config error: consumer threads out of range\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    c->consumer_threads = cfg->consumer_threads;

    /* advanced */
    c->escape_injection = cfg->escape_injection;
    if (cfg->max_log_length != 0) {
        if (cfg->max_log_length < 256 || cfg->max_log_length > 65536) {
            hpu_conf_diag(
                    "hpulogc: config error: max log length out of range\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
        c->max_log_length = cfg->max_log_length;
    }
    if (cfg->truncation_marker != NULL) {
        if (strlen(cfg->truncation_marker) >= HPULOGC_MAX_FMT_LEN) {
            hpu_conf_diag(
                    "hpulogc: config error: truncation marker too long\n");
            return HPULOGC_ERR_INVALID_ARG;
        }
        snprintf(c->truncation_marker, sizeof(c->truncation_marker), "%s",
                 cfg->truncation_marker);
    }
    if (cfg->crash_safety < HPULOGC_CRASH_NONE ||
        cfg->crash_safety > HPULOGC_CRASH_SHUTDOWN) {
        hpu_conf_diag( "hpulogc: config error: invalid crash safety\n");
        return HPULOGC_ERR_INVALID_ARG;
    }
    c->crash_safety = cfg->crash_safety;
    c->signal_safe = cfg->signal_safe;

    /* re-raise buffer if max_log_length changed the record bound */
    {
        size_t need = max_record_size(c->max_log_length) * 2;

        if (c->buffer_size < need) {
            c->buffer_size = need;
            hpu_conf_diag(
                    "hpulogc: warning: buffer size raised to %zu bytes\n",
                    c->buffer_size);
        }
    }

    return 0;
}

int hpu_conf_find_output(const hpu_conf_t* c, const char* name)
{
    size_t i;

    for (i = 0; i < c->output_count; i++) {
        if (strcmp(c->outputs[i].name_buf, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int hpu_conf_rule_level_match(const hpu_conf_rule_t* rule, int level)
{
    if (rule->negate_level) {
        /* "!LEVEL": everything except the exact level (spec 10.3) */
        return level != rule->min_level;
    }
    return rule->min_level <= level && level <= rule->max_level;
}

const hpu_format_t* hpu_conf_find_format(const hpu_conf_t* c,
                                         const char* name)
{
    size_t i;

    for (i = 0; i < c->format_count; i++) {
        if (strcmp(c->formats[i].name, name) == 0) {
            return &c->formats[i];
        }
    }
    return NULL;
}

/**
 * @brief Resolve default output names to indices.
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int resolve_default_outputs(hpu_conf_t* c)
{
    size_t i, k;

    c->default_output_count = 0; /* reused as resolved count */
    for (i = 0; i < c->default_output_name_count; i++) {
        int idx = hpu_conf_find_output(c, c->default_output_names[i]);

        if (idx < 0) {
            hpu_conf_diag(
                    "hpulogc: config error: default output '%s' undefined\n",
                    c->default_output_names[i]);
            return HPULOGC_ERR_CONFIG;
        }
        c->default_output_idx[i] = idx;
        c->default_output_count++;
    }
    for (k = 0; k < c->rule_count; k++) {
        /* rules resolved during parse/finalize already */
    }
    (void)k;
    return 0;
}

/**
 * @brief Resolve rule format/output references.
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int resolve_rules(hpu_conf_t* c)
{
    size_t i, k;

    for (i = 0; i < c->rule_count; i++) {
        if (hpu_conf_find_format(c, c->rules[i].format_name) == NULL) {
            hpu_conf_diag(
                    "hpulogc: config error: rule references undefined "
                    "format '%s'\n",
                    c->rules[i].format_name);
            return HPULOGC_ERR_CONFIG;
        }
        for (k = 0; k < c->rules[i].output_count; k++) {
            int idx = c->rules[i].output_idx[k];

            if (idx < 0 || (size_t)idx >= c->output_count) {
                hpu_conf_diag(
                        "hpulogc: config error: rule references undefined "
                        "output\n");
                return HPULOGC_ERR_CONFIG;
            }
        }
    }
    return 0;
}

/**
 * @brief Reject duplicate file output paths (normalized, spec 10.4).
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int check_dup_paths(hpu_conf_t* c)
{
    size_t i, k;

    for (i = 0; i < c->output_count; i++) {
        char na[HPULOGC_MAX_PATH_LEN];
        char nb[HPULOGC_MAX_PATH_LEN];

        if (c->outputs[i].pub.type != HPULOGC_OUT_FILE) {
            continue;
        }
        if (hpu_path_normalize(c->outputs[i].path_buf, na, sizeof(na)) != 0) {
            return HPULOGC_ERR_CONFIG;
        }
        for (k = i + 1; k < c->output_count; k++) {
            if (c->outputs[k].pub.type != HPULOGC_OUT_FILE) {
                continue;
            }
            if (hpu_path_normalize(c->outputs[k].path_buf, nb,
                                   sizeof(nb)) != 0) {
                return HPULOGC_ERR_CONFIG;
            }
            if (strcmp(na, nb) == 0) {
                hpu_conf_diag(
                        "hpulogc: config error: duplicate file output path "
                        "'%s'\n",
                        c->outputs[i].path_buf);
                return HPULOGC_ERR_CONFIG;
            }
        }
    }
    return 0;
}

/**
 * @brief Check that every rotate naming template contains {index} or
 *        {timestamp} (spec 10.4).
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int check_naming(hpu_conf_t* c)
{
    size_t i;

    for (i = 0; i < c->output_count; i++) {
        const char* naming = c->outputs[i].pub.rotate_naming;

        if (naming == NULL) {
            continue;
        }
        if (strstr(naming, "{index}") == NULL &&
            strstr(naming, "{timestamp}") == NULL) {
            hpu_conf_diag(
                    "hpulogc: config error: rotate naming template needs "
                    "{index} or {timestamp}\n");
            return HPULOGC_ERR_CONFIG;
        }
    }
    return 0;
}

/**
 * @brief Reject consumer thread counts unsupported by this build
 *        (spec rd_v0.3 1.3): more than one consumer requires the MPMC
 *        concurrency build, and any consumer requires an async build.
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int check_consumer_threads(hpu_conf_t* c)
{
    if (c->consumer_threads <= 1) {
        return 0;
    }
#if !HPULOGC_ENABLE_ASYNC
    hpu_conf_diag(
            "hpulogc: config error: consumer threads > 1 requires an "
            "async build\n");
    return HPULOGC_ERR_CONFIG;
#elif !defined(HPULOGC_CONCURRENCY_MPMC)
    hpu_conf_diag(
            "hpulogc: config error: consumer threads > 1 requires the "
            "MPMC concurrency build\n");
    return HPULOGC_ERR_CONFIG;
#else
    (void)c;
    return 0;
#endif
}

/**
 * @brief Reject overflow policies unsupported by this build (spec 4.3).
 * @return 0 or HPULOGC_ERR_CONFIG.
 */
static int check_overflow_policy(hpu_conf_t* c)
{
#if !HPULOGC_ENABLE_ASYNC
    /* Synchronous pipeline: the ring is not used; policies accepted. */
    (void)c;
    return 0;
#else
#if HPULOGC_LOCKFREE
    if (c->overflow_policy == HPULOGC_OVERFLOW_WAIT) {
        hpu_conf_diag(
                "hpulogc: config error: overflow policy 'wait' is not "
                "available in lock-free builds\n");
        return HPULOGC_ERR_CONFIG;
    }
#if defined(HPULOGC_CONCURRENCY_MSPC) || defined(HPULOGC_CONCURRENCY_MPMC)
    if (c->overflow_policy == HPULOGC_OVERFLOW_OVERWRITE) {
        hpu_conf_diag(
                "hpulogc: config error: overflow policy 'overwrite' is not "
                "available in lock-free MPSC/MPMC builds\n");
        return HPULOGC_ERR_CONFIG;
    }
#endif
#else
    (void)c;
#endif
    return 0;
#endif
}

/**
 * @brief Open every file output that has no handle yet.
 * @return 0 or HPULOGC_ERR_IO / HPULOGC_ERR_NO_MEM.
 */
static int open_outputs(hpu_conf_t* c)
{
    size_t i;
    int err_out;

    for (i = 0; i < c->output_count; i++) {
        int sev = hpu_fsync_severity(c->crash_safety);

        if (c->outputs[i].pub.type == HPULOGC_OUT_FILE &&
            c->outputs[i].pub.fsync) {
            sev = HPU_FSYNC_SEV_ENTRY; /* per-output fsync is stricter */
        }
        if (c->outputs[i].handle != NULL) {
            continue; /* reused from the previous generation */
        }
        err_out = HPULOGC_OK;
        c->outputs[i].handle =
            hpu_output_open_from_conf(&c->outputs[i], sev, c->use_utc,
                                      c->flush_interval_ms, c->batch_size,
                                      &err_out);
        if (c->outputs[i].handle == NULL) {
            if (err_out == HPULOGC_ERR_CONFIG) {
                if (!c->strict_init) {
                    /* lenient: unknown key / bad value degrades to a
                     * disabled sink (spec 10.1 unknown-key flow) */
                    hpu_conf_diag(
                            "hpulogc: init error: invalid definition for "
                            "output '%s' ignored (lenient)\n",
                            c->outputs[i].name_buf);
                    continue;
                }
                hpu_conf_diag(
                        "hpulogc: init error: invalid definition for "
                        "output '%s'\n",
                        c->outputs[i].name_buf);
                return HPULOGC_ERR_CONFIG;
            }
            hpu_conf_diag(
                    "hpulogc: init error: cannot open output '%s' (%s)\n",
                    c->outputs[i].path_buf, strerror(errno));
            return err_out != HPULOGC_OK ? err_out : HPULOGC_ERR_IO;
        }
    }
    return 0;
}

static int hpu_conf_finalize_impl(hpu_conf_t* c, int dry_run);

int hpu_conf_finalize(hpu_conf_t* c)
{
    return hpu_conf_finalize_impl(c, 0);
}

int hpu_conf_finalize_dry_run(hpu_conf_t* c)
{
    return hpu_conf_finalize_impl(c, 1);
}

static int hpu_conf_finalize_impl(hpu_conf_t* c, int dry_run)
{
    int rc;

    if (hpu_conf_find_format(c, c->default_format) == NULL) {
        hpu_conf_diag(
                "hpulogc: config error: default format '%s' undefined\n",
                c->default_format);
        return HPULOGC_ERR_CONFIG;
    }
    rc = check_overflow_policy(c);
    if (rc != 0) {
        return rc;
    }
    rc = check_consumer_threads(c);
    if (rc != 0) {
        return rc;
    }
    rc = check_dup_paths(c);
    if (rc != 0) {
        return rc;
    }
    rc = check_naming(c);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_default_outputs(c);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_rules(c);
    if (rc != 0) {
        return rc;
    }

    /* derived renderer environment */
    c->env.time_format = c->time_format;
    c->env.use_utc = c->use_utc;
    c->env.timestamp_source = c->timestamp_source;
    c->env.newline_style = c->newline_style;
    c->env.pid_fmt = c->pid_fmt;
    c->env.tid_fmt = c->tid_fmt;
    c->env.source_loc_enabled = c->capture_source_loc;
    c->env.mono_base_us = (int64_t)(hpu_now_ns() / 1000ULL);
    c->env.pid = hpu_getpid();

    if (!dry_run) {
        rc = open_outputs(c);
        if (rc != 0) {
            hpu_conf_close_outputs(c);
            return rc;
        }
    }

    if (c->output_count == 0 && c->default_output_count == 0) {
        hpu_conf_diag(
                "hpulogc: warning: no outputs defined and no default "
                "outputs; logs will be discarded\n");
    }
    return 0;
}

int hpu_conf_finalize_reload(hpu_conf_t* fresh, hpu_conf_t* old,
                              int reuse_old_idx[HPULOGC_MAX_OUTPUTS])
{
    size_t i;
    int rc;
    int err_out;

    if (hpu_conf_find_format(fresh, fresh->default_format) == NULL) {
        hpu_conf_diag(
                "hpulogc: config error: default format '%s' undefined\n",
                fresh->default_format);
        return HPULOGC_ERR_CONFIG;
    }
    rc = check_overflow_policy(fresh);
    if (rc != 0) {
        return rc;
    }
    rc = check_consumer_threads(fresh);
    if (rc != 0) {
        return rc;
    }
    rc = check_dup_paths(fresh);
    if (rc != 0) {
        return rc;
    }
    rc = check_naming(fresh);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_default_outputs(fresh);
    if (rc != 0) {
        return rc;
    }
    rc = resolve_rules(fresh);
    if (rc != 0) {
        return rc;
    }

    /* Renderer environment: carry over the captured init timestamps. */
    fresh->env = old->env;
    fresh->env.time_format = fresh->time_format;
    fresh->env.use_utc = fresh->use_utc;
    fresh->env.timestamp_source = fresh->timestamp_source;
    fresh->env.newline_style = fresh->newline_style;
    fresh->env.pid_fmt = fresh->pid_fmt;
    fresh->env.tid_fmt = fresh->tid_fmt;
    fresh->env.source_loc_enabled = fresh->capture_source_loc;

    /* fd-reuse plan: match by path + full parameter equality. Old is
     * read-only here; the handle move happens under the write lock. */
    for (i = 0; i < fresh->output_count; i++) {
        hpu_conf_output_t* fo = &fresh->outputs[i];
        size_t j;
        int found = -1;

        reuse_old_idx[i] = -1;
        if (fo->pub.type != HPULOGC_OUT_FILE) {
            continue;
        }
        for (j = 0; j < old->output_count; j++) {
            hpu_conf_output_t* oo = &old->outputs[j];

            if (oo->handle == NULL || oo->pub.type != HPULOGC_OUT_FILE) {
                continue;
            }
            if (strcmp(oo->path_buf, fo->path_buf) == 0 &&
                oo->pub.rotate == fo->pub.rotate &&
                oo->pub.max_size == fo->pub.max_size &&
                oo->pub.time_unit == fo->pub.time_unit &&
                oo->pub.max_files == fo->pub.max_files &&
                oo->pub.fsync == fo->pub.fsync &&
                oo->pub.symlink_latest == fo->pub.symlink_latest &&
                oo->pub.file_mode == fo->pub.file_mode &&
                oo->pub.dir_mode == fo->pub.dir_mode &&
                (oo->pub.rotate_naming == fo->pub.rotate_naming ||
                 (oo->pub.rotate_naming != NULL &&
                  fo->pub.rotate_naming != NULL &&
                  strcmp(oo->pub.rotate_naming, fo->pub.rotate_naming) ==
                      0))) {
                found = (int)j;
                break;
            }
        }
        reuse_old_idx[i] = found;
    }

    /* Open everything that is not reused (fail-fast with rollback of the
     * newly opened handles only; reused handles are still owned by the
     * old generation and stay open). */
    {
        int opened[HPULOGC_MAX_OUTPUTS];
        size_t opened_count = 0;

        for (i = 0; i < fresh->output_count; i++) {
            int sev = hpu_fsync_severity(fresh->crash_safety);
            hpu_conf_output_t* fo = &fresh->outputs[i];

            if (fo->pub.type == HPULOGC_OUT_FILE && fo->pub.fsync) {
                sev = HPU_FSYNC_SEV_ENTRY;
            }
            if (reuse_old_idx[i] >= 0) {
                continue;
            }
            err_out = HPULOGC_OK;
            fo->handle =
                hpu_output_open_from_conf(fo, sev, fresh->use_utc,
                                          fresh->flush_interval_ms,
                                          fresh->batch_size, &err_out);
            if (fo->handle == NULL) {
                if (err_out == HPULOGC_ERR_CONFIG) {
                    hpu_conf_diag(
                            "hpulogc: reload error: invalid definition for "
                            "output '%s'\n",
                            fo->name_buf);
                    while (opened_count > 0) {
                        hpu_output_close(
                            fresh->outputs[opened[--opened_count]].handle,
                            fresh->shutdown_timeout_ms);
                        fresh->outputs[opened[opened_count]].handle = NULL;
                    }
                    return HPULOGC_ERR_CONFIG;
                }
                hpu_conf_diag(
                        "hpulogc: reload error: cannot open output '%s' "
                        "(%s)\n",
                        fo->path_buf, strerror(errno));
                while (opened_count > 0) {
                    hpu_output_close(
                        fresh->outputs[opened[--opened_count]].handle,
                        fresh->shutdown_timeout_ms);
                    fresh->outputs[opened[opened_count]].handle = NULL;
                }
                return HPULOGC_ERR_IO;
            }
            opened[opened_count++] = (int)i;
        }
    }
    return 0;
}

void hpu_conf_close_outputs(hpu_conf_t* c)
{
    size_t i;

    if (c->outputs == NULL) {
        return;
    }
    for (i = 0; i < c->output_count; i++) {
        if (c->outputs[i].handle != NULL) {
            hpu_output_close(c->outputs[i].handle, c->shutdown_timeout_ms);
            c->outputs[i].handle = NULL;
        }
    }
}

void hpu_conf_free(hpu_conf_t* c)
{
    size_t i;

    if (c == NULL) {
        return;
    }
    hpu_conf_close_outputs(c);
    for (i = 0; i < c->format_count; i++) {
        hpu_format_free(&c->formats[i]);
    }
    free(c->formats);
    free(c->outputs);
    free(c->rules);
    c->formats = NULL;
    c->outputs = NULL;
    c->rules = NULL;
    c->format_count = 0;
    c->output_count = 0;
    c->rule_count = 0;
}
