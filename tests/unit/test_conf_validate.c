/**
 * @file test_conf_validate.c
 * @brief hpulogc_conf_validate() semantics (spec rd_v0.6 v0.6.1 §7.3/§11):
 *        dry-run validity, strict handling, error reporting, exit mapping.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Scratch path buffer. */
static char g_cfgpath[300];

/**
 * @brief Write a config file into the scratch area.
 *
 * @param name  File name suffix.
 * @param text  Full INI text.
 * @return      Path of the written file.
 */
static const char* write_cfg(const char* name, const char* text)
{
    char tmpdir[128];
    FILE* fp;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_cfgpath, sizeof(g_cfgpath), "%s/hpu_cval_%s_%d.ini", tmpdir,
             name, hpu_test_getpid());
    fp = fopen(g_cfgpath, "w");
    if (fp == NULL) {
        /* Infrastructure failure (not an assertion): abort the binary. */
        HPU_FAIL("cannot write %s", g_cfgpath);
        exit(1);
    }
    fputs(text, fp);
    fclose(fp);
    return g_cfgpath;
}

TEST(conf_validate_valid_file)
{
    char err[256];
    const char* p = write_cfg("ok",
                              "[global]\n"
                              "level = INFO\n"
                              "default format = standard\n"
                              "default outputs = f\n"
                              "[outputs]\n"
                              "f = file, path=LOGPATH\n"
                              "[rules]\n"
                              "*.* = standard, f\n");

    /* Dry-run: the referenced log file must NOT be created. */
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)), HPULOGC_OK);
    CHECK_EQ(err[0], '\0');
}

TEST(conf_validate_accepts_negate_rule_and_new_placeholders)
{
    char err[256];
    const char* p = write_cfg("v064",
                              "[global]\n"
                              "default format = standard\n"
                              "default outputs = f\n"
                              "[formats]\n"
                              "utc = \"%g %-8level %.30msg%n\"\n"
                              "[outputs]\n"
                              "f = file, path=LOGPATH\n"
                              "[rules]\n"
                              "svc.!INFO = utc, f\n"
                              "*.* = standard, f\n");

    /* v0.6.4 lexing: "!LEVEL" rule plus %g and width/precision modifiers
     * must validate as a legal configuration (dry-run, no side effects). */
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)), HPULOGC_OK);
    CHECK_EQ(err[0], '\0');
}

TEST(conf_validate_rejects_invalid_negate_form)
{
    char err[256];
    const char* p = write_cfg("badneg",
                              "[global]\n"
                              "default format = standard\n"
                              "default outputs = f\n"
                              "[outputs]\n"
                              "f = file, path=LOGPATH\n"
                              "[rules]\n"
                              "svc.!* = standard, f\n");

    err[0] = 'x';
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)),
             HPULOGC_ERR_CONFIG);
    CHECK(err[0] != 'x');
    /* parse diagnostic carries file and line (the rule is line 7) */
    CHECK(strstr(err, ":7:") != NULL);
}

TEST(conf_validate_invalid_reports_file_line)
{
    char err[256];
    char line[300];
    const char* p;

    /* Unknown key with strict init: parse error carrying file:line. */
    snprintf(line, sizeof(line),
             "[global]\n"
             "strict init = true\n"
             "bogus_key = 42\n"
             "default format = standard\n"
             "default outputs = f\n"
             "[outputs]\n"
             "f = file, path=LOGPATH\n"
             "[rules]\n"
             "*.* = standard, f\n");
    p = write_cfg("badkey", line);

    err[0] = 'x';
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)),
             HPULOGC_ERR_CONFIG);
    CHECK(err[0] != 'x');
    /* First diagnostic names the file and the offending line (3). */
    CHECK(strstr(err, g_cfgpath) != NULL || strstr(err, "hpu_cval") != NULL);
    CHECK(strstr(err, ":3:") != NULL);
}

TEST(conf_validate_semantic_error_no_line)
{
    char err[256];
    const char* p = write_cfg("badref",
                              "[global]\n"
                              "default format = standard\n"
                              "default outputs = f\n"
                              "[outputs]\n"
                              "f = file, path=LOGPATH\n"
                              "[rules]\n"
                              "*.* = nosuchformat, f\n");

    /* Reference errors come from finalize: config error without line. */
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)),
             HPULOGC_ERR_CONFIG);
    CHECK(strstr(err, "config error") != NULL);
}

TEST(conf_validate_strict_override)
{
    char err[256];
    const char* p = write_cfg("strict",
                              "[global]\n"
                              "bogus_key = 42\n"
                              "default format = standard\n"
                              "default outputs = f\n"
                              "[outputs]\n"
                              "f = file, path=LOGPATH\n"
                              "[rules]\n"
                              "*.* = standard, f\n");

    /* Default (no 'strict init' in file): unknown keys are errors. */
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)),
             HPULOGC_ERR_CONFIG);
    /* Explicit lenient override ignores the unknown key. */
    CHECK_EQ(hpulogc_conf_validate(p, 0, err, sizeof(err)), HPULOGC_OK);
    /* Explicit strict override keeps flagging the unknown key. */
    CHECK_EQ(hpulogc_conf_validate(p, 1, err, sizeof(err)),
             HPULOGC_ERR_CONFIG);
}

TEST(conf_validate_arg_and_io_errors)
{
    char err[256];

    CHECK_EQ(hpulogc_conf_validate(NULL, -1, err, sizeof(err)),
             HPULOGC_ERR_INVALID_ARG);
    /* Unreadable file: ERR_IO with a diagnostic. */
    err[0] = 'x';
    CHECK_EQ(hpulogc_conf_validate("/nonexistent/hpulogc_cval.ini", -1, err,
                                   sizeof(err)),
             HPULOGC_ERR_IO);
    CHECK(strstr(err, "cannot read") != NULL);
}

TEST(conf_validate_works_while_running)
{
    /* Independent of the runtime state: queryable before init and without
     * disturbing a running instance (spec: dry-run, no side effects). */
    char err[256];
    const char* p;

    CHECK_EQ(hpulogc_init_default(), HPULOGC_OK);

    p = write_cfg("running",
                  "[global]\n"
                  "default format = standard\n"
                  "default outputs = f\n"
                  "[outputs]\n"
                  "f = file, path=LOGPATH\n"
                  "[rules]\n"
                  "*.* = standard, f\n");
    CHECK_EQ(hpulogc_conf_validate(p, -1, err, sizeof(err)), HPULOGC_OK);
    /* The running instance is untouched. */
    HPULOGC_INFO("cval", "still running");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    hpulogc_shutdown();
}
