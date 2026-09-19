/**
 * @file test_format.c
 * @brief Formatter tests: precompiled placeholders, time extensions,
 *        JSON escaping, injection escaping, truncation.
 */

#include "test_util.h"
#include "format/format.h"
#include "conf/conf_model.h"

#include <string.h>

/**
 * @brief Build a render environment with deterministic time settings.
 */
static void make_env(hpu_fmt_env_t* env)
{
    memset(env, 0, sizeof(*env));
    env->time_format = "%Y-%m-%d %H:%M:%S.%f";
    env->use_utc = 1;
    env->timestamp_source = HPULOGC_TS_REALTIME;
    env->newline_style = HPULOGC_NEWLINE_LF;
    env->pid_fmt = HPU_ID_FMT_DECIMAL;
    env->tid_fmt = HPU_ID_FMT_DECIMAL;
    env->source_loc_enabled = 1;
    env->mono_base_us = 1000000;
    env->pid = 4242;
}

/**
 * @brief Build a record with fixed timestamps.
 */
static void make_rec(hpu_log_record_t* rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->level = HPULOGC_LEVEL_WARN;
    rec->category = "app.db";
    rec->category_len = 6;
    rec->file = "db.c";
    rec->file_len = 4;
    rec->func = "query";
    rec->func_len = 5;
    rec->line = 77;
    rec->msg = "hello world";
    rec->msg_len = 11;
    rec->realtime_ns = (int64_t)1700000000ULL * 1000000000LL +
                       123456000; /* 2023-11-14 22:13:20.123456 UTC */
    rec->mono_us = 1500000;
    rec->tid = 99;
}

/**
 * @brief Render with a template and return the line (static buffer).
 */
static const char* render(const char* template_, int escape, size_t max_line,
                          const char* marker)
{
    static char out[8192];
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    size_t len = 0;

    make_env(&env);
    make_rec(&rec);
    hpu_fmt_cache_init(&cache);
    if (hpu_format_compile(&fmt, "t", template_) != 0) {
        return NULL;
    }
    if (hpu_format_render(&fmt, &rec, &env, &cache, escape, max_line,
                          marker, out, sizeof(out), &len) != 0) {
        hpu_format_free(&fmt);
        return NULL;
    }
    hpu_format_free(&fmt);
    return out;
}

TEST(format_builtin_minimal)
{
    CHECK_STREQ(render("%level: %msg%n", 1, 4096, "..."), "WARN: hello world\n");
}

TEST(format_placeholders)
{
    CHECK_STREQ(render("%level|%category|%file:%line|%func", 1, 4096, "..."),
                "WARN|app.db|db.c:77|query");
}

TEST(format_time_extension_micros)
{
    /* UTC time of 1700000000 is 2023-11-14 22:13:20; fraction .123456 */
    const char* line = render("%time", 1, 4096, "...");

    CHECK(line != NULL);
    CHECK_STREQ(line, "2023-11-14 22:13:20.123456");
}

TEST(format_time_extension_millis)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    env.time_format = "%H:%M:%S.%F3"; /* millis live in the time format */
    make_rec(&rec);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "%time"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    CHECK_STREQ(out, "22:13:20.123");
}

TEST(format_monotonic_time)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    env.timestamp_source = HPULOGC_TS_MONOTONIC;
    make_rec(&rec);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "%time"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    CHECK_STREQ(out, "0.500000"); /* 1500000 - 1000000 base */
}

TEST(format_json_escaping_and_decimal_ids)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[512];
    size_t len = 0;

    make_env(&env);
    env.pid_fmt = HPU_ID_FMT_HEX;
    env.tid_fmt = HPU_ID_FMT_NONE; /* json must force decimal output */
    make_rec(&rec);
    rec.msg = "he said \"hi\"\nnew";
    rec.msg_len = strlen(rec.msg);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "json", "{\"m\":\"%msg\",\"p\":%pid,"
                                             "\"t\":%tid,\"c\":\"%category\"}%n"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 300, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    /* JSON escaping applied, pid/tid forced decimal despite hex/none */
    CHECK_STREQ(out, "{\"m\":\"he said \\\"hi\\\"\\nnew\",\"p\":4242,"
                     "\"t\":99,\"c\":\"app.db\"}\n");
}

TEST(format_injection_escaping)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    make_rec(&rec);
    rec.msg = "a\nb\rc\x1b[31m";
    rec.msg_len = strlen(rec.msg);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "[%msg]"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    /* newlines -> literal "\n", ESC -> literal "\x1b" (spec 4.9 step 7) */
    CHECK_STREQ(out, "[a\\nb\\nc\\x1b[31m]");
}

TEST(format_no_escaping_when_disabled)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    make_rec(&rec);
    rec.msg = "a\nb";
    rec.msg_len = 3;
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "[%msg]"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 0, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    CHECK_STREQ(out, "[a\nb]");
}

TEST(format_truncation_with_marker)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    char big[300];
    size_t len = 0;
    size_t i;

    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    make_env(&env);
    make_rec(&rec);
    rec.msg = big;
    rec.msg_len = strlen(big);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "%msg%n"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 64,
                               "...[TRUNCATED]", out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);

    /* total incl. newline must not exceed max_line and must end with the
     * marker */
    CHECK(len <= 64);
    CHECK_STREQ(out + len - strlen("...[TRUNCATED]\n"), "...[TRUNCATED]\n");
    for (i = 0; i < len - strlen("...[TRUNCATED]\n"); i++) {
        CHECK_EQ(out[i], 'A');
    }
}

TEST(format_source_loc_disabled)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    env.source_loc_enabled = 0;
    make_rec(&rec);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "%file|%line|%func"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    CHECK_STREQ(out, "||");
}

TEST(format_unknown_placeholder_rejected)
{
    hpu_format_t fmt;

    CHECK_EQ(hpu_format_compile(&fmt, "t", "%bogus"),
             HPULOGC_ERR_CONFIG);
}

TEST(format_crlf_newline)
{
    hpu_format_t fmt;
    hpu_fmt_env_t env;
    hpu_fmt_cache_t cache;
    hpu_log_record_t rec;
    char out[256];
    size_t len = 0;

    make_env(&env);
    env.newline_style = HPULOGC_NEWLINE_CRLF;
    make_rec(&rec);
    hpu_fmt_cache_init(&cache);
    CHECK_EQ(hpu_format_compile(&fmt, "t", "x%n"), 0);
    CHECK_EQ(hpu_format_render(&fmt, &rec, &env, &cache, 1, 200, "...",
                               out, sizeof(out), &len), 0);
    hpu_format_free(&fmt);
    CHECK_EQ(len, 3);
    CHECK_EQ(out[0], 'x');
    CHECK_EQ(out[1], '\r');
    CHECK_EQ(out[2], '\n');
}
