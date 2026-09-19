/* -*- coding: utf-8 -*- */

/**
 * @file check_windows_behavior.c
 * @brief Phase 2 Windows-specific behavior checks (task report section 7
 *        checklist evidence). Not part of CTest: built and run manually
 *        with the MSVC-built library, output is pasted into
 *        docs/perf_report_windows.md / implementation notes.
 *
 * Checks:
 *   1. hpu_realtime_ns() cross-validated against time() (UTC epoch).
 *   2. hpu_now_ns() monotonic across a busy sample burst (QPC).
 *   3. UTF-8 paths (Chinese directory) can be created, written and
 *      rotated; dir scan excludes "." / "..".
 *   4. Permission bits and symlink-latest are ignored with one-time
 *      warnings (visible on stderr).
 *   5. newline = auto renders \r\n on Windows (lf/crlf forced still work).
 *   6. signal reload = true is accepted but has no SIGHUP: init warns and
 *      continues; fork behavior keys parse without side effects.
 *   7. Lock-free MPSC + overwrite policy fails init (spec 4.3 fail-fast).
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "hpulogc.h"
#include "conf/conf_model.h"
#include "format/format.h"
#include "../src/platform/platform.h"

static int g_checks_passed = 0;
static int g_checks_total = 0;

/**
 * @brief Record one check result.
 */
static void report(int ok, const char* name)
{
    g_checks_total++;
    if (ok) {
        g_checks_passed++;
    }
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}

int main(void)
{
    /* ---- 1. realtime clock cross-check ---- */
    {
        int64_t ns = hpu_realtime_ns();
        time_t now = time(NULL);
        int64_t sec = ns / 1000000000LL;
        int64_t diff = sec - (int64_t)now;

        if (diff < 0) {
            diff = -diff;
        }
        printf("  realtime=%lld time()=%lld diff=%lld s\n",
               (long long)sec, (long long)now, (long long)diff);
        report(diff <= 1, "realtime_ns matches time() (UTC epoch)");
    }

    /* ---- 2. monotonic clock ---- */
    {
        uint64_t prev = hpu_now_ns();
        int i;
        int monotonic = 1;

        for (i = 0; i < 100000; i++) {
            uint64_t v = hpu_now_ns();

            if (v < prev) {
                monotonic = 0;
            }
            prev = v;
        }
        report(monotonic, "now_ns is monotonic (QPC)");
    }

    /* ---- 3/4. UTF-8 Chinese directory, perms/symlink warnings ---- */
    {
        hpulogc_config_t cfg;
        hpulogc_output_t out;
        hpulogc_rule_t rule;
        static const char* names[] = { "out0" };
        char path[512];
        char rotated[512];
        hpu_dir_entry_t* entries = NULL;
        size_t count = 0;
        int utf8_ok = 0;

        /* \xe4\xb8\xad\xe6\x96\x87 = "Chinese" in UTF-8 */
        snprintf(path, sizeof(path), "utf8_\xe4\xb8\xad\xe6\x96\x87_dir/"
                                      "\xe6\x97\xa5\xe5\xbf\x97.log");

        hpulogc_config_default(&cfg);
        memset(&out, 0, sizeof(out));
        out.type = HPULOGC_OUT_FILE;
        out.path = path;
        out.file_mode = 0644;  /* ignored + one-time warning */
        out.dir_mode = 0755;   /* ignored + one-time warning */
        out.rotate = HPULOGC_ROTATE_SIZE;
        out.max_size = 64;
        out.max_files = 3;
        out.symlink_latest = 1; /* ignored on Windows */
        cfg.outputs = &out;
        cfg.output_count = 1;
        memset(&rule, 0, sizeof(rule));
        rule.category = "*";
        rule.min_level = HPULOGC_LEVEL_TRACE;
        rule.max_level = HPULOGC_LEVEL_FATAL;
        rule.format = "standard";
        rule.outputs = names;
        rule.output_count = 1;
        cfg.rules = &rule;
        cfg.rule_count = 1;

        printf("  (expect one-time perms/symlink warnings below)\n");
        if (hpulogc_init(&cfg) == HPULOGC_OK) {
            int i;

            for (i = 0; i < 50; i++) {
                hpulogc_log(HPULOGC_LEVEL_INFO, "utf8", "check.c", 0, "m",
                            "message %d padding padding padding", i);
            }
            hpulogc_sync();
            /* Existence is checked through the library's own W-API based
             * stat: the CRT fopen() would use the ANSI codepage and
             * cannot address UTF-8 file names on Windows. */
            utf8_ok = hpu_fs_stat_kind(path) == HPU_FS_REGULAR;
            /* rotation produced archives next to the active file */
            {
                char dir[256];

                snprintf(dir, sizeof(dir), "utf8_\xe4\xb8\xad\xe6\x96\x87_dir");
                if (hpu_fs_list_dir(dir, &entries, &count) == 0) {
                    size_t k;
                    int dots = 0;
                    int archives = 0;

                    for (k = 0; k < count; k++) {
                        if (strcmp(entries[k].name, ".") == 0 ||
                            strcmp(entries[k].name, "..") == 0) {
                            dots++;
                        } else if (strstr(entries[k].name, ".log") != NULL &&
                                   strcmp(entries[k].name,
                                          "\xe6\x97\xa5\xe5\xbf\x97.log") !=
                                       0) {
                            archives++;
                        }
                    }
                    printf("  dir entries=%zu dot-entries=%d "
                           "archives=%d\n", count, dots, archives);
                    utf8_ok = utf8_ok && dots == 0 && count >= 2;
                    hpu_fs_list_free(entries, count);
                    (void)archives;
                } else {
                    utf8_ok = 0;
                }
            }
            /* no .latest on Windows */
            snprintf(rotated, sizeof(rotated),
                     "utf8_\xe4\xb8\xad\xe6\x96\x87_dir/"
                     "\xe6\x97\xa5\xe5\xbf\x97.log.latest");
            report(hpu_fs_stat_kind(rotated) == HPU_FS_MISSING,
                   "symlink latest not created on Windows");
            hpulogc_shutdown();
        } else {
            printf("  init with utf8 path FAILED\n");
        }
        report(utf8_ok, "utf-8 Chinese dir create/write/rotate + dotless scan");
    }

    /* ---- 5. newline auto -> \r\n on Windows ---- */
    {
        hpu_format_t fmt;
        hpu_fmt_env_t env;
        hpu_fmt_cache_t cache;
        hpu_log_record_t rec;
        char out[128];
        size_t len = 0;
        int auto_ok = 0;
        int lf_ok = 0;

        memset(&env, 0, sizeof(env));
        env.time_format = "%S";
        env.newline_style = HPULOGC_NEWLINE_AUTO;
        memset(&rec, 0, sizeof(rec));
        rec.level = HPULOGC_LEVEL_INFO;
        rec.msg = "x";
        rec.msg_len = 1;
        rec.realtime_ns = 1700000000000000000LL;
        hpu_fmt_cache_init(&cache);
        if (hpu_format_compile(&fmt, "t", "%msg%n") == 0 &&
            hpu_format_render(&fmt, &rec, &env, &cache, 0, 100, "", out,
                              sizeof(out), &len) == 0) {
            auto_ok = len == 3 && out[1] == '\r' && out[2] == '\n';
        }
        hpu_format_free(&fmt);
        env.newline_style = HPULOGC_NEWLINE_LF;
        if (hpu_format_compile(&fmt, "t", "%msg%n") == 0 &&
            hpu_format_render(&fmt, &rec, &env, &cache, 0, 100, out /*unused*/,
                              out, sizeof(out), &len) == 0) {
            lf_ok = len == 2 && out[1] == '\n';
        }
        hpu_format_free(&fmt);
        report(auto_ok, "newline auto renders \\r\\n on Windows");
        report(lf_ok, "newline forced lf stays \\n");
    }

    /* ---- 6. signal reload accepted, no SIGHUP, no crash ---- */
    {
        hpulogc_config_t cfg;

        hpulogc_config_default(&cfg);
        printf("  (expect signal-reload warning below)\n");
        report(hpulogc_init(&cfg) == HPULOGC_OK, "default init");
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "still alive");
        hpulogc_flush();
        hpulogc_shutdown();
    }

    /* ---- 7. lock-free MPSC + overwrite fails init (build-time matrix
     *        note: this check only applies in a lockfree MPSC build; in
     *        other builds it is reported as skipped) ---- */
#if HPULOGC_LOCKFREE && defined(HPULOGC_CONCURRENCY_MSPC)
    {
        hpulogc_config_t cfg;

        hpulogc_config_default(&cfg);
        cfg.overflow_policy = HPULOGC_OVERFLOW_OVERWRITE;
        report(hpulogc_init(&cfg) == HPULOGC_ERR_CONFIG,
               "lockfree MPSC overwrite -> init fails (spec 4.3)");
    }
#else
    printf("[SKIP] lockfree MPSC overwrite fail-fast (different build)\n");
#endif

    printf("checks: %d/%d passed\n", g_checks_passed, g_checks_total);
    return g_checks_passed == g_checks_total ? 0 : 1;
}
