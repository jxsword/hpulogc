/**
 * @file test_sinks.c
 * @brief Multi-sink integration (rd_v0.6 §4.7): built-in sinks via INI,
 *        file alias, unknown type/key fail-fast, common keys, per-sink
 *        stats and custom code-config sinks.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Scratch path prefix (unique per pid). */
static char g_base[256];

static void setup_base(void)
{
    char tmpdir[128];

    if (g_base[0] == '\0') {
        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(g_base, sizeof(g_base), "%s/hpu_sinks_%d", tmpdir,
                 hpu_test_getpid());
    }
}

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

/**
 * @brief Read a whole file into a malloc'ed buffer.
 * @return Buffer (caller frees) or NULL.
 */
static char* read_file_dyn(const char* path)
{
    FILE* fp = fopen(path, "rb");
    size_t cap = 65536, len = 0;
    char* buf;

    if (fp == NULL) {
        return NULL;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        fclose(fp);
        return NULL;
    }
    for (;;) {
        size_t got;

        if (len + 4096 > cap) {
            char* grown;

            cap *= 2;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = grown;
        }
        got = fread(buf + len, 1, cap - len - 1, fp);
        if (got == 0) {
            break;
        }
        len += got;
    }
    fclose(fp);
    buf[len] = '\0';
    return buf;
}

#if !HPULOGC_ENABLE_INI
TEST(sinks_ini_disabled_noop)
{
    /* INI=OFF builds: the [outputs] sink types are unreachable; the flat
     * shim and code-config sinks still work (covered elsewhere). */
    CHECK(1);
}
#else

TEST(sinks_null_and_file_alias)
{
    char cfgpath[300];
    char* content;
    hpulogc_sink_stats_t st;

    setup_base();
    snprintf(cfgpath, sizeof(cfgpath), "%s_null.ini", g_base);
    {
        char logpath[300];
        char text[1024];

        snprintf(logpath, sizeof(logpath), "%s_null.log", g_base);
        hpu_test_unlink(logpath);
        snprintf(text, sizeof(text),
                 "[global]\n"
                 "default outputs = null0\n"
                 "[outputs]\n"
                 "null0 = null\n"
                 "[rules]\n"
                 "*.* = standard, null0\n");
        write_config("null.ini", text);
        CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
        HPULOGC_INFO("t", "to the null sink");
        CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
        CHECK_EQ(hpulogc_get_sink_stats("null0", &st), HPULOGC_OK);
        CHECK_EQ(st.written, 1); /* null discards but counts as written */
        CHECK_EQ(hpulogc_get_sink_stats("nope", &st),
                 HPULOGC_ERR_INVALID_ARG);
        hpulogc_shutdown();
        content = read_file_dyn(logpath);
        /* the logpath was never opened (null has no file) */
        CHECK(content == NULL);
        free(content);
    }

    /* `file` alias: rollingfile with rotate defaulting to none */
    {
        char logpath[300];
        char text[1024];

        snprintf(logpath, sizeof(logpath), "%s_alias.log", g_base);
        hpu_test_unlink(logpath);
        snprintf(text, sizeof(text),
                 "[global]\n"
                 "default outputs = f0\n"
                 "[outputs]\n"
                 "f0 = file, path=%s\n"
                 "[rules]\n"
                 "*.* = standard, f0\n",
                 logpath);
        write_config("alias.ini", text);
        snprintf(cfgpath, sizeof(cfgpath), "%s_alias.ini", g_base);
        CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
        HPULOGC_INFO("t", "alias line");
        CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
        CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
        CHECK_EQ(st.written, 1);
        hpulogc_shutdown();
        content = read_file_dyn(logpath);
        CHECK(content != NULL);
        CHECK(strstr(content, "alias line") != NULL);
        free(content);
        hpu_test_unlink(logpath);
    }
}

TEST(sinks_unknown_type_fails_fast)
{
    char cfgpath[300];

    setup_base();
    snprintf(cfgpath, sizeof(cfgpath), "%s_bad.ini", g_base);
    write_config("bad", "[outputs]\n"
                        "x0 = socket, host=1.2.3.4\n");
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);
    hpulogc_shutdown(); /* idempotent after failed init */
}

TEST(sinks_unknown_key_strict_and_lenient)
{
    char cfgpath[300];
    char text[512];

    setup_base();
    snprintf(cfgpath, sizeof(cfgpath), "%s_uk.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "strict init = true\n"
             "default outputs = n0\n"
             "[outputs]\n"
             "n0 = null, bogus=1\n");
    write_config("uk.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);
    hpulogc_shutdown();

    /* lenient: unknown key ignored */
    snprintf(text, sizeof(text),
             "[global]\n"
             "strict init = false\n"
             "default outputs = n0\n"
             "[outputs]\n"
             "n0 = null, bogus=1\n");
    write_config("uk2.ini", text);
    snprintf(cfgpath, sizeof(cfgpath), "%s_uk2.ini", g_base);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    hpulogc_shutdown();
}

TEST(sinks_enabled_common_key)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    setup_base();
    snprintf(cfgpath, sizeof(cfgpath), "%s_en.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = n0, n1\n"
             "[outputs]\n"
             "n0 = null\n"
             "n1 = null, enabled=false\n"
             "[rules]\n"
             "*.* = standard, n0, n1\n");
    write_config("en.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    HPULOGC_INFO("t", "x");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("n0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1);
    CHECK_EQ(hpulogc_get_sink_stats("n1", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 0); /* disabled sink receives nothing */
    hpulogc_shutdown();
}

#if defined(HPULOGC_SINK_SYSLOG)
TEST(sinks_syslog_single_instance)
{
    char cfgpath[300];
    char text[512];

    setup_base();
    snprintf(cfgpath, sizeof(cfgpath), "%s_sl.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = s0\n"
             "[outputs]\n"
             "s0 = syslog, facility=user\n"
             "s1 = syslog, facility=daemon\n");
    write_config("sl.ini", text);
    /* openlog state is process-global: one instance max (§10.4) */
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);
    hpulogc_shutdown();
}
#endif

#if defined(HPULOGC_SINK_SYSLOG)
TEST(sinks_custom_code_config_and_async)
{
    hpulogc_config_t cfg;
    hpulogc_sink_decl_t sinks[1];
    static const char* const keys[] = { "async", "queue size" };
    static const char* const vals[] = { "on", "64kb" };
    static const char* const names[] = { "s0" };

    /* async=on on a SYNC-only built-in (syslog) -> ERR_CONFIG: the type
     * does not declare HPULOGC_CAP_ASYNC (§4.10.1). */
    hpulogc_config_default(&cfg);
    sinks[0].name = "s0";
    sinks[0].type = "syslog";
    sinks[0].keys = keys;
    sinks[0].vals = vals;
    sinks[0].count = 2;
    cfg.sinks = sinks;
    cfg.sink_count = 1;
    cfg.default_outputs = names;
    cfg.default_output_count = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_ERR_CONFIG);
}
#endif /* HPULOGC_SINK_SYSLOG */

#endif /* HPULOGC_ENABLE_INI */
