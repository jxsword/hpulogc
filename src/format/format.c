/**
 * @file format.c
 * @brief Format precompilation and hot-path line rendering.
 */

#include "format.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../platform/platform.h"

/** @brief Level tags, uppercase, indexed by hpulogc_level_t. */
static const char* const g_level_tags[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/** @brief Built-in formats, spec 10.3. */
const char* const hpu_builtin_format_names[5] = {
    "minimal", "standard", "categorized", "detailed", "json"
};

/** @brief Built-in format templates, spec 10.3. */
const char* const hpu_builtin_format_templates[5] = {
    "%level: %msg%n",
    "%time [%level] %msg%n",
    "%time [%level] [%category] %msg%n",
    "%time [%level] [pid:%pid tid:%tid] [%file:%line %func] [%category] %msg%n",
    "{\"time\":\"%time\",\"level\":\"%level\",\"category\":\"%category\","
    "\"pid\":%pid,\"tid\":%tid,\"file\":\"%file\",\"line\":%line,"
    "\"msg\":\"%msg\"}%n"
};

void hpu_fmt_cache_init(hpu_fmt_cache_t* cache)
{
    memset(cache, 0, sizeof(*cache));
}

/**
 * @brief Append bytes to a growing byte pool.
 * @return 0 on success, -1 on allocation failure.
 */
static int pool_append(char** pool, size_t* len, size_t* cap,
                       const char* bytes, size_t n)
{
    if (n == 0) {
        return 0;
    }
    if (*len + n > *cap) {
        size_t new_cap = *cap == 0 ? 64 : *cap;
        char* grown;

        while (new_cap < *len + n) {
            new_cap *= 2;
        }
        grown = realloc(*pool, new_cap);
        if (grown == NULL) {
            return -1;
        }
        *pool = grown;
        *cap = new_cap;
    }
    memcpy(*pool + *len, bytes, n);
    *len += n;
    return 0;
}

/**
 * @brief Map a placeholder letter to an action type; -1 when unknown.
 */
static int placeholder_type(char c)
{
    switch (c) {
    case 'l': return HPU_FA_LEVEL;
    case 't': return HPU_FA_TIME;
    case 'p': return HPU_FA_PID;
    case 'i': return HPU_FA_TID;
    case 'f': return HPU_FA_FILE;
    case 'n': return HPU_FA_NEWLINE;
    case 'c': return HPU_FA_CATEGORY;
    case 'm': return HPU_FA_MSG;
    default: return -1;
    }
}

/**
 * @brief Match a long placeholder name at @p s.
 * @return Action type, or -1 when no name matches.
 */
static int match_long_placeholder(const char* s, size_t* len)
{
    static const struct {
        const char* name;
        int type;
    } table[] = {
        { "%level",    HPU_FA_LEVEL },
        { "%time",     HPU_FA_TIME },
        { "%pid",      HPU_FA_PID },
        { "%tid",      HPU_FA_TID },
        { "%file",     HPU_FA_FILE },
        { "%line",     HPU_FA_LINE },
        { "%func",     HPU_FA_FUNC },
        { "%msg",      HPU_FA_MSG },
        { "%category", HPU_FA_CATEGORY },
        { "%n",        HPU_FA_NEWLINE },
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t n = strlen(table[i].name);

        if (strncmp(s, table[i].name, n) == 0) {
            /* "%l" must not swallow the prefix of "%line"/"%level" */
            if (table[i].type == HPU_FA_LEVEL && strncmp(s, "%level", 6) == 0) {
                *len = 6;
                return HPU_FA_LEVEL;
            }
            *len = n;
            return table[i].type;
        }
    }
    return -1;
}

/**
 * @brief Append one action to the growing action sequence.
 */
static int push_action(hpu_format_t* fmt, uint8_t type, uint32_t lit_off,
                       uint32_t lit_len)
{
    hpu_fmt_action_t* grown =
        realloc(fmt->actions, (fmt->action_count + 1) *
                                  sizeof(*fmt->actions));

    if (grown == NULL) {
        return -1;
    }
    fmt->actions = grown;
    fmt->actions[fmt->action_count].type = type;
    fmt->actions[fmt->action_count].lit_off = lit_off;
    fmt->actions[fmt->action_count].lit_len = lit_len;
    fmt->action_count++;
    return 0;
}

int hpu_format_compile(hpu_format_t* fmt, const char* name,
                       const char* template_)
{
    size_t name_len = strlen(name);
    const char* p = template_;
    const char* lit_start = p;
    int rc = 0;

    memset(fmt, 0, sizeof(*fmt));
    if (name_len >= sizeof(fmt->name)) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    memcpy(fmt->name, name, name_len + 1);
    fmt->is_json = (strcmp(name, "json") == 0);

    while (*p != '\0') {
        if (*p == '%') {
            size_t name_len2 = 0;
            int type = match_long_placeholder(p, &name_len2);

            if (type < 0 && p[1] == '%') {
                p += 2; /* literal %% */
                continue;
            }
            if (type < 0 && p[1] != '\0') {
                type = placeholder_type(p[1]);
                name_len2 = 2;
            }
            if (type < 0) {
                rc = HPULOGC_ERR_CONFIG;
                break;
            }
            /* flush the pending literal as its own action */
            if (p > lit_start) {
                uint32_t off = (uint32_t)fmt->pool_len;

                if (pool_append(&fmt->pool, &fmt->pool_len, &fmt->pool_cap,
                                lit_start, (size_t)(p - lit_start)) != 0) {
                    rc = HPULOGC_ERR_NO_MEM;
                    break;
                }
                if (push_action(fmt, (uint8_t)HPU_FA_LITERAL, off,
                                (uint32_t)((size_t)(p - lit_start))) != 0) {
                    rc = HPULOGC_ERR_NO_MEM;
                    break;
                }
            }
            if (push_action(fmt, (uint8_t)type, 0, 0) != 0) {
                rc = HPULOGC_ERR_NO_MEM;
                break;
            }
            p += name_len2;
            lit_start = p;
            continue;
        }
        p++;
    }

    if (rc == 0 && p > lit_start) {
        uint32_t off = (uint32_t)fmt->pool_len;

        if (pool_append(&fmt->pool, &fmt->pool_len, &fmt->pool_cap,
                        lit_start, (size_t)(p - lit_start)) != 0) {
            rc = HPULOGC_ERR_NO_MEM;
        } else if (push_action(fmt, (uint8_t)HPU_FA_LITERAL, off,
                               (uint32_t)((size_t)(p - lit_start))) != 0) {
            rc = HPULOGC_ERR_NO_MEM;
        }
    }
    if (rc != 0) {
        hpu_format_free(fmt);
    }
    return rc;
}

void hpu_format_free(hpu_format_t* fmt)
{
    if (fmt == NULL) {
        return;
    }
    free(fmt->actions);
    free(fmt->pool);
    fmt->actions = NULL;
    fmt->pool = NULL;
    fmt->action_count = 0;
    fmt->pool_len = 0;
    fmt->pool_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Bounded append cursor used while building the line.
 */
typedef struct render_buf {
    char*   p;        /*!< Write position */
    size_t  len;      /*!< Bytes written */
    size_t  limit;    /*!< Hard cap (truncation threshold) */
    int     truncated; /*!< Set when bytes were dropped at the limit */
} render_buf_t;

/**
 * @brief Append bytes, honoring the limit.
 */
static void rb_append(render_buf_t* b, const char* bytes, size_t n)
{
    size_t space = b->limit - b->len;

    if (n > space) {
        n = space;
        b->truncated = 1;
    }
    memcpy(b->p + b->len, bytes, n);
    b->len += n;
}

/**
 * @brief Append a NUL-terminated string.
 */
static void rb_append_str(render_buf_t* b, const char* s)
{
    rb_append(b, s, strlen(s));
}

/**
 * @brief Append one character.
 */
static void rb_append_ch(render_buf_t* b, char c)
{
    if (b->len < b->limit) {
        b->p[b->len++] = c;
    } else {
        b->truncated = 1;
    }
}

/**
 * @brief Append an unsigned decimal number.
 */
static void rb_append_u64(render_buf_t* b, uint64_t v)
{
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);

    if (n > 0) {
        rb_append(b, tmp, (size_t)n);
    }
}

/**
 * @brief Append a number in decimal or hexadecimal.
 * @param hex_mode Non-zero for hexadecimal (0x prefix).
 */
static void rb_append_num(render_buf_t* b, uint64_t v, int hex_mode)
{
    char tmp[32];
    int n;

    if (hex_mode) {
        n = snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)v);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
    }
    if (n > 0) {
        rb_append(b, tmp, (size_t)n);
    }
}

/**
 * @brief Injection escaping: newline characters become the literal text
 *        "\\n", ESC becomes the literal text "\\x1b" (spec 4.9 step 7).
 */
static void rb_append_escaped_injection(render_buf_t* b, const char* s,
                                        size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n' || c == '\r') {
            rb_append_str(b, "\\n");
        } else if (c == 0x1B) {
            rb_append_str(b, "\\x1b");
        } else {
            rb_append_ch(b, (char)c);
        }
    }
}

/**
 * @brief JSON string escaping for control characters and quotes.
 */
static void rb_append_escaped_json(render_buf_t* b, const char* s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':  rb_append_str(b, "\\\""); break;
        case '\\': rb_append_str(b, "\\\\"); break;
        case '\n': rb_append_str(b, "\\n"); break;
        case '\r': rb_append_str(b, "\\r"); break;
        case '\t': rb_append_str(b, "\\t"); break;
        case '\b': rb_append_str(b, "\\b"); break;
        case '\f': rb_append_str(b, "\\f"); break;
        default:
            if (c < 0x20) {
                char tmp[8];

                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                rb_append_str(b, tmp);
            } else {
                rb_append_ch(b, (char)c); /* UTF-8 bytes pass through */
            }
            break;
        }
    }
}

/**
 * @brief Newline bytes for the configured style.
 */
static const char* newline_bytes(int style, size_t* len)
{
    /* AUTO follows the platform: LF on POSIX, CRLF on Windows (spec 12). */
    int crlf = (style == HPULOGC_NEWLINE_CRLF);

#if defined(_WIN32)
    if (style == HPULOGC_NEWLINE_AUTO) {
        crlf = 1;
    }
#endif
    if (crlf) {
        *len = 2;
        return "\r\n";
    }
    *len = 1;
    return "\n";
}

/**
 * @brief Locate the fraction-of-second spec (%f or %F1..%F6) in a time
 *        format.
 *
 * %% sequences are skipped. Returns the byte index of the spec and stores
 * the number of fraction digits; -1 when the format has no fraction.
 */
static int find_frac_spec(const char* fmt, int* digits)
{
    const char* p = fmt;
    int idx = 0;

    while (*p != '\0') {
        if (*p == '%') {
            if (p[1] == '%') {
                p += 2;
                idx += 2;
                continue;
            }
            if (p[1] == 'f') {
                *digits = 6;
                return idx;
            }
            if (p[1] == 'F' && p[2] >= '1' && p[2] <= '6') {
                *digits = p[2] - '0';
                return idx;
            }
            p += 2;
            idx += 2;
            continue;
        }
        p++;
        idx++;
    }
    return -1;
}

/**
 * @brief Render the fraction-of-second digits (truncated microseconds).
 */
static void render_frac(render_buf_t* b, int64_t sub_ns, int digits)
{
    char tmp[16];
    long long micros = sub_ns / 1000;
    long long scale = 1;
    int k;
    int n;

    for (k = 0; k < 6 - digits; k++) {
        scale *= 10;
    }
    n = snprintf(tmp, sizeof(tmp), "%0*lld", digits, micros / scale);
    if (n > 0) {
        rb_append(b, tmp, (size_t)n);
    }
}

/**
 * @brief Render the time placeholder using the per-second cache.
 */
static void render_time(render_buf_t* b, const hpu_log_record_t* rec,
                        const hpu_fmt_env_t* env, hpu_fmt_cache_t* cache)
{
    if (env->timestamp_source == HPULOGC_TS_MONOTONIC) {
        /* Fixed seconds.microseconds relative to init (spec 12). */
        int64_t rel_us = rec->mono_us - env->mono_base_us;
        char tmp[48];

        if (rel_us < 0) {
            rel_us = 0;
        }
        snprintf(tmp, sizeof(tmp), "%lld.%06lld",
                 (long long)(rel_us / 1000000),
                 (long long)(rel_us % 1000000));
        rb_append_str(b, tmp);
        return;
    }

    {
        int64_t sec = rec->realtime_ns / 1000000000LL;
        int64_t sub_ns = rec->realtime_ns % 1000000000LL;

        if (!cache->valid || cache->cached_sec != sec) {
            const char* tf = env->time_format != NULL
                                 ? env->time_format
                                 : "%Y-%m-%d %H:%M:%S.%f";
            int digits = 0;
            int frac_idx = find_frac_spec(tf, &digits);
            struct tm tm_buf;
            hpu_tm_t htm;

            /* Platform contract conversion (works on POSIX and Windows);
             * epoch_sec is seconds since the Unix epoch. */
            hpu_localtime(sec, &htm, env->use_utc);
            memset(&tm_buf, 0, sizeof(tm_buf));
            tm_buf.tm_year = htm.year - 1900;
            tm_buf.tm_mon  = htm.mon - 1;
            tm_buf.tm_mday = htm.day;
            tm_buf.tm_hour = htm.hour;
            tm_buf.tm_min  = htm.min;
            tm_buf.tm_sec  = htm.sec;
            tm_buf.tm_wday = htm.wday;
            tm_buf.tm_yday = htm.yday;
            tm_buf.tm_isdst = 0;

            cache->prefix_len = 0;
            cache->suffix_len = 0;

            if (frac_idx >= 0) {
                /* Render the prefix and suffix separately (both belong to
                 * the same second, so splitting the output is exact). */
                size_t pre = (size_t)frac_idx;
                size_t spec_len = (tf[frac_idx + 1] == 'f') ? 2 : 3;
                size_t suf = strlen(tf) - pre - spec_len;
                char pfmt[HPULOGC_MAX_FMT_LEN];
                char sfmt[HPULOGC_MAX_FMT_LEN];

                if (pre < sizeof(pfmt) && suf < sizeof(sfmt)) {
                    memcpy(pfmt, tf, pre);
                    pfmt[pre] = '\0';
                    memcpy(sfmt, tf + pre + spec_len, suf);
                    sfmt[suf] = '\0';
                    cache->prefix_len =
                        strftime(cache->prefix, sizeof(cache->prefix),
                                 pfmt, &tm_buf);
                    cache->prefix[cache->prefix_len] = '\0';
                    cache->suffix_len =
                        strftime(cache->suffix, sizeof(cache->suffix),
                                 sfmt, &tm_buf);
                    cache->suffix[cache->suffix_len] = '\0';
                }
            } else {
                size_t n = strftime(cache->prefix, sizeof(cache->prefix),
                                    tf, &tm_buf);

                cache->prefix_len = n;
                cache->prefix[n] = '\0';
                cache->suffix_len = 0;
            }
            cache->cached_sec = sec;
            cache->valid = 1;
        }

        rb_append(b, cache->prefix, cache->prefix_len);
        {
            int digits = 0;

            if (find_frac_spec(env->time_format != NULL
                                   ? env->time_format
                                   : "%Y-%m-%d %H:%M:%S.%f",
                               &digits) >= 0) {
                render_frac(b, sub_ns, digits > 0 ? digits : 6);
                rb_append(b, cache->suffix, cache->suffix_len);
            }
        }
    }
}

int hpu_format_render(const hpu_format_t* fmt, const hpu_log_record_t* rec,
                      const hpu_fmt_env_t* env, hpu_fmt_cache_t* cache,
                      int escape_injection, size_t max_line,
                      const char* marker, char* out, size_t out_size,
                      size_t* out_len)
{
    render_buf_t b;
    size_t i;
    size_t nl_len;
    const char* nl;
    size_t marker_len = marker != NULL ? strlen(marker) : 0;

    if (out == NULL || out_size < max_line + 1) {
        return HPULOGC_ERR_INVALID_ARG;
    }

    nl = newline_bytes(env->newline_style, &nl_len);
    b.p = out;
    b.len = 0;
    b.limit = max_line;
    b.truncated = 0;

    for (i = 0; i < fmt->action_count; i++) {
        const hpu_fmt_action_t* a = &fmt->actions[i];

        switch (a->type) {
        case HPU_FA_LITERAL:
            rb_append(&b, fmt->pool + a->lit_off, a->lit_len);
            break;
        case HPU_FA_LEVEL:
            if (rec->level >= 0 && rec->level <= 5) {
                rb_append_str(&b, g_level_tags[rec->level]);
            }
            break;
        case HPU_FA_TIME:
            render_time(&b, rec, env, cache);
            break;
        case HPU_FA_PID:
            if (fmt->is_json) {
                rb_append_num(&b, (uint64_t)env->pid, 0); /* forced decimal */
            } else if (env->pid_fmt != 2) {
                rb_append_num(&b, (uint64_t)env->pid, env->pid_fmt == 1);
            }
            break;
        case HPU_FA_TID:
            if (fmt->is_json) {
                rb_append_num(&b, rec->tid, 0); /* forced decimal */
            } else if (env->tid_fmt != 2) {
                rb_append_num(&b, rec->tid, env->tid_fmt == 1);
            }
            break;
        case HPU_FA_FILE:
            if (env->source_loc_enabled && rec->file != NULL) {
                if (fmt->is_json) {
                    rb_append_escaped_json(&b, rec->file, rec->file_len);
                } else {
                    rb_append(&b, rec->file, rec->file_len);
                }
            }
            break;
        case HPU_FA_LINE:
            if (env->source_loc_enabled) {
                rb_append_num(&b, (uint64_t)(rec->line < 0
                                                 ? 0
                                                 : (uint64_t)rec->line), 0);
            }
            break;
        case HPU_FA_FUNC:
            if (env->source_loc_enabled && rec->func != NULL) {
                if (fmt->is_json) {
                    rb_append_escaped_json(&b, rec->func, rec->func_len);
                } else {
                    rb_append(&b, rec->func, rec->func_len);
                }
            }
            break;
        case HPU_FA_MSG:
            if (fmt->is_json) {
                rb_append_escaped_json(&b, rec->msg, rec->msg_len);
            } else if (escape_injection) {
                rb_append_escaped_injection(&b, rec->msg, rec->msg_len);
            } else {
                rb_append(&b, rec->msg, rec->msg_len);
            }
            break;
        case HPU_FA_CATEGORY:
            if (rec->category != NULL && rec->category_len > 0) {
                if (fmt->is_json) {
                    rb_append_escaped_json(&b, rec->category,
                                           rec->category_len);
                } else {
                    rb_append(&b, rec->category, rec->category_len);
                }
            }
            /* NULL/empty category expands to the empty string (spec 4.9) */
            break;
        case HPU_FA_NEWLINE:
            rb_append(&b, nl, nl_len);
            break;
        default:
            break;
        }
    }

    /* Full-line truncation (spec 9): line + marker + newline <= max_line */
    if (b.truncated && b.len >= max_line) {
        size_t content = max_line;

        if (content > marker_len + nl_len) {
            content -= marker_len + nl_len;
        } else {
            content = 0;
            marker_len = 0;
        }
        b.len = content;
        if (marker_len > 0) {
            memcpy(out + b.len, marker, marker_len);
            b.len += marker_len;
        }
        memcpy(out + b.len, nl, nl_len);
        b.len += nl_len;
    }

    out[b.len] = '\0';
    *out_len = b.len;
    return 0;
}
