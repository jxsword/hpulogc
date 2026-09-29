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
    case 'g': return HPU_FA_TIME_UTC;
    case 'p': return HPU_FA_PID;
    case 'i': return HPU_FA_TID;
    case 'f': return HPU_FA_FILE;
    case 'n': return HPU_FA_NEWLINE;
    case 'c': return HPU_FA_CATEGORY;
    case 'm': return HPU_FA_MSG;
    case 'v': return HPU_FA_FIELDS;
    default: return -1;
    }
}

/**
 * @brief Match a long placeholder name (without its '%') at @p s.
 * @return Action type, or -1 when no name matches.
 */
static int match_long_placeholder(const char* s, size_t* len)
{
    static const struct {
        const char* name;
        int type;
    } table[] = {
        { "level",    HPU_FA_LEVEL },
        { "time",     HPU_FA_TIME },
        { "pid",      HPU_FA_PID },
        { "tid",      HPU_FA_TID },
        { "file",     HPU_FA_FILE },
        { "line",     HPU_FA_LINE },
        { "func",     HPU_FA_FUNC },
        { "msg",      HPU_FA_MSG },
        { "category", HPU_FA_CATEGORY },
        { "n",        HPU_FA_NEWLINE },
    };
    size_t i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        size_t n = strlen(table[i].name);

        if (strncmp(s, table[i].name, n) == 0) {
            *len = n;
            return table[i].type;
        }
    }
    return -1;
}

/**
 * @brief Parsed printf-style width/precision modifier (spec 12, v0.6.4).
 */
typedef struct fmt_modifier {
    int    left;     /*!< '-' flag present (left-align) */
    int    overflow; /*!< Parsed value exceeded HPU_FMT_MOD_LIMIT */
    int    width;    /*!< Field width value (0 = absent) */
    int    has_prec; /*!< Precision part present */
    int    prec;     /*!< Precision value (max output bytes) */
    size_t len;      /*!< Consumed bytes after '%' */
} fmt_modifier_t;

/**
 * @brief Parse the modifier part of a placeholder: %[-][width][.prec].
 *
 * Only '-' is a valid flag; width/precision are decimal numbers capped at
 * HPU_FMT_MOD_LIMIT (values beyond set @p overflow). Unsupported spellings
 * (other flags, negative precision) fall out as either no-consumption or
 * overflow, both of which the caller rejects.
 *
 * @param s  Position right after '%' (not consumed by the caller yet).
 * @param m  Filled with the parsed parts.
 * @return   Number of modifier bytes consumed (0 = plain placeholder).
 */
static size_t parse_modifier(const char* s, fmt_modifier_t* m)
{
    const char* q = s;

    memset(m, 0, sizeof(*m));
    if (*q == '-') {
        m->left = 1;
        q++;
    }
    while (*q >= '0' && *q <= '9') {
        m->width = m->width * 10 + (*q - '0');
        if (m->width > HPU_FMT_MOD_LIMIT) {
            m->overflow = 1;
        }
        q++;
    }
    if (*q == '.') {
        m->has_prec = 1;
        q++;
        while (*q >= '0' && *q <= '9') {
            m->prec = m->prec * 10 + (*q - '0');
            if (m->prec > HPU_FMT_MOD_LIMIT) {
                m->overflow = 1;
            }
            q++;
        }
    }
    m->len = (size_t)(q - s);
    return m->len;
}

/**
 * @brief Whether an action type accepts width/precision modifiers.
 *
 * Spec 12: only the ten value placeholders; %n / %% / %v reject them.
 */
static int modifier_supported(int type)
{
    switch (type) {
    case HPU_FA_LEVEL:
    case HPU_FA_TIME:
    case HPU_FA_TIME_UTC:
    case HPU_FA_PID:
    case HPU_FA_TID:
    case HPU_FA_FILE:
    case HPU_FA_LINE:
    case HPU_FA_FUNC:
    case HPU_FA_MSG:
    case HPU_FA_CATEGORY:
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief Append one action to the growing action sequence.
 */
static int push_action(hpu_format_t* fmt, uint8_t type, uint32_t lit_off,
                       uint32_t lit_len, const fmt_modifier_t* mod)
{
    hpu_fmt_action_t* grown =
        realloc(fmt->actions, (fmt->action_count + 1) *
                                  sizeof(*fmt->actions));

    if (grown == NULL) {
        return -1;
    }
    fmt->actions = grown;
    fmt->actions[fmt->action_count].type = type;
    fmt->actions[fmt->action_count].flags = (uint8_t)
        ((mod != NULL && mod->left ? HPU_FMT_MOD_LEFT : 0) |
         (mod != NULL && mod->has_prec ? HPU_FMT_MOD_PREC : 0));
    fmt->actions[fmt->action_count].width =
        (uint16_t)(mod != NULL ? mod->width : 0);
    fmt->actions[fmt->action_count].precision =
        (uint16_t)(mod != NULL ? mod->prec : 0);
    fmt->actions[fmt->action_count].lit_off = lit_off;
    fmt->actions[fmt->action_count].lit_len = lit_len;
    fmt->action_count++;
    return 0;
}

/**
 * @brief Splice a HPU_FA_FIELDS action before the final closing brace of a
 *        json template (§4.11.5).
 *
 * Finds the LAST literal action containing '}' (the template's closing
 * brace for the builtin json shape) and splits it into
 * [literal prefix][FIELDS][literal suffix starting at the brace]. The
 * FIELDS action emits nothing for field-less records (output stays
 * byte-identical) and `,"fields":{...}` otherwise, so the member lands
 * inside the object right before its closing brace.
 *
 * @return 0 on success (or no brace found), -1 on allocation failure.
 */
static int json_splice_fields_action(hpu_format_t* fmt)
{
    size_t i;

    for (i = fmt->action_count; i-- > 0; ) {
        hpu_fmt_action_t* a = &fmt->actions[i];
        size_t k;

        if (a->type != HPU_FA_LITERAL || a->lit_len == 0) {
            continue;
        }
        for (k = a->lit_len; k-- > 0; ) {
            char tmp[HPULOGC_MAX_FMT_LEN];
            size_t suffix_len;
            size_t suffix_off;
            hpu_fmt_action_t* grown;

            if (fmt->pool[a->lit_off + k] != '}') {
                continue;
            }
            suffix_len = a->lit_len - k; /* suffix INCLUDES the brace */
            if (suffix_len > sizeof(tmp)) {
                return -1; /* template shape not supported for injection */
            }
            memcpy(tmp, fmt->pool + a->lit_off + k, suffix_len);
            if (pool_append(&fmt->pool, &fmt->pool_len, &fmt->pool_cap,
                            tmp, suffix_len) != 0) {
                return -1;
            }
            suffix_off = fmt->pool_len - suffix_len;
            grown = realloc(fmt->actions, (fmt->action_count + 2) *
                                               sizeof(*fmt->actions));
            if (grown == NULL) {
                return -1;
            }
            fmt->actions = grown;
            memmove(&fmt->actions[i + 3], &fmt->actions[i + 1],
                    (fmt->action_count - i - 1) * sizeof(*fmt->actions));
            fmt->actions[i].lit_len = (uint32_t)k; /* prefix stays in place */
            fmt->actions[i + 1].type = (uint8_t)HPU_FA_FIELDS;
            fmt->actions[i + 1].flags = 0;
            fmt->actions[i + 1].width = 0;
            fmt->actions[i + 1].precision = 0;
            fmt->actions[i + 1].lit_off = 0;
            fmt->actions[i + 1].lit_len = 0;
            fmt->actions[i + 2].type = (uint8_t)HPU_FA_LITERAL;
            fmt->actions[i + 2].flags = 0;
            fmt->actions[i + 2].width = 0;
            fmt->actions[i + 2].precision = 0;
            fmt->actions[i + 2].lit_off = (uint32_t)suffix_off;
            fmt->actions[i + 2].lit_len = (uint32_t)suffix_len;
            fmt->action_count += 2;
            return 0;
        }
        /* literal without '}': keep scanning earlier actions */
    }
    return 0; /* no closing brace: no injection point */
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
            fmt_modifier_t mod;
            size_t mod_len = parse_modifier(p + 1, &mod);
            const char* body = p + 1 + mod_len;
            size_t ph_len = 0;
            int type = match_long_placeholder(body, &ph_len);

            if (type < 0 && body[0] != '\0' && body[0] != '%') {
                /* single-letter placeholder */
                type = placeholder_type(body[0]);
                ph_len = 1;
            }
            if (type < 0 && mod_len == 0 && body[0] == '%') {
                p += 2; /* literal %% */
                continue;
            }
            /* With modifiers the next token must be a supported value
             * placeholder spelled within the modifier limits (spec 12). */
            if (type < 0 || (mod_len > 0 &&
                             (!modifier_supported(type) || mod.overflow))) {
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
                                (uint32_t)((size_t)(p - lit_start)),
                                NULL) != 0) {
                    rc = HPULOGC_ERR_NO_MEM;
                    break;
                }
            }
            if (push_action(fmt, (uint8_t)type, 0, 0,
                            mod_len > 0 ? &mod : NULL) != 0) {
                rc = HPULOGC_ERR_NO_MEM;
                break;
            }
            p += 1 + mod_len + ph_len;
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
                               (uint32_t)((size_t)(p - lit_start)),
                               NULL) != 0) {
            rc = HPULOGC_ERR_NO_MEM;
        }
    }
    if (rc == 0 && fmt->is_json && json_splice_fields_action(fmt) != 0) {
        rc = HPULOGC_ERR_NO_MEM;
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
 *
 * @param use_utc  Non-zero renders UTC (the %g placeholder), zero follows
 *                 the configured timezone (env->use_utc).
 */
static void render_time(render_buf_t* b, const hpu_log_record_t* rec,
                        const hpu_fmt_env_t* env, hpu_fmt_cache_t* cache,
                        int use_utc)
{
    hpu_fmt_cache_slot_t* slot = &cache->slot[use_utc ? 1 : 0];

    if (env->timestamp_source == HPULOGC_TS_MONOTONIC) {
        /* Fixed seconds.microseconds relative to init (spec 12); the same
         * output for %time and %g (UTC is meaningless for monotonic). */
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

        if (!slot->valid || slot->cached_sec != sec) {
            const char* tf = env->time_format != NULL
                                 ? env->time_format
                                 : "%Y-%m-%d %H:%M:%S.%f";
            int digits = 0;
            int frac_idx = find_frac_spec(tf, &digits);
            struct tm tm_buf;
            hpu_tm_t htm;

            /* Platform contract conversion (works on POSIX and Windows);
             * epoch_sec is seconds since the Unix epoch. */
            hpu_localtime(sec, &htm, use_utc);
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

            slot->prefix_len = 0;
            slot->suffix_len = 0;

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
                    slot->prefix_len =
                        strftime(slot->prefix, sizeof(slot->prefix),
                                 pfmt, &tm_buf);
                    slot->prefix[slot->prefix_len] = '\0';
                    slot->suffix_len =
                        strftime(slot->suffix, sizeof(slot->suffix),
                                 sfmt, &tm_buf);
                    slot->suffix[slot->suffix_len] = '\0';
                }
            } else {
                size_t n = strftime(slot->prefix, sizeof(slot->prefix),
                                    tf, &tm_buf);

                slot->prefix_len = n;
                slot->prefix[n] = '\0';
                slot->suffix_len = 0;
            }
            slot->cached_sec = sec;
            slot->valid = 1;
        }

        rb_append(b, slot->prefix, slot->prefix_len);
        {
            int digits = 0;

            if (find_frac_spec(env->time_format != NULL
                                   ? env->time_format
                                   : "%Y-%m-%d %H:%M:%S.%f",
                               &digits) >= 0) {
                render_frac(b, sub_ns, digits > 0 ? digits : 6);
                rb_append(b, slot->suffix, slot->suffix_len);
            }
        }
    }
}

/* ---- Structured fields (§4.11) ------------------------------------ */

/**
 * @brief Render one wire field into the line buffer (json or text style).
 */
static void fields_render_one(render_buf_t* b, const char* key,
                              size_t key_len, int type, const void* val,
                              size_t val_len, int is_json,
                              int escape_injection, int first)
{
    char tmp[40];

    if (is_json) {
        if (!first) {
            rb_append_str(b, ",");
        }
        rb_append_str(b, "\"");
        rb_append_escaped_json(b, key, key_len);
        rb_append_str(b, "\":");
    } else {
        if (!first) {
            rb_append_str(b, " ");
        }
        if (escape_injection) {
            rb_append_escaped_injection(b, key, key_len);
        } else {
            rb_append(b, key, key_len);
        }
        rb_append_str(b, "=");
    }

    switch (type) {
    case HPULOGC_FIELD_I64: {
        int64_t v;

        memcpy(&v, val, sizeof(v));
        snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
        rb_append_str(b, tmp);
        break;
    }
    case HPULOGC_FIELD_U64: {
        uint64_t v;

        memcpy(&v, val, sizeof(v));
        snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
        rb_append_str(b, tmp);
        break;
    }
    case HPULOGC_FIELD_F64: {
        double v;

        memcpy(&v, val, sizeof(v));
        snprintf(tmp, sizeof(tmp), "%.17g", v);
        rb_append_str(b, tmp);
        break;
    }
    case HPULOGC_FIELD_BOOL: {
        int v = *(const int*)val != 0;

        if (is_json) {
            rb_append_str(b, v ? "true" : "false");
        } else {
            rb_append_str(b, v ? "true" : "false");
        }
        break;
    }
    case HPULOGC_FIELD_STR:
        if (is_json) {
            rb_append_str(b, "\"");
            rb_append_escaped_json(b, (const char*)val, val_len);
            rb_append_str(b, "\"");
        } else if (escape_injection) {
            rb_append_escaped_injection(b, (const char*)val, val_len);
        } else {
            rb_append(b, (const char*)val, val_len);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief Render the whole field region (%v / json "fields" member).
 *
 * json emits `,"fields":{...}` (leading comma, empty when no fields);
 * text emits `k1=v1 k2=v2` (nothing when no fields).
 */
static void fields_render(render_buf_t* b, const hpu_log_record_t* rec,
                          int is_json, int escape_injection)
{
    size_t off = 0;
    uint16_t i;
    int first = 1;

    if (is_json) {
        rb_append_str(b, ",\"fields\":{");
    }
    for (i = 0; i < rec->field_count && rec->fields_wire != NULL; i++) {
        uint16_t key_len;
        uint16_t val_len;
        uint8_t type;

        if (off + 6 > rec->fields_len) {
            break; /* corrupt region: stop (defensive) */
        }
        memcpy(&key_len, rec->fields_wire + off, 2);
        type = rec->fields_wire[off + 2];
        memcpy(&val_len, rec->fields_wire + off + 4, 2);
        off += 6;
        if (off + (size_t)key_len + val_len > rec->fields_len) {
            break;
        }
        fields_render_one(b, (const char*)rec->fields_wire + off, key_len,
                          type, rec->fields_wire + off + key_len, val_len,
                          is_json, escape_injection, first);
        first = 0;
        off += (size_t)key_len + val_len;
    }
    if (is_json) {
        rb_append_str(b, "}");
    }
}

int hpu_fields_serialize(const hpulogc_field_t* fields, size_t n,
                         size_t budget, uint8_t* out, size_t cap,
                         size_t* out_len, uint16_t* out_cnt,
                         unsigned long long* dropped)
{
    size_t len = 0;
    uint16_t cnt = 0;
    unsigned long long drop = 0;
    size_t i;
    int budget_hit = 0;

    if (out == NULL || cap == 0 || out_len == NULL || out_cnt == NULL) {
        return HPULOGC_ERR_INVALID_ARG;
    }
    for (i = 0; fields != NULL && i < n; i++) {
        const char* key = fields[i].key;
        size_t key_len = key != NULL ? strlen(key) : 0;
        size_t val_len = 0;
        const void* val = NULL;
        uint8_t type = 0;
        size_t need;

        if (cnt >= HPULOGC_MAX_FIELDS || budget_hit) {
            drop++; /* count budget exhausted / tail beyond budget */
            continue;
        }
        if (key == NULL) {
            drop++;
            continue;
        }
        if (key_len > HPULOGC_MAX_FIELD_KEY_LEN) {
            drop++;
            continue;
        }
        switch (fields[i].value.type) {
        case HPULOGC_FIELD_I64:
            type = HPULOGC_FIELD_I64;
            val = &fields[i].value.v.i64;
            val_len = 8;
            break;
        case HPULOGC_FIELD_U64:
            type = HPULOGC_FIELD_U64;
            val = &fields[i].value.v.u64;
            val_len = 8;
            break;
        case HPULOGC_FIELD_F64:
            type = HPULOGC_FIELD_F64;
            val = &fields[i].value.v.f64;
            val_len = 8;
            break;
        case HPULOGC_FIELD_BOOL:
            type = HPULOGC_FIELD_BOOL;
            val = &fields[i].value.v.b;
            val_len = 1;
            break;
        case HPULOGC_FIELD_STR:
            type = HPULOGC_FIELD_STR;
            val = fields[i].value.v.str.s;
            val_len = fields[i].value.v.str.len;
            if (val == NULL) {
                val_len = 0;
            } else if (val_len > HPULOGC_MAX_FIELD_STR_LEN) {
                val_len = HPULOGC_MAX_FIELD_STR_LEN; /* truncate */
                drop++;
            }
            break;
        default:
            drop++; /* invalid type tag */
            continue;
        }
        need = 6 + key_len + val_len;
        if (len + need > budget || len + need > cap) {
            budget_hit = 1; /* this and all remaining fields are dropped */
            drop++;
            continue;
        }
        memcpy(out + len, &key_len, 2);
        out[len + 2] = type;
        out[len + 3] = 0;
        memcpy(out + len + 4, &val_len, 2);
        memcpy(out + len + 6, key, key_len);
        memcpy(out + len + 6 + key_len, val, val_len);
        len += need;
        cnt++;
    }
    while (len % 8 != 0 && len < cap) {
        out[len++] = 0; /* pad the region to the record alignment */
    }
    *out_len = len;
    *out_cnt = cnt;
    if (dropped != NULL) {
        *dropped = drop;
    }
    return 0;
}

void hpu_fields_iterate(const uint8_t* wire, size_t len, uint16_t count,
                        hpu_fields_iter_cb cb, void* ud)
{
    size_t off = 0;
    uint16_t i;

    if (wire == NULL || len == 0 || cb == NULL) {
        return;
    }
    for (i = 0; i < count && off + 6 <= len; i++) {
        uint16_t key_len;
        uint16_t val_len;
        uint8_t type;

        memcpy(&key_len, wire + off, 2);
        type = wire[off + 2];
        memcpy(&val_len, wire + off + 4, 2);
        off += 6;
        if (off + (size_t)key_len + val_len > len) {
            return; /* corrupt region: stop (defensive) */
        }
        if (cb(ud, (const char*)wire + off, key_len, type,
               wire + off + key_len, val_len) != 0) {
            return;
        }
        off += (size_t)key_len + val_len;
    }
}

size_t hpu_fields_unpack(const uint8_t* wire, size_t len, uint16_t count,
                         hpulogc_field_t* out, size_t max)
{
    size_t off = 0;
    size_t n = 0;
    uint16_t i;

    if (wire == NULL || len == 0 || out == NULL) {
        return 0;
    }
    for (i = 0; i < count && n < max && off + 6 <= len; i++) {
        uint16_t key_len;
        uint16_t val_len;
        uint8_t type;

        memcpy(&key_len, wire + off, 2);
        type = wire[off + 2];
        memcpy(&val_len, wire + off + 4, 2);
        off += 6;
        if (off + (size_t)key_len + val_len > len) {
            break; /* corrupt region: stop (defensive) */
        }
        out[n].key = (const char*)wire + off;
        out[n].value.type = (hpulogc_field_type_t)type;
        if (type == HPULOGC_FIELD_STR) {
            out[n].value.v.str.s = (const char*)wire + off + key_len;
            out[n].value.v.str.len = val_len;
        } else {
            /* zero first: BOOL carries 1 wire byte into a wider int */
            memset(&out[n].value.v, 0, sizeof(out[n].value.v));
            memcpy(&out[n].value.v, wire + off + key_len, val_len);
        }
        n++;
        off += (size_t)key_len + val_len;
    }
    return n;
}

/**
 * @brief Apply width/precision modifiers to a rendered placeholder.
 *
 * Spec 12 (v0.6.4): precision truncates the content to at most that many
 * bytes (the excess bytes are rewound - later appends overwrite them, and
 * the line-truncated flag they may have raised is rolled back); width pads
 * with spaces, right-aligned by default and left-aligned with the '-'
 * flag. Padding is skipped when it would push the line past the
 * truncation threshold.
 *
 * @param b          Line buffer (content for this action already written).
 * @param start      Buffer position where this action's content began.
 * @param pre_trunc  Value of b->truncated before the content was written.
 * @param a          The action carrying the modifier fields.
 */
static void modifier_finish(render_buf_t* b, size_t start, int pre_trunc,
                            const hpu_fmt_action_t* a)
{
    size_t written = b->len - start;

    if ((a->flags & HPU_FMT_MOD_PREC) != 0 && written > a->precision) {
        b->len = start + a->precision;
        written = a->precision;
        b->truncated = pre_trunc;
    }
    if (a->width != 0 && written < (size_t)a->width) {
        size_t pad = (size_t)a->width - written;

        if ((a->flags & HPU_FMT_MOD_LEFT) != 0) {
            if (b->len + pad <= b->limit) {
                memset(b->p + b->len, ' ', pad);
                b->len += pad;
            }
        } else if (start + written + pad <= b->limit) {
            memmove(b->p + start + pad, b->p + start, written);
            memset(b->p + start, ' ', pad);
            b->len += pad;
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
        size_t start = b.len;
        int pre_trunc = b.truncated;

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
            render_time(&b, rec, env, cache, env->use_utc);
            break;
        case HPU_FA_TIME_UTC:
            render_time(&b, rec, env, cache, 1);
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
        case HPU_FA_FIELDS:
            if (rec->field_count > 0 && rec->fields_wire != NULL) {
                fields_render(&b, rec, fmt->is_json, escape_injection);
            }
            break;
        case HPU_FA_NEWLINE:
            rb_append(&b, nl, nl_len);
            break;
        default:
            break;
        }

        if (a->flags != 0 || a->width != 0) {
            modifier_finish(&b, start, pre_trunc, a);
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
