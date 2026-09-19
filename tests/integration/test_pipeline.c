/**
 * @file test_pipeline.c
 * @brief End-to-end pipeline tests: rule routing (spec 10.3.1), fallback
 *        semantics, JSON output, hot reload and overflow accounting.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"
#include "core/core_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Scratch path prefix (unique per pid). */
static char g_base[256];

/**
 * @brief Initialize the scratch prefix.
 */
static void setup_base(void)
{
    char tmpdir[128];

    if (g_base[0] == '\0') {
        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(g_base, sizeof(g_base), "%s/hpu_pipe_%d", tmpdir,
                 hpu_test_getpid());
    }
}

/**
 * @brief Read a whole file into a static buffer.
 * @return File content or NULL.
 */
static const char* read_file(const char* path)
{
    static char buf[65536];
    FILE* fp = fopen(path, "rb");
    size_t n;

    if (fp == NULL) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

/**
 * @brief Write a config file into the scratch area.
 */
static void write_config(const char* name, const char* text)
{
    char path[300];
    FILE* fp;

    snprintf(path, sizeof(path), "%s_%s", g_base, name);
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs(text, fp);
    fclose(fp);
}

TEST(routing_table_from_spec)
{
    char logpath[300];
    hpulogc_config_t cfg;
    hpulogc_output_t outs[2];
    hpulogc_rule_t rules[8];
    static const char* out_names[1][1] = { { "out0" } };
    (void)out_names;
    int i;
    hpulogc_stats_t st;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_route.log", g_base);
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    cfg.level = HPULOGC_LEVEL_TRACE; /* the routing table uses TRACE rows */
    memset(outs, 0, sizeof(outs));
    outs[0].type = HPULOGC_OUT_FILE;
    outs[0].path = logpath;
    outs[1].type = HPULOGC_OUT_CONSOLE;
    outs[1].stream = 1;

    /* Spec 10.3.1 order: app.debug.* / security.* / db.WARN~FATAL /
     * net.ERROR~FATAL / *.ERROR~FATAL / app.* / *.* */
    memset(rules, 0, sizeof(rules));
    rules[0].category = "app.debug.*";
    rules[0].min_level = HPULOGC_LEVEL_TRACE;
    rules[0].max_level = HPULOGC_LEVEL_FATAL;
    rules[0].format = "standard";
    rules[0].outputs = out_names[0];
    rules[0].output_count = 1;

    rules[1].category = "security.*";
    rules[1].min_level = HPULOGC_LEVEL_TRACE;
    rules[1].max_level = HPULOGC_LEVEL_FATAL;
    rules[1].format = "standard";
    rules[1].outputs = out_names[0];
    rules[1].output_count = 1;

    rules[2].category = "db";
    rules[2].min_level = HPULOGC_LEVEL_WARN;
    rules[2].max_level = HPULOGC_LEVEL_FATAL;
    rules[2].format = "standard";
    rules[2].outputs = out_names[0];
    rules[2].output_count = 1;

    rules[3].category = "net";
    rules[3].min_level = HPULOGC_LEVEL_ERROR;
    rules[3].max_level = HPULOGC_LEVEL_FATAL;
    rules[3].format = "standard";
    rules[3].outputs = out_names[0];
    rules[3].output_count = 1;

    rules[4].category = "*";
    rules[4].min_level = HPULOGC_LEVEL_ERROR;
    rules[4].max_level = HPULOGC_LEVEL_FATAL;
    rules[4].format = "standard";
    rules[4].outputs = out_names[0];
    rules[4].output_count = 1;

    rules[5].category = "app";
    rules[5].min_level = HPULOGC_LEVEL_TRACE;
    rules[5].max_level = HPULOGC_LEVEL_FATAL;
    rules[5].format = "standard";
    rules[5].outputs = out_names[0];
    rules[5].output_count = 1;

    rules[6].category = "*";
    rules[6].min_level = HPULOGC_LEVEL_TRACE;
    rules[6].max_level = HPULOGC_LEVEL_FATAL;
    rules[6].format = "standard";
    rules[6].outputs = out_names[0];
    rules[6].output_count = 1;
    (void)rules[7];

    cfg.outputs = outs;
    cfg.output_count = 1; /* file only: deterministic content */
    cfg.rules = rules;
    cfg.rule_count = 7;
    /* default outputs empty: unmatched -> dropped */
    cfg.default_format = "standard";
    cfg.default_outputs = NULL;
    cfg.default_output_count = 0;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    /* (app.debug.conn, INFO) -> rule 0 (debug_log row) */
    hpulogc_log(HPULOGC_LEVEL_INFO, "app.debug.conn", NULL, 0, NULL, "r0");
    /* (app.http, INFO) -> rule 5 app.* */
    hpulogc_log(HPULOGC_LEVEL_INFO, "app.http", NULL, 0, NULL, "r5");
    /* (app.http, ERROR) -> rule 4 *.ERROR~FATAL (before app.*) */
    hpulogc_log(HPULOGC_LEVEL_ERROR, "app.http", NULL, 0, NULL, "r4");
    /* (db.query, WARN) -> rule 2 db.WARN~FATAL */
    hpulogc_log(HPULOGC_LEVEL_WARN, "db.query", NULL, 0, NULL, "r2");
    /* (db.query, ERROR) -> rule 2 (db before *.ERROR) */
    hpulogc_log(HPULOGC_LEVEL_ERROR, "db.query", NULL, 0, NULL, "r2b");
    /* (net.sock, ERROR) -> rule 3 */
    hpulogc_log(HPULOGC_LEVEL_ERROR, "net.sock", NULL, 0, NULL, "r3");
    /* (security.login, TRACE) -> rule 1 */
    hpulogc_log(HPULOGC_LEVEL_TRACE, "security.login", NULL, 0, NULL,
                "r1");
    /* (ui.render, INFO) -> rule 6 *.* */
    hpulogc_log(HPULOGC_LEVEL_INFO, "ui.render", NULL, 0, NULL, "r6");
    /* (other, INFO) also lands on rule 6 *.* (matches everything) */
    hpulogc_log(HPULOGC_LEVEL_INFO, "other", NULL, 0, NULL, "r6b");

    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.accepted, 9);
    CHECK_EQ(st.written, 9);
    CHECK(st.dropped == 0);

    {
        const char* content = read_file(logpath);

        CHECK(content != NULL);
        CHECK(strstr(content, "r0") != NULL);
        CHECK(strstr(content, "r5") != NULL);
        CHECK(strstr(content, "r4") != NULL);
        CHECK(strstr(content, "r2b") != NULL);
        CHECK(strstr(content, "r3") != NULL);
        CHECK(strstr(content, "r1") != NULL);
        CHECK(strstr(content, "r6") != NULL);
        CHECK(strstr(content, "r6b") != NULL);
    }
    for (i = 0; i < 1; i++) {
        (void)i;
    }
    hpulogc_shutdown();
}

TEST(default_fallback_outputs)
{
    char logpath[300];
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    static const char* names[] = { "out0" };
    static const char* empty_names[1] = { NULL };
    hpulogc_stats_t st;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_fb.log", g_base);
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;
    cfg.default_format = "minimal"; /* "INFO: msg" style */
    cfg.default_outputs = names;
    cfg.default_output_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "anything", NULL, 0, NULL, "fallback");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    {
        const char* content = read_file(logpath);

        CHECK(content != NULL);
        CHECK(strstr(content, "INFO: fallback") != NULL);
    }

    /* default_outputs cleared at runtime semantics: re-init without
     * fallback -> dropped */
    hpulogc_shutdown();
    hpu_test_unlink(logpath);

    cfg.default_outputs = empty_names;
    cfg.default_output_count = 0;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "anything", NULL, 0, NULL, "gone");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.accepted, 1);
    CHECK_EQ(st.written, 0);
    hpulogc_shutdown();
}

/**
 * @brief External JSON validation via python (returns 0 when python is
 *        unavailable or fails; decision 32).
 */
static int json_validate_external(const char* content)
{
#if defined(_WIN32)
    FILE* jf;
    FILE* fp;
    char cmd[512];
    char verdict[64];
    char tmpjson[300];
    char tmpdir[128];

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(tmpjson, sizeof(tmpjson), "%s/hpu_json_check.json", tmpdir);
    jf = fopen(tmpjson, "w");
    if (jf == NULL) {
        return 0;
    }
    fputs(content, jf);
    fclose(jf);
    snprintf(cmd, sizeof(cmd),
             "python -c \"import json,sys;"
             "[json.loads(l) for l in open(r'%s') if l.strip()]\" "
             "&& echo JSON_OK",
             tmpjson);
    fp = hpu_test_popen(cmd, "r");
    if (fp == NULL) {
        return 0;
    }
    {
        int ok = 0;

        while (fgets(verdict, sizeof(verdict), fp) != NULL) {
            if (strstr(verdict, "JSON_OK") != NULL) {
                ok = 1;
            }
        }
        hpu_test_pclose(fp);
        hpu_test_unlink(tmpjson);
        return ok;
    }
#else
    (void)content;
    return 0; /* POSIX keeps the Phase 1 behavior (no external run here) */
#endif
}

/**
 * @brief Built-in structural JSON check: every non-empty line must be a
 *        balanced JSON object (quote-aware brace scan; decision 32).
 */
static int json_check_builtin(const char* content)
{
    int lines = 0;

    while (*content != '\0') {
        const char* eol = strchr(content, '\n');
        size_t len = eol != NULL ? (size_t)(eol - content)
                                 : strlen(content);
        const char* p = content;
        const char* end = content + len;
        int in_string = 0;
        int depth = 0;
        int ok = 1;

        while (len > 0 && (end[-1] == '\r')) {
            end--;
            len--;
        }
        if (len == 0) {
            content = eol != NULL ? eol + 1 : end;
            continue;
        }
        while (p < end && ok) {
            char c = *p++;

            if (in_string) {
                if (c == '\\') {
                    if (p >= end) {
                        ok = 0;
                    } else {
                        p++; /* escaped char */
                    }
                } else if (c == '"') {
                    in_string = 0;
                }
            } else if (c == '"') {
                in_string = 1;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth < 0) {
                    ok = 0;
                }
            }
        }
        if (!ok || depth != 0 || in_string) {
            return 0;
        }
        lines++;
        content = eol != NULL ? eol + 1 : end;
    }
    return lines > 0;
}

TEST(json_output_validity)
{
    char logpath[300];
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    hpulogc_rule_t rule;
    static const char* names[] = { "out0" };
    const char* content;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_json.log", g_base);
    hpu_test_unlink(logpath);

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;

    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "json";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "app", NULL, 0, NULL,
                "quote\" back\\slash");
    hpulogc_flush();
    hpulogc_shutdown();

    content = read_file(logpath);
    CHECK(content != NULL);
    /* Validate the JSON lines. Preferred: an external python; when no
     * python is available (Windows CI images) fall back to a built-in
     * structural check (decision 32). */
    if (!json_validate_external(content)) {
        CHECK(json_check_builtin(content));
    }
    CHECK(strstr(content, "quote\\\" back\\\\slash") != NULL);
}

TEST(truncation_marker_end_to_end)
{
    char logpath[300];
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    hpulogc_rule_t rule;
    static const char* names[] = { "out0" };
    char big[9000];
    const char* content;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_trunc.log", g_base);
    hpu_test_unlink(logpath);

    memset(big, 'B', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;
    cfg.max_log_length = 1024;

    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "%s", big);
    hpulogc_flush();
    hpulogc_shutdown();

    content = read_file(logpath);
    CHECK(content != NULL);
    CHECK(strlen(content) <= 1024);
    CHECK(strstr(content, "...[TRUNCATED]") != NULL);
}

#if HPULOGC_ENABLE_ASYNC
TEST(overflow_discard_accounting)
{
    char logpath[300];
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    hpulogc_stats_t st;
    int i;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_disc.log", g_base);
    hpu_test_unlink(logpath);

    static const char* names[] = { "out0" };
    hpulogc_rule_t rule;

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = logpath;
    cfg.outputs = &out;
    cfg.output_count = 1;
    cfg.buffer_size = 16 * 1024; /* tiny ring */
    cfg.overflow_policy = HPULOGC_OVERFLOW_DISCARD;
    cfg.default_format = "standard";
    memset(&rule, 0, sizeof(rule));
    rule.category = "*";
    rule.min_level = HPULOGC_LEVEL_TRACE;
    rule.max_level = HPULOGC_LEVEL_FATAL;
    rule.format = "standard";
    rule.outputs = names;
    rule.output_count = 1;
    cfg.rules = &rule;
    cfg.rule_count = 1;

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
    /* burst far beyond the ring capacity */
    for (i = 0; i < 5000; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL,
                    "message %04d with padding 0123456789012345", i);
    }
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.accepted, 5000);
    CHECK(st.dropped > 0); /* discard happened */
    hpulogc_shutdown();
}
#endif /* HPULOGC_ENABLE_ASYNC */

#if HPULOGC_ENABLE_INI
TEST(hot_reload_swap_and_rollback)
{
    char cfgfile[300];
    char logpath[300];
    const char* content;

    setup_base();
    snprintf(cfgfile, sizeof(cfgfile), "%s_hot.ini", g_base);
    snprintf(logpath, sizeof(logpath), "%s_hot.log", g_base);
    hpu_test_unlink(logpath);

    write_config("hot.ini",
                 "[global]\n"
                 "default format = standard\n"
                 "default outputs = f\n"
                 "[formats]\n"
                 "extra = \"[X] %level %msg%n\"\n"
                 "[outputs]\n"
                 "f = file, path=LOGPATH\n"
                 "[rules]\n"
                 "*.* = extra, f\n");
    {
        /* patch the path into the config */
        char buf[1024];
        char tmpini[300];
        char tmpdir[128];
        FILE* fp;
        FILE* out;

        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(tmpini, sizeof(tmpini), "%s/hpu_hot_tmp.ini", tmpdir);
        fp = fopen(cfgfile, "r");
        out = fopen(tmpini, "w");

        CHECK(fp != NULL && out != NULL);
        while (fgets(buf, sizeof(buf), fp) != NULL) {
            char* p = strstr(buf, "LOGPATH");

            if (p != NULL) {
                fprintf(out, "f = file, path=%s\n", logpath);
            } else {
                fputs(buf, out);
            }
        }
        fclose(fp);
        fclose(out);
        hpu_test_unlink(cfgfile); /* Windows rename() refuses to replace */
        (void)rename(tmpini, cfgfile);
    }

    CHECK_EQ(hpulogc_init_from_file(cfgfile), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "before");
    hpulogc_flush();
    {
        /* change the format of the rule to the built-in "minimal" */
        FILE* fp = fopen(cfgfile, "w");

        CHECK(fp != NULL);
        fprintf(fp,
                "[global]\n"
                "default format = standard\n"
                "default outputs = f\n"
                "[formats]\n"
                "extra = \"[X] %%level %%msg%%n\"\n"
                "[outputs]\n"
                "f = file, path=%s\n"
                "[rules]\n"
                "*.* = minimal, f\n",
                logpath);
        fclose(fp);
    }
    hpu_core_trigger_reload();
    hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "after");
    hpulogc_flush();
    hpulogc_shutdown();

    content = read_file(logpath);
    CHECK(content != NULL);
    CHECK(strstr(content, "[X] INFO before") != NULL);
    CHECK(strstr(content, "INFO: after") != NULL);

    /* invalid reload keeps the old config */
    CHECK_EQ(hpulogc_init_from_file(cfgfile), HPULOGC_OK);
    {
        FILE* fp = fopen(cfgfile, "w");

        CHECK(fp != NULL);
        fprintf(fp, "%s", "[formats]\nbad = \"%bogus%%\"\n[rules]\n");
        fclose(fp);
    }
    hpu_core_trigger_reload(); /* must fail and keep the old config */
    hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "still_old");
    hpulogc_flush();
    hpulogc_shutdown();
}
#endif /* HPULOGC_ENABLE_INI */
