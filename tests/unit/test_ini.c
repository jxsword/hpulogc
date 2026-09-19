/**
 * @file test_ini.c
 * @brief INI lexer and config-parser behavior tests (spec 10.1/10.4).
 */

#include "test_util.h"
#include "conf/ini.h"
#include "conf/conf_model.h"

#if !HPULOGC_ENABLE_INI
/* The INI-specific cases are meaningless without the parser. */
#define HPU_TEST_SKIP_INI 1
#else
#define HPU_TEST_SKIP_INI 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Collected lines for lexer tests. */
static char g_lines[64][2048];
static int g_line_no[64];
static int g_line_count;

/**
 * @brief Line collector callback.
 */
static int collect_cb(void* ud, const char* line, int line_no)
{
    (void)ud;
    if (g_line_count < 64) {
        snprintf(g_lines[g_line_count], sizeof(g_lines[0]), "%s", line);
        g_line_no[g_line_count] = line_no;
        g_line_count++;
    }
    return 0;
}

/**
 * @brief Run the buffer parser on a text and return its result code.
 */
static int lex(const char* text)
{
    g_line_count = 0;
    return hpu_ini_parse_buffer(text, collect_cb, NULL, NULL);
}

#if !HPU_TEST_SKIP_INI
TEST(ini_comments_and_quotes)
{
    CHECK_EQ(lex("# full comment\n"), 0);
    CHECK_EQ(g_line_count, 0);

    CHECK_EQ(lex("key = value # trailing comment\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = value");

    CHECK_EQ(lex("key = \"value # not comment\"\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = \"value # not comment\"");

    /* semicolon is NOT a comment character */
    CHECK_EQ(lex("key = value ; kept\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = value ; kept");
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(ini_continuation)
{
    CHECK_EQ(lex("key = one \\\ntwo\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = one two");
    CHECK_EQ(g_line_no[0], 1); /* error line = first physical line */

    /* double backslash is literal, no continuation */
    CHECK_EQ(lex("key = one \\\\\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = one \\");
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(ini_line_too_long)
{
    char big[2048];
    int err_line = 0;
    size_t i;

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    big[1023] = '\n'; /* first physical line exactly 1023+nl is fine */
    big[1023] = 'x';

    /* build: 1100 x's, one line: too long */
    memset(big, 'x', 1100);
    big[1100] = '\n';
    big[1101] = '\0';
    CHECK_EQ(hpu_ini_parse_buffer(big, collect_cb, NULL, &err_line),
             HPULOGC_ERR_CONFIG);
    CHECK_EQ(err_line, 1);

    /* 1000 chars + newline is fine */
    memset(big, 'x', 1000);
    big[1000] = '\n';
    big[1001] = '\0';
    g_line_count = 0;
    CHECK_EQ(hpu_ini_parse_buffer(big, collect_cb, NULL, &err_line), 0);

    for (i = 0; i < (size_t)g_line_count; i++) {
        CHECK_EQ((long long)strlen(g_lines[i]), 1000LL);
    }
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(ini_blank_lines_skipped)
{
    CHECK_EQ(lex("\n\n   \n\t\nkey = v\n\n"), 0);
    CHECK_EQ(g_line_count, 1);
    CHECK_STREQ(g_lines[0], "key = v");
    CHECK_EQ(g_line_no[0], 5);
}
#endif /* INI cases */

/* ------------------------------------------------------------------ */
/* Config parser behavior (spec 10.2-10.4)                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Parse a config text into a defaulted snapshot.
 */
#if !HPU_TEST_SKIP_INI
static int conf_parse(hpu_conf_t* c, const char* text)
{
    char path[256];
    FILE* fp;
    int rc;

    snprintf(path, sizeof(path), "/tmp/hpu_test_conf_%d.ini", (int)getpid());
    fp = fopen(path, "w");
    if (fp == NULL) {
        return -99;
    }
    fputs(text, fp);
    fclose(fp);
    rc = hpu_conf_load_file(c, path, -1);
    if (rc == 0) {
        rc = hpu_conf_finalize(c);
        if (rc != 0) {
            hpu_conf_free(c);
        }
    }
    unlink(path);
    return rc;
}
#endif /* !HPU_TEST_SKIP_INI */

#if !HPU_TEST_SKIP_INI
TEST(conf_empty_file_ok)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "# only comments\n\n"), 0);
    CHECK_EQ(c.level, HPULOGC_LEVEL_INFO); /* defaults kept */
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_section_order_enforced)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* rules before outputs: relative order violation (spec 10.2) */
    CHECK_EQ(conf_parse(&c, "[rules]\n*.* = standard\n"
                           "[outputs]\no = console, stream=stderr\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* backtracking into an earlier section is rejected too */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[buffer]\nbuffer size = 1mb\n"
                           "[global]\nlevel = info\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_unknown_section_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[bogus]\nkey = value\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_unknown_key_strict)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\nnot a real key = 1\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_size_suffixes)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[buffer]\nbuffer size = 2mb\n"), 0);
    CHECK_EQ(c.buffer_size, 2U * 1024U * 1024U);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[buffer]\nbuffer size = 8k\n"), 0);
    CHECK_EQ(c.buffer_size, 8000U);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[buffer]\nbuffer size = 2GB\n"), 0);
    CHECK_EQ(c.buffer_size, 1073741824ULL); /* clamped to the 1GB cap */
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_duplicate_format_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c,
                        "[formats]\nmine = \"%level: %msg%n\"\n"
                        "mine = \"%level|%msg%n\"\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_rules_duplicate_keys_allowed)
{
    hpu_conf_t c;
    char path[256];
    FILE* fp;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    snprintf(path, sizeof(path), "/tmp/hpu_rules_%d.ini", (int)getpid());
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs("[rules]\napp.* = standard\napp.* = minimal\n", fp);
    fclose(fp);
    /* format references resolve against built-ins; no outputs needed */
    CHECK_EQ(hpu_conf_load_file(&c, path, 1), 0);
    CHECK_EQ(c.rule_count, 2);
    unlink(path);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_rule_level_range_validated)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[rules]\napp.FATAL~ERROR = standard\n"),
             HPULOGC_ERR_CONFIG); /* min > max */
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_undefined_reference_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[rules]\napp.* = no_such_format\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\ndefault format = nope\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_socket_output_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[outputs]\ns1 = socket, host=h, port=1\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_clamp_with_warning)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* max log length below range: clamped to 256 (config file semantics) */
    CHECK_EQ(conf_parse(&c, "[advanced]\nmax log length = 10\n"), 0);
    CHECK_EQ(c.max_log_length, 256U);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_naming_template_validated)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c,
                        "[outputs]\nf = file, path=/tmp/hpu_t.log, "
                        "rotate naming=plain.log\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_duplicate_paths_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c,
                        "[outputs]\na = file, path=/tmp/x.log\n"
                        "b = file, path=/tmp/./x.log\n"),
             HPULOGC_ERR_CONFIG); /* normalized collision */
    hpu_conf_free(&c);
}
#endif /* INI cases */

#if !HPU_TEST_SKIP_INI
TEST(conf_all_sections_parse)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c,
        "[build]\n"
        "build version = full\n"
        "concurrency = mpsc\n"
        "lockfree = false\n"
        "has async = true\n"
        "has color = true\n"
        "has rotate = true\n"
        "has hot reload = true\n"
        "has category = true\n"
        "has throttle = false\n"
        "has ini = true\n"
        "[global]\n"
        "strict init = false\n"
        "level = trace\n"
        "default format = standard\n"
        "default outputs = f\n"
        "timezone = utc\n"
        "timestamp source = monotonic\n"
        "time format = %H:%M:%S.%F3\n"
        "encoding = utf-8\n"
        "capture source loc = false\n"
        "newline = crlf\n"
        "pid format = hex\n"
        "tid format = none\n"
        "hot reload interval = 2\n"
        "signal reload = false\n"
        "[formats]\n"
        "mine = \"%level %msg%n\"\n"
        "[outputs]\n"
        "f = file, path=/tmp/hpu_all.log, rotate=both, max size=1mb, "
        "time unit=week, max files=5, fsync=true, symlink latest=true, "
        "rotate naming=\"{base}.{index}.log\", file perms=0600, "
        "dir perms=0700\n"
        "c = console, stream=stderr, color=false\n"
        "[buffer]\n"
        "buffer size = 512kb\n"
        "overflow policy = discard\n"
        "[async]\n"
        "batch size = 128\n"
        "flush interval = 50\n"
        "shutdown timeout = 1000\n"
        "[throttle]\n"
        "global rate limit = 1000\n"
        "per category rate limit = 500\n"
        "sampling rate = 0.25\n"
        "burst size = 50\n"
        "[rules]\n"
        "app.debug.* = mine, f\n"
        "net.ERROR~FATAL = detailed, f, c\n"
        "*.* = standard\n"
        "[advanced]\n"
        "escape injection = false\n"
        "max log length = 8192\n"
        "truncation marker = ...cut\n"
        "fork behavior = disable\n"
        "signal safe = true\n"
        "crash safety = periodic\n"
        "stats interval = 30\n"
        "stats output = stderr\n"), 0);

    CHECK_EQ(c.use_utc, 1);
    CHECK_EQ(c.timestamp_source, HPULOGC_TS_MONOTONIC);
    CHECK_EQ(c.newline_style, HPULOGC_NEWLINE_CRLF);
    CHECK_EQ(c.pid_fmt, HPU_ID_FMT_HEX);
    CHECK_EQ(c.tid_fmt, HPU_ID_FMT_NONE);
    CHECK_EQ(c.capture_source_loc, 0);
#if HPULOGC_ENABLE_ASYNC
    CHECK_EQ(c.batch_size, 128);
    CHECK_EQ(c.flush_interval_ms, 50);
    CHECK_EQ(c.shutdown_timeout_ms, 1000);
#else
    CHECK_EQ(c.batch_size, 64); /* trimmed section: defaults kept */
#endif
#if HPULOGC_ENABLE_THROTTLE
    CHECK_EQ(c.throttle.global_rate, 1000);
    CHECK_EQ(c.throttle.per_category_rate, 500);
    CHECK_EQ(c.throttle.sampling_n, 4); /* 1/0.25 */
    CHECK_EQ(c.throttle.burst, 50);
#else
    /* trimmed section: keys silently skipped (spec 10.2) */
    CHECK_EQ(c.throttle.global_rate, 0);
#endif
    CHECK_EQ(c.escape_injection, 0);
    CHECK_EQ(c.max_log_length, 8192);
    CHECK_EQ(c.fork_behavior, HPU_FORK_DISABLE);
    CHECK_EQ(c.signal_safe, 1);
    CHECK_EQ(c.crash_safety, HPULOGC_CRASH_PERIODIC);
    CHECK_EQ(c.stats_interval, 30);
    CHECK_EQ(c.output_count, 2);
    CHECK_EQ(c.rule_count, 3);
    CHECK_EQ(c.outputs[0].pub.file_mode, 0600u);
    CHECK_EQ(c.outputs[0].pub.dir_mode, 0700u);
    hpu_conf_free(&c);
}

TEST(conf_build_mismatch_warns_only)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* [build] mismatches are warnings, never startup failures (10.4) */
    CHECK_EQ(conf_parse(&c,
        "[build]\n"
        "build version = min\n"
        "concurrency = spsc\n"
        "lockfree = true\n"
        "has async = false\n"), 0);
    hpu_conf_free(&c);
}

TEST(conf_lenient_unknown_key)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* strict init = false: unknown keys degrade to warnings (10.4) */
    CHECK_EQ(conf_parse(&c,
        "[global]\n"
        "strict init = false\n"
        "bogus key = whatever\n"
        "level = info\n"), 0);
    CHECK_EQ(c.level, HPULOGC_LEVEL_INFO);
    hpu_conf_free(&c);
}

TEST(conf_output_param_errors)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* missing '=' in an output parameter */
    CHECK_EQ(conf_parse(&c, "[outputs]\nf = file, path nope\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* file output without a path */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[outputs]\nf = file\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* unknown type */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[outputs]\nf = syslog, x=1\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* too many outputs */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    {
        char big[4096];
        int i;
        size_t off = 0;

        off += (size_t)snprintf(big + off, sizeof(big) - off, "[outputs]\n");
        for (i = 0; i < HPULOGC_MAX_OUTPUTS + 1; i++) {
            off += (size_t)snprintf(big + off, sizeof(big) - off,
                                    "o%d = console, stream=stderr\n", i);
        }
        CHECK_EQ(conf_parse(&c, big), HPULOGC_ERR_CONFIG);
    }
    hpu_conf_free(&c);
}

TEST(conf_rule_key_errors)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    /* no dot in the rule key */
    CHECK_EQ(conf_parse(&c, "[rules]\nnodot = standard\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* empty category part */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[rules]\n.INFO = standard\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* invalid level part */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[rules]\na.LOUD = standard\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}

TEST(conf_value_errors)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\nlevel = LOUD\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\nnewline = tabs\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[buffer]\nbuffer size = 12qx\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[advanced]\ncrash safety = maybe\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[advanced]\nfork behavior = clone\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* missing '=' */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\nlevel INFO\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);

    /* missing value */
    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(conf_parse(&c, "[global]\nlevel =\n"),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}

TEST(conf_missing_file_rejected)
{
    hpu_conf_t c;

    CHECK_EQ(hpu_conf_defaults(&c), 0);
    CHECK_EQ(hpu_conf_load_file(&c, "/tmp/hpu_definitely_missing_42.ini", 1),
             HPULOGC_ERR_CONFIG);
    hpu_conf_free(&c);
}
#endif /* INI cases */
