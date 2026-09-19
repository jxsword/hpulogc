/**
 * @file conf_file.c
 * @brief Configuration file parsing: sections, keys, behavior table
 *        (spec 10.1-10.4) and hot-reload entry point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "conf_model.h"
#include "ini.h"
#include "../output/rotate.h"
#include "../platform/platform.h"

/** @brief Section ranks for the order check (spec 10.2). */
enum {
    SEC_NONE = -1,
    SEC_BUILD = 0,
    SEC_GLOBAL,
    SEC_FORMATS,
    SEC_OUTPUTS,
    SEC_BUFFER,
    SEC_ASYNC,
    SEC_THROTTLE,
    SEC_RULES,
    SEC_ADVANCED,
    SEC_COUNT
};

/** @brief Known section names in rank order. */
static const char* const g_section_names[SEC_COUNT] = {
    "build", "global", "formats", "outputs", "buffer", "async", "throttle",
    "rules", "advanced"
};

/**
 * @brief Parser context for the line callback.
 */
typedef struct conf_parser {
    hpu_conf_t* c;        /*!< Snapshot being filled */
    const char* path;     /*!< Source file (diagnostics) */
    int strict;           /*!< Effective unknown-key handling */
    int cur_section;      /*!< Current section rank, SEC_NONE at start */
    int seen[SEC_COUNT];  /*!< Sections already entered */
    int err;              /*!< First error code, stops further callbacks */
} conf_parser_t;

/**
 * @brief Report a parse error and stop.
 */
static int parse_fail(conf_parser_t* ps, int line_no, const char* msg)
{
    fprintf(stderr, "hpulogc: %s:%d: %s\n", ps->path != NULL ? ps->path : "?",
            line_no, msg);
    ps->err = HPULOGC_ERR_CONFIG;
    return ps->err;
}

/**
 * @brief Case-insensitive string equality.
 */
static int eqcase(const char* a, const char* b)
{
    return strcasecmp(a, b) == 0;
}

/**
 * @brief Parse a strict boolean (true/false, case-insensitive).
 * @return 0 ok / -1 invalid.
 */
static int parse_bool(const char* v, int* out)
{
    if (eqcase(v, "true")) {
        *out = 1;
        return 0;
    }
    if (eqcase(v, "false")) {
        *out = 0;
        return 0;
    }
    return -1;
}

/**
 * @brief Parse a level name (TRACE..OFF, case-insensitive).
 * @return 0 ok / -1 invalid.
 */
static int parse_level(const char* v, int* out)
{
    static const char* names[] = { "trace", "debug", "info", "warn",
                                   "error", "fatal", "off" };
    size_t i;

    for (i = 0; i < 7; i++) {
        if (eqcase(v, names[i])) {
            *out = (int)i;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief Parse a size with zlog suffixes (1k=1000, 1kb=1024, ...).
 * @return 0 ok / -1 invalid.
 */
static int parse_size(const char* v, unsigned long long* out)
{
    char* end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    char suf[3];
    size_t sl;

    if (end == v) {
        return -1;
    }
    sl = strlen(end);
    if (sl == 0) {
        *out = n;
        return 0;
    }
    if (sl > 2) {
        return -1;
    }
    suf[0] = (char)((end[0] >= 'A' && end[0] <= 'Z') ? end[0] + 32 : end[0]);
    suf[1] = sl > 1
                 ? (char)((end[1] >= 'A' && end[1] <= 'Z') ? end[1] + 32
                                                           : end[1])
                 : '\0';
    suf[2] = '\0';

    if (strcmp(suf, "k") == 0) {
        *out = n * 1000ULL;
    } else if (strcmp(suf, "kb") == 0) {
        *out = n * 1024ULL;
    } else if (strcmp(suf, "m") == 0) {
        *out = n * 1000000ULL;
    } else if (strcmp(suf, "mb") == 0) {
        *out = n * 1048576ULL;
    } else if (strcmp(suf, "g") == 0) {
        *out = n * 1000000000ULL;
    } else if (strcmp(suf, "gb") == 0) {
        *out = n * 1073741824ULL;
    } else {
        return -1;
    }
    return 0;
}

/**
 * @brief Strip one pair of surrounding double quotes.
 */
static void strip_quotes(char* v)
{
    size_t n = strlen(v);

    if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v[n - 1] = '\0';
        memmove(v, v + 1, n - 1);
    }
}

/**
 * @brief Split a comma-separated list outside quotes.
 *
 * @param s        Input string (modified: separators replaced by NUL).
 * @param parts    Output pointers into @p s.
 * @param max      Capacity of @p parts.
 * @return         Number of parts found.
 */
static size_t split_csv(char* s, char** parts, size_t max)
{
    size_t n = 0;
    char* p = s;
    int in_quotes = 0;

    if (*s != '\0') {
        parts[n++] = s;
    }
    while (*p != '\0' && n < max) {
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (*p == ',' && !in_quotes) {
            *p = '\0';
            parts[n++] = p + 1;
        }
        p++;
    }
    return n;
}

/**
 * @brief Trim leading/trailing whitespace in place.
 */
static char* trim(char* s)
{
    char* end;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    return s;
}

/**
 * @brief Clamp an integer with a stderr warning.
 */
static long clamp_long(conf_parser_t* ps, int line_no, long v, long lo,
                       long hi, const char* key)
{
    if (v < lo || v > hi) {
        long clamped = v < lo ? lo : hi;

        fprintf(stderr,
                "hpulogc: %s:%d: value of '%s' out of range (%ld), clamped "
                "to %ld\n",
                ps->path ? ps->path : "?", line_no, key, v, clamped);
        return clamped;
    }
    return v;
}

/**
 * @brief Parse one key=value line inside [global].
 */
static int parse_global(conf_parser_t* ps, const char* key, char* value,
                        int line_no)
{
    hpu_conf_t* c = ps->c;
    int bval;

    if (strcmp(key, "strict init") == 0) {
        if (parse_bool(value, &c->strict_init) != 0) {
            return parse_fail(ps, line_no, "invalid boolean for 'strict init'");
        }
    } else if (strcmp(key, "level") == 0) {
        if (parse_level(value, &c->level) != 0) {
            return parse_fail(ps, line_no, "invalid level");
        }
    } else if (strcmp(key, "default format") == 0) {
        snprintf(c->default_format, sizeof(c->default_format), "%s", value);
    } else if (strcmp(key, "default outputs") == 0) {
        char* parts[HPULOGC_MAX_OUTPUTS];
        size_t n = split_csv(value, parts, HPULOGC_MAX_OUTPUTS);
        size_t i;

        if (strlen(value) > 0 && n == 0) {
            n = 0;
        }
        for (i = 0; i < n; i++) {
            char* name = trim(parts[i]);

            strip_quotes(name);
            if (name[0] == '\0') {
                continue;
            }
            if (c->default_output_name_count >= HPULOGC_MAX_OUTPUTS) {
                return parse_fail(ps, line_no, "too many default outputs");
            }
            snprintf(c->default_output_names[c->default_output_name_count++],
                     HPULOGC_MAX_NAME_LEN, "%s", name);
        }
    } else if (strcmp(key, "timezone") == 0) {
        if (eqcase(value, "utc")) {
            c->use_utc = 1;
        } else if (eqcase(value, "local")) {
            c->use_utc = 0;
        } else {
            return parse_fail(ps, line_no, "invalid timezone (local|utc)");
        }
    } else if (strcmp(key, "timestamp source") == 0) {
        if (eqcase(value, "realtime")) {
            c->timestamp_source = HPULOGC_TS_REALTIME;
        } else if (eqcase(value, "monotonic")) {
            c->timestamp_source = HPULOGC_TS_MONOTONIC;
        } else {
            return parse_fail(ps, line_no,
                              "invalid timestamp source");
        }
    } else if (strcmp(key, "time format") == 0) {
        snprintf(c->time_format, sizeof(c->time_format), "%s", value);
    } else if (strcmp(key, "encoding") == 0) {
        if (!eqcase(value, "utf-8") && !eqcase(value, "utf8")) {
            return parse_fail(ps, line_no,
                              "unsupported encoding (only utf-8)");
        }
    } else if (strcmp(key, "capture source loc") == 0) {
#if HPULOGC_ENABLE_SOURCE_LOC
        if (parse_bool(value, &c->capture_source_loc) != 0) {
            return parse_fail(ps, line_no, "invalid boolean");
        }
#else
        (void)bval;
#endif
    } else if (strcmp(key, "newline") == 0) {
        if (eqcase(value, "auto")) {
            c->newline_style = HPULOGC_NEWLINE_AUTO;
        } else if (eqcase(value, "lf")) {
            c->newline_style = HPULOGC_NEWLINE_LF;
        } else if (eqcase(value, "crlf")) {
            c->newline_style = HPULOGC_NEWLINE_CRLF;
        } else {
            return parse_fail(ps, line_no, "invalid newline style");
        }
    } else if (strcmp(key, "pid format") == 0) {
        if (eqcase(value, "decimal")) {
            c->pid_fmt = HPU_ID_FMT_DECIMAL;
        } else if (eqcase(value, "hex")) {
            c->pid_fmt = HPU_ID_FMT_HEX;
        } else if (eqcase(value, "none")) {
            c->pid_fmt = HPU_ID_FMT_NONE;
        } else {
            return parse_fail(ps, line_no, "invalid pid format");
        }
    } else if (strcmp(key, "tid format") == 0) {
        if (eqcase(value, "decimal")) {
            c->tid_fmt = HPU_ID_FMT_DECIMAL;
        } else if (eqcase(value, "hex")) {
            c->tid_fmt = HPU_ID_FMT_HEX;
        } else if (eqcase(value, "none")) {
            c->tid_fmt = HPU_ID_FMT_NONE;
        } else {
            return parse_fail(ps, line_no, "invalid tid format");
        }
    } else if (strcmp(key, "hot reload interval") == 0) {
        long v = strtol(value, NULL, 10);

        c->hot_reload_interval =
            (int)clamp_long(ps, line_no, v, 0, 86400,
                            "hot reload interval");
    } else if (strcmp(key, "signal reload") == 0) {
#if HPULOGC_ENABLE_HOT_RELOAD
        if (parse_bool(value, &c->signal_reload) != 0) {
            return parse_fail(ps, line_no, "invalid boolean");
        }
#else
        (void)bval;
#endif
    } else {
        goto unknown;
    }
    return 0;

unknown:
    if (ps->strict) {
        return parse_fail(ps, line_no, "unknown key in [global]");
    }
    fprintf(stderr, "hpulogc: %s:%d: unknown key '%s' ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    (void)bval;
    return 0;
}

/**
 * @brief Parse one [formats] definition.
 */
static int parse_formats(conf_parser_t* ps, const char* key, char* value,
                         int line_no)
{
    size_t i;

    if (strlen(key) >= HPULOGC_MAX_NAME_LEN) {
        return parse_fail(ps, line_no, "format name too long");
    }
    for (i = 0; i < ps->c->format_count; i++) {
        if (strcmp(ps->c->formats[i].name, key) == 0) {
            return parse_fail(ps, line_no, "duplicate format definition");
        }
    }
    if (strlen(value) >= HPULOGC_MAX_FMT_LEN) {
        return parse_fail(ps, line_no, "format template too long (256 max)");
    }
    {
        int rc = 0;

        /* append: grow the table, then compile into the new slot */
        {
            hpu_format_t* grown =
                realloc(ps->c->formats,
                        (ps->c->format_count + 1) * sizeof(hpu_format_t));

            if (grown == NULL) {
                ps->err = HPULOGC_ERR_NO_MEM;
                return ps->err;
            }
            ps->c->formats = grown;
            memset(&ps->c->formats[ps->c->format_count], 0,
                   sizeof(hpu_format_t));
            ps->c->format_count++;
        }
        rc = hpu_format_compile(&ps->c->formats[ps->c->format_count - 1],
                                key, value);
        if (rc != 0) {
            ps->c->format_count--;
            return parse_fail(ps, line_no, "invalid format template");
        }
    }
    return 0;
}

/**
 * @brief Parse one [outputs] definition line.
 */
static int parse_outputs(conf_parser_t* ps, const char* key, char* value,
                         int line_no)
{
    hpu_conf_t* c = ps->c;
    char* parts[32];
    size_t n;
    size_t i;
    hpu_conf_output_t* o;

    if (hpu_conf_find_output(c, key) >= 0) {
        return parse_fail(ps, line_no, "duplicate output definition");
    }
    if (strlen(key) >= HPULOGC_MAX_NAME_LEN) {
        return parse_fail(ps, line_no, "output name too long");
    }
    if (c->output_count >= HPULOGC_MAX_OUTPUTS) {
        return parse_fail(ps, line_no, "too many outputs (16 max)");
    }

    {
        hpu_conf_output_t* grown =
            realloc(c->outputs, (c->output_count + 1) *
                                     sizeof(hpu_conf_output_t));

        if (grown == NULL) {
            ps->err = HPULOGC_ERR_NO_MEM;
            return ps->err;
        }
        c->outputs = grown;
        o = &c->outputs[c->output_count];
        memset(o, 0, sizeof(*o));
        snprintf(o->name_buf, sizeof(o->name_buf), "%s", key);
        c->output_count++;
    }

    o->pub.file_mode = 0644;
    o->pub.dir_mode = 0755;
    o->pub.rotate_naming = NULL;

    n = split_csv(value, parts, 32);
    if (n < 1) {
        return parse_fail(ps, line_no, "output type missing");
    }
    {
        char* type = trim(parts[0]);

        if (eqcase(type, "console")) {
            o->pub.type = HPULOGC_OUT_CONSOLE;
        } else if (eqcase(type, "file")) {
            o->pub.type = HPULOGC_OUT_FILE;
        } else if (eqcase(type, "socket")) {
            return parse_fail(ps, line_no,
                              "socket outputs are not supported (fail-fast)");
        } else {
            return parse_fail(ps, line_no, "unknown output type");
        }
    }

    for (i = 1; i < n; i++) {
        char* kv = parts[i];
        char* eq = strchr(kv, '=');
        char* k;
        char* v;

        if (eq == NULL) {
            return parse_fail(ps, line_no, "output parameter missing '='");
        }
        *eq = '\0';
        k = trim(kv);
        v = trim(eq + 1);
        strip_quotes(v);

        if (strcmp(k, "stream") == 0) {
            if (eqcase(v, "stdout")) {
                o->pub.stream = 0;
            } else if (eqcase(v, "stderr")) {
                o->pub.stream = 1;
            } else {
                return parse_fail(ps, line_no, "invalid stream");
            }
        } else if (strcmp(k, "color") == 0) {
            if (parse_bool(v, &o->pub.color) != 0) {
                return parse_fail(ps, line_no, "invalid boolean");
            }
        } else if (strcmp(k, "path") == 0) {
            if (strlen(v) >= HPULOGC_MAX_PATH_LEN) {
                return parse_fail(ps, line_no, "path too long (512 max)");
            }
            snprintf(o->path_buf, sizeof(o->path_buf), "%s", v);
            o->pub.path = o->path_buf;
        } else if (strcmp(k, "rotate") == 0) {
            if (eqcase(v, "none")) {
                o->pub.rotate = HPULOGC_ROTATE_NONE;
            } else if (eqcase(v, "size")) {
                o->pub.rotate = HPULOGC_ROTATE_SIZE;
            } else if (eqcase(v, "time")) {
                o->pub.rotate = HPULOGC_ROTATE_TIME;
            } else if (eqcase(v, "both")) {
                o->pub.rotate = HPULOGC_ROTATE_BOTH;
            } else {
                return parse_fail(ps, line_no, "invalid rotate policy");
            }
        } else if (strcmp(k, "max size") == 0) {
            unsigned long long sz;

            if (parse_size(v, &sz) != 0) {
                return parse_fail(ps, line_no, "invalid size");
            }
            o->pub.max_size = (size_t)sz;
        } else if (strcmp(k, "time unit") == 0) {
            if (eqcase(v, "hour")) {
                o->pub.time_unit = HPULOGC_TU_HOUR;
            } else if (eqcase(v, "day")) {
                o->pub.time_unit = HPULOGC_TU_DAY;
            } else if (eqcase(v, "week")) {
                o->pub.time_unit = HPULOGC_TU_WEEK;
            } else if (eqcase(v, "month")) {
                o->pub.time_unit = HPULOGC_TU_MONTH;
            } else {
                return parse_fail(ps, line_no, "invalid time unit");
            }
        } else if (strcmp(k, "max files") == 0) {
            o->pub.max_files =
                (int)clamp_long(ps, line_no, strtol(v, NULL, 10), 0, 100000,
                                "max files");
        } else if (strcmp(k, "fsync") == 0) {
            if (parse_bool(v, &o->pub.fsync) != 0) {
                return parse_fail(ps, line_no, "invalid boolean");
            }
        } else if (strcmp(k, "symlink latest") == 0) {
            if (parse_bool(v, &o->pub.symlink_latest) != 0) {
                return parse_fail(ps, line_no, "invalid boolean");
            }
        } else if (strcmp(k, "rotate naming") == 0) {
            if (strlen(v) >= HPULOGC_MAX_FMT_LEN) {
                return parse_fail(ps, line_no, "rotate naming too long");
            }
            snprintf(o->naming_buf, sizeof(o->naming_buf), "%s", v);
            o->pub.rotate_naming = o->naming_buf;
        } else if (strcmp(k, "file perms") == 0) {
            o->pub.file_mode =
                (unsigned)strtol(v, NULL, 8) & 07777u;
        } else if (strcmp(k, "dir perms") == 0) {
            o->pub.dir_mode =
                (unsigned)strtol(v, NULL, 8) & 07777u;
        } else {
            if (ps->strict) {
                return parse_fail(ps, line_no,
                                  "unknown output parameter");
            }
            fprintf(stderr,
                    "hpulogc: %s:%d: unknown output parameter '%s' ignored\n",
                    ps->path ? ps->path : "?", line_no, k);
        }
    }

    if (o->pub.type == HPULOGC_OUT_FILE && o->pub.path == NULL) {
        return parse_fail(ps, line_no, "file output requires 'path'");
    }
    /* default rotate naming (spec 4.6) */
    if (o->pub.rotate_naming == NULL) {
        snprintf(o->naming_buf, sizeof(o->naming_buf), "%s",
                 HPU_ROTATE_DEFAULT_NAMING);
        o->pub.rotate_naming = o->naming_buf;
    }
    return 0;
}

/**
 * @brief Parse one [buffer] key.
 */
static int parse_buffer(conf_parser_t* ps, const char* key, char* value,
                        int line_no)
{
    hpu_conf_t* c = ps->c;

    if (strcmp(key, "buffer size") == 0) {
        unsigned long long sz;

        if (parse_size(value, &sz) != 0) {
            return parse_fail(ps, line_no, "invalid size");
        }
        if (sz < 4096) {
            fprintf(stderr,
                    "hpulogc: %s:%d: buffer size below 4KB, clamped\n",
                    ps->path ? ps->path : "?", line_no);
            sz = 4096;
        }
        if (sz > 1073741824ULL) {
            fprintf(stderr,
                    "hpulogc: %s:%d: buffer size above 1GB, clamped\n",
                    ps->path ? ps->path : "?", line_no);
            sz = 1073741824ULL;
        }
        c->buffer_size = (size_t)sz;
    } else if (strcmp(key, "overflow policy") == 0) {
        if (eqcase(value, "discard")) {
            c->overflow_policy = HPULOGC_OVERFLOW_DISCARD;
        } else if (eqcase(value, "overwrite")) {
            c->overflow_policy = HPULOGC_OVERFLOW_OVERWRITE;
        } else if (eqcase(value, "wait")) {
            c->overflow_policy = HPULOGC_OVERFLOW_WAIT;
        } else {
            return parse_fail(ps, line_no, "invalid overflow policy");
        }
    } else {
        goto unknown;
    }
    return 0;

unknown:
    if (ps->strict) {
        return parse_fail(ps, line_no, "unknown key in [buffer]");
    }
    fprintf(stderr, "hpulogc: %s:%d: unknown key '%s' ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    return 0;
}

/**
 * @brief Parse one [async] key (section skipped when trimmed).
 */
static int parse_async(conf_parser_t* ps, const char* key, char* value,
                       int line_no)
{
    hpu_conf_t* c = ps->c;

    if (strcmp(key, "batch size") == 0) {
        c->batch_size = (uint32_t)clamp_long(ps, line_no,
                                             strtol(value, NULL, 10), 1,
                                             65535, "batch size");
    } else if (strcmp(key, "flush interval") == 0) {
        c->flush_interval_ms = (uint32_t)clamp_long(
            ps, line_no, strtol(value, NULL, 10), 1, 60000,
            "flush interval");
    } else if (strcmp(key, "shutdown timeout") == 0) {
        c->shutdown_timeout_ms = (uint32_t)clamp_long(
            ps, line_no, strtol(value, NULL, 10), 0, 3600000,
            "shutdown timeout");
    } else {
        goto unknown;
    }
    return 0;

unknown:
    if (ps->strict) {
        return parse_fail(ps, line_no, "unknown key in [async]");
    }
    fprintf(stderr, "hpulogc: %s:%d: unknown key '%s' ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    return 0;
}

/**
 * @brief Parse one [throttle] key (section skipped when trimmed).
 */
#if HPULOGC_ENABLE_THROTTLE
static int parse_throttle(conf_parser_t* ps, const char* key, char* value,
                          int line_no)
{
    hpu_throttle_cfg_t* t = &ps->c->throttle;

    if (strcmp(key, "global rate limit") == 0) {
        t->global_rate = clamp_long(ps, line_no, strtol(value, NULL, 10), 0,
                                    1000000000L, "global rate limit");
    } else if (strcmp(key, "per category rate limit") == 0) {
        t->per_category_rate =
            clamp_long(ps, line_no, strtol(value, NULL, 10), 0,
                       1000000000L, "per category rate limit");
    } else if (strcmp(key, "sampling rate") == 0) {
        double d = strtod(value, NULL);

        if (d < 0.0 || d > 1.0) {
            double clamped = d < 0.0 ? 0.0 : 1.0;

            fprintf(stderr,
                    "hpulogc: %s:%d: sampling rate out of range, clamped\n",
                    ps->path ? ps->path : "?", line_no);
            d = clamped;
        }
        t->sampling_rate = d;
        if (d <= 0.0) {
            t->sampling_n = 0; /* everything sampled away */
        } else if (d >= 1.0) {
            t->sampling_n = 1; /* everything passes */
        } else {
            t->sampling_n = (int)(1.0 / d + 0.5);
            if (t->sampling_n < 1) {
                t->sampling_n = 1;
            }
        }
    } else if (strcmp(key, "burst size") == 0) {
        t->burst = clamp_long(ps, line_no, strtol(value, NULL, 10), 1,
                              1000000000L, "burst size");
    } else {
        goto unknown;
    }
    return 0;

unknown:
    if (ps->strict) {
        return parse_fail(ps, line_no, "unknown key in [throttle]");
    }
    fprintf(stderr, "hpulogc: %s:%d: unknown key '%s' ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    return 0;
}
#endif /* HPULOGC_ENABLE_THROTTLE */

/**
 * @brief Parse a rule level part: "*", "LEVEL" or "A~B".
 * @return 0 ok / -1 invalid.
 */
static int parse_rule_levels(const char* part, int* min_out, int* max_out)
{
    const char* tilde = strchr(part, '~');

    if (strcmp(part, "*") == 0) {
        *min_out = HPULOGC_LEVEL_TRACE;
        *max_out = HPULOGC_LEVEL_FATAL;
        return 0;
    }
    if (tilde == NULL) {
        if (parse_level(part, min_out) != 0) {
            return -1;
        }
        *max_out = *min_out;
        return 0;
    }
    {
        char lo[HPULOGC_MAX_NAME_LEN];
        size_t llen = (size_t)(tilde - part);

        if (llen >= sizeof(lo)) {
            return -1;
        }
        memcpy(lo, part, llen);
        lo[llen] = '\0';
        if (parse_level(lo, min_out) != 0 ||
            parse_level(tilde + 1, max_out) != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Parse one [rules] line.
 */
static int parse_rules(conf_parser_t* ps, const char* key, char* value,
                       int line_no)
{
    hpu_conf_t* c = ps->c;
    hpu_conf_rule_t* r;
    const char* last_dot;
    char sel[HPULOGC_MAX_NAME_LEN];
    char level_part[HPULOGC_MAX_NAME_LEN];
    size_t sel_len;
    char* parts[HPULOGC_MAX_OUTPUTS + 1];
    size_t n;
    size_t i;

    if (c->rule_count >= HPULOGC_MAX_RULES) {
        return parse_fail(ps, line_no, "too many rules (64 max)");
    }

    last_dot = strrchr(key, '.');
    if (last_dot == NULL) {
        return parse_fail(ps, line_no, "rule key must be category.level");
    }
    sel_len = (size_t)(last_dot - key);
    if (sel_len == 0 || sel_len >= sizeof(sel)) {
        return parse_fail(ps, line_no, "invalid rule selector");
    }
    if (strchr(last_dot + 1, '.') != NULL) {
        return parse_fail(ps, line_no, "level part must not contain '.'");
    }
    memcpy(sel, key, sel_len);
    sel[sel_len] = '\0';
    snprintf(level_part, sizeof(level_part), "%s", last_dot + 1);

    {
        hpu_conf_rule_t* grown =
            realloc(c->rules, (c->rule_count + 1) * sizeof(hpu_conf_rule_t));

        if (grown == NULL) {
            ps->err = HPULOGC_ERR_NO_MEM;
            return ps->err;
        }
        c->rules = grown;
        r = &c->rules[c->rule_count];
        memset(r, 0, sizeof(*r));
        c->rule_count++;
    }

    /* "*" selector normalization: category NULL/"" == "*" (spec 4.9) */
    if (strcmp(sel, "*") == 0) {
        snprintf(r->category, sizeof(r->category), "*");
    } else {
        snprintf(r->category, sizeof(r->category), "%s", sel);
    }

    if (parse_rule_levels(level_part, &r->min_level, &r->max_level) != 0) {
        return parse_fail(ps, line_no, "invalid rule level");
    }
    if (r->min_level > r->max_level) {
        return parse_fail(ps, line_no, "rule min level > max level");
    }

    n = split_csv(value, parts, HPULOGC_MAX_OUTPUTS + 1);
    if (n < 1) {
        return parse_fail(ps, line_no, "rule needs a format name");
    }
    {
        char* fmt = trim(parts[0]);

        strip_quotes(fmt);
        if (strlen(fmt) >= HPULOGC_MAX_NAME_LEN) {
            return parse_fail(ps, line_no, "format name too long");
        }
        snprintf(r->format_name, sizeof(r->format_name), "%s", fmt);
    }
    for (i = 1; i < n; i++) {
        char* name = trim(parts[i]);

        strip_quotes(name);
        if (name[0] == '\0') {
            continue;
        }
        if (r->output_count >= HPULOGC_MAX_OUTPUTS) {
            return parse_fail(ps, line_no, "too many outputs in rule");
        }
        r->output_idx[r->output_count++] = hpu_conf_find_output(c, name);
        /* unresolved names (-1) are reported at finalize */
    }
    return 0;
}

/**
 * @brief Parse one [advanced] key.
 */
static int parse_advanced(conf_parser_t* ps, const char* key, char* value,
                          int line_no)
{
    hpu_conf_t* c = ps->c;

    if (strcmp(key, "escape injection") == 0) {
        if (parse_bool(value, &c->escape_injection) != 0) {
            return parse_fail(ps, line_no, "invalid boolean");
        }
    } else if (strcmp(key, "max log length") == 0) {
        long v = clamp_long(ps, line_no, strtol(value, NULL, 10), 256,
                            65536, "max log length");

        c->max_log_length = (size_t)v;
    } else if (strcmp(key, "truncation marker") == 0) {
        if (strlen(value) >= HPULOGC_MAX_FMT_LEN) {
            return parse_fail(ps, line_no, "truncation marker too long");
        }
        snprintf(c->truncation_marker, sizeof(c->truncation_marker), "%s",
                 value);
    } else if (strcmp(key, "fork behavior") == 0) {
        if (eqcase(value, "reinit")) {
            c->fork_behavior = HPU_FORK_REINIT;
        } else if (eqcase(value, "disable")) {
            c->fork_behavior = HPU_FORK_DISABLE;
        } else if (eqcase(value, "inherit")) {
            c->fork_behavior = HPU_FORK_INHERIT;
        } else {
            return parse_fail(ps, line_no, "invalid fork behavior");
        }
    } else if (strcmp(key, "signal safe") == 0) {
        if (parse_bool(value, &c->signal_safe) != 0) {
            return parse_fail(ps, line_no, "invalid boolean");
        }
    } else if (strcmp(key, "crash safety") == 0) {
        if (eqcase(value, "none")) {
            c->crash_safety = HPULOGC_CRASH_NONE;
        } else if (eqcase(value, "periodic")) {
            c->crash_safety = HPULOGC_CRASH_PERIODIC;
        } else if (eqcase(value, "entry")) {
            c->crash_safety = HPULOGC_CRASH_ENTRY;
        } else if (eqcase(value, "shutdown")) {
            c->crash_safety = HPULOGC_CRASH_SHUTDOWN;
        } else {
            return parse_fail(ps, line_no, "invalid crash safety");
        }
    } else if (strcmp(key, "stats interval") == 0) {
        c->stats_interval = (int)clamp_long(ps, line_no,
                                            strtol(value, NULL, 10), 0,
                                            86400, "stats interval");
    } else if (strcmp(key, "stats output") == 0) {
        if (eqcase(value, "stderr")) {
            c->stats_output_file = 0;
        } else if (eqcase(value, "file")) {
            c->stats_output_file = 1;
        } else {
            return parse_fail(ps, line_no, "invalid stats output");
        }
    } else if (strcmp(key, "stats file") == 0) {
        if (strlen(value) >= HPULOGC_MAX_PATH_LEN) {
            return parse_fail(ps, line_no, "stats file path too long");
        }
        snprintf(c->stats_file, sizeof(c->stats_file), "%s", value);
    } else {
        goto unknown;
    }
    return 0;

unknown:
    if (ps->strict) {
        return parse_fail(ps, line_no, "unknown key in [advanced]");
    }
    fprintf(stderr, "hpulogc: %s:%d: unknown key '%s' ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    return 0;
}

/**
 * @brief [build] section: compare against the actual build, warn+ignore.
 */
static int parse_build(conf_parser_t* ps, const char* key, char* value,
                       int line_no)
{
    int bval;
    int actual;
    const char* sval;

    if (strcmp(key, "build version") == 0) {
        sval = HPULOGC_BUILD_VERSION_NAME;
        if (!eqcase(value, sval)) {
            goto mismatch;
        }
        return 0;
    }
    if (strcmp(key, "concurrency") == 0) {
#if defined(HPULOGC_CONCURRENCY_SPSC)
        sval = "spsc";
#else
        sval = "mpsc";
#endif
        if (!eqcase(value, sval)) {
            goto mismatch;
        }
        return 0;
    }
    if (strcmp(key, "lockfree") == 0) {
#if HPULOGC_LOCKFREE
        actual = 1;
#else
        actual = 0;
#endif
        if (parse_bool(value, &bval) != 0) {
            return parse_fail(ps, line_no, "invalid boolean");
        }
        if (bval != actual) {
            goto mismatch;
        }
        return 0;
    }
    if (strcmp(key, "has async") == 0) {
        actual = HPULOGC_ENABLE_ASYNC;
    } else if (strcmp(key, "has color") == 0) {
        actual = HPULOGC_ENABLE_COLOR;
    } else if (strcmp(key, "has rotate") == 0) {
        actual = HPULOGC_ENABLE_ROTATE;
    } else if (strcmp(key, "has hot reload") == 0) {
        actual = HPULOGC_ENABLE_HOT_RELOAD;
    } else if (strcmp(key, "has category") == 0) {
        actual = HPULOGC_ENABLE_CATEGORY;
    } else if (strcmp(key, "has throttle") == 0) {
        actual = HPULOGC_ENABLE_THROTTLE;
    } else if (strcmp(key, "has ini") == 0) {
        actual = HPULOGC_ENABLE_INI;
    } else {
        /* build keys are informational: never fail on unknown */
        return 0;
    }
    if (parse_bool(value, &bval) != 0) {
        return parse_fail(ps, line_no, "invalid boolean");
    }
    if (bval != (actual != 0)) {
        goto mismatch;
    }
    return 0;

mismatch:
    fprintf(stderr,
            "hpulogc: %s:%d: [build] '%s' does not match the actual build; "
            "ignored\n",
            ps->path ? ps->path : "?", line_no, key);
    return 0;
}

/**
 * @brief Dispatch one key=value line to its section parser.
 */
static int parse_key_value(conf_parser_t* ps, char* key, char* value,
                           int line_no)
{
    switch (ps->cur_section) {
    case SEC_BUILD:
        return parse_build(ps, key, value, line_no);
    case SEC_GLOBAL:
        return parse_global(ps, key, value, line_no);
    case SEC_FORMATS:
        return parse_formats(ps, key, value, line_no);
    case SEC_OUTPUTS:
        return parse_outputs(ps, key, value, line_no);
    case SEC_BUFFER:
        return parse_buffer(ps, key, value, line_no);
    case SEC_ASYNC:
#if HPULOGC_ENABLE_ASYNC
        return parse_async(ps, key, value, line_no);
#else
        return 0; /* trimmed feature section: silently skipped */
#endif
    case SEC_THROTTLE:
#if HPULOGC_ENABLE_THROTTLE
        return parse_throttle(ps, key, value, line_no);
#else
        return 0;
#endif
    case SEC_RULES:
        return parse_rules(ps, key, value, line_no);
    case SEC_ADVANCED:
        return parse_advanced(ps, key, value, line_no);
    default:
        return parse_fail(ps, line_no, "key outside of any section");
    }
}

/**
 * @brief Line callback: section headers and key/value dispatch.
 */
static int conf_line_cb(void* ud, const char* line, int line_no)
{
    conf_parser_t* ps = ud;

    if (ps->err != 0) {
        return ps->err;
    }

    if (line[0] == '[') {
        const char* close = strchr(line, ']');
        char name[HPULOGC_MAX_NAME_LEN];
        size_t len;
        int rank = -1;
        int i;

        if (close == NULL) {
            return parse_fail(ps, line_no, "malformed section header");
        }
        len = (size_t)(close - line - 1);
        if (len == 0 || len >= sizeof(name)) {
            return parse_fail(ps, line_no, "malformed section header");
        }
        memcpy(name, line + 1, len);
        name[len] = '\0';
        if (strchr(name, ']') != NULL || close[1] != '\0') {
            return parse_fail(ps, line_no, "malformed section header");
        }
        for (i = 0; i < SEC_COUNT; i++) {
            if (eqcase(name, g_section_names[i])) {
                rank = i;
            }
        }
        if (rank < 0) {
            return parse_fail(ps, line_no, "unknown section");
        }
        if (ps->seen[rank]) {
            return parse_fail(ps, line_no, "section appears out of order");
        }
        if (ps->cur_section >= rank && ps->cur_section != SEC_NONE) {
            return parse_fail(ps, line_no, "section appears out of order");
        }
        ps->cur_section = rank;
        ps->seen[rank] = 1;
        return 0;
    }

    {
        char buf[HPU_INI_MAX_LINE * 2];
        char* eq;
        char* key;
        char* value;

        snprintf(buf, sizeof(buf), "%s", line);
        eq = strchr(buf, '=');
        if (eq == NULL) {
            return parse_fail(ps, line_no, "missing '='");
        }
        *eq = '\0';
        key = trim(buf);
        value = trim(eq + 1);
        strip_quotes(value);
        if (key[0] == '\0') {
            return parse_fail(ps, line_no, "empty key");
        }
        if (value[0] == '\0' && strcmp(key, "default outputs") != 0) {
            return parse_fail(ps, line_no, "missing value");
        }
        return parse_key_value(ps, key, value, line_no);
    }
}

/**
 * @brief Pre-scan pass: capture the file's own strict init value.
 */
static int strict_scan_cb(void* ud, const char* line, int line_no)
{
    int* strict_out = ud;
    char buf[HPU_INI_MAX_LINE * 2];
    char* eq;
    char* key;
    char* value;

    (void)line_no;
    if (line[0] == '[') {
        return 0;
    }
    snprintf(buf, sizeof(buf), "%s", line);
    eq = strchr(buf, '=');
    if (eq == NULL) {
        return 0;
    }
    *eq = '\0';
    key = trim(buf);
    value = trim(eq + 1);
    strip_quotes(value);
    if (strcmp(key, "strict init") == 0 && parse_bool(value, strict_out) == 0) {
        return 1; /* stop the scan */
    }
    return 0;
}

/**
 * @brief Load and parse a configuration file into a fresh snapshot.
 *
 * Unknown-key strictness follows the file's own `strict init` value
 * (two-pass: a pre-scan captures it, then the real parse runs with it).
 * An explicit @p strict overrides the pre-scan.
 *
 * @param c       Snapshot with defaults applied.
 * @param path    Configuration file path.
 * @param strict  Effective unknown-key handling (-1: taken from the file).
 * @return        0 or HPULOGC_ERR_CONFIG.
 */
int hpu_conf_load_file(hpu_conf_t* c, const char* path, int strict)
{
    if (strict < 0) {
        int found = 1;
        int err_line = 0;

        {
            int rc = hpu_ini_parse_file(path, strict_scan_cb, &found,
                                        &err_line);

            /* rc 1 = scan stopped early (strict init found) */
            if (rc == 0 || rc == 1) {
                strict = found;
            }
        }
    }
    conf_parser_t ps;
    int err_line = 0;
    int rc;

    ps.c = c;
    ps.path = path;
    ps.strict = strict;
    ps.cur_section = SEC_NONE;
    memset(ps.seen, 0, sizeof(ps.seen));
    ps.err = 0;

    rc = hpu_ini_parse_file(path, conf_line_cb, &ps, &err_line);

    /* The outputs array was reallocated while parsing: re-derive the
     * public string pointers into the (final) per-entry storage. */
    {
        size_t i;

        for (i = 0; i < c->output_count; i++) {
            if (c->outputs[i].path_buf[0] != '\0') {
                c->outputs[i].pub.path = c->outputs[i].path_buf;
            } else {
                c->outputs[i].pub.path = NULL;
            }
            c->outputs[i].pub.rotate_naming = c->outputs[i].naming_buf;
        }
    }
    if (rc != 0) {
        if (rc == HPULOGC_ERR_CONFIG && err_line > 0) {
            fprintf(stderr, "hpulogc: %s:%d: configuration parse error\n",
                    path, err_line);
        } else if (rc == HPULOGC_ERR_CONFIG) {
            fprintf(stderr, "hpulogc: %s: cannot read configuration file\n",
                    path);
        }
        return HPULOGC_ERR_CONFIG;
    }
    if (ps.err != 0) {
        return ps.err;
    }
    snprintf(c->source_path, sizeof(c->source_path), "%s", path);
    return 0;
}
