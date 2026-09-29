/**
 * @file test_ext_rotate.c
 * @brief External rotation detection (spec 4.6, A.1 #5): logrotate-style
 *        mv + recreate of the open active file, throttled inode/dev
 *        comparison, no false positives, self-rotation coexistence and
 *        the write-failure reopen-once synergy.
 */

#include "test_util.h"
#include "hpulogc.h"
#include "output/output.h"
#include "../src/platform/platform.h"
#include "portability.h"

#include <stdio.h>
#include <string.h>

/** @brief Detection window (must match output_file.c, D-R11). */
#define INODE_CHECK_EVERY 1024

/** @brief Shared scratch paths per test (tests run sequentially). */
static char g_log[300];
static char g_moved[320];
static char g_dir[256];

/** @brief Line reader scratch (largest file is ~1105 rendered lines). */
static char g_buf[1024 * 1024];

/**
 * @brief Initialize the library with a single file output.
 */
static void init_file_logger(const char* path, hpulogc_rotate_t rot,
                             size_t max_size)
{
    hpulogc_config_t cfg;
    hpulogc_output_t out;
    static const char* names[] = { "out0" };
    hpulogc_rule_t rule;

    hpulogc_config_default(&cfg);
    memset(&out, 0, sizeof(out));
    out.type = HPULOGC_OUT_FILE;
    out.path = path;
    out.rotate = rot;
    out.max_size = max_size;
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

    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);
}

/**
 * @brief Write @p count unique lines starting at index @p from.
 */
static void write_lines(int from, int count)
{
    int i;

    for (i = from; i < from + count; i++) {
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "line %d", i);
    }
}

/**
 * @brief Simulate logrotate: mv the active file aside, recreate an empty
 *        file at the original path. Stores the moved path in g_moved.
 */
static void external_rotate(void)
{
    FILE* fp;

    snprintf(g_moved, sizeof(g_moved), "%s.moved", g_log);
    hpu_test_unlink(g_moved);
    /* Works on POSIX always; on Windows the library opens with
     * FILE_SHARE_DELETE (D-R12), so the rename succeeds too. */
    CHECK_EQ(hpu_test_rename(g_log, g_moved), 0);
    fp = fopen(g_log, "w");
    CHECK(fp != NULL);
    fclose(fp);
}

/**
 * @brief Read a whole file; returns the number of '\n'-terminated lines.
 */
static long read_file_lines(const char* path, char* buf, size_t bufsz)
{
    FILE* fp = fopen(path, "rb");
    size_t n = 0;
    long lines = 0;
    const char* p;

    if (fp != NULL) {
        n = fread(buf, 1, bufsz - 1, fp);
        fclose(fp);
    }
    buf[n] = '\0';
    p = buf;
    while ((p = strchr(p, '\n')) != NULL) {
        lines++;
        p++;
    }
    return lines;
}

/**
 * @brief Check that a rendered log line with @p marker exists. The message
 *        is the last field; the line terminator is "\n" on POSIX and
 *        "\r\n" on Windows (format.c), so the boundary check accepts both.
 */
static int contains_log_line(const char* buf, const char* marker)
{
    size_t mlen = strlen(marker);
    const char* p = buf;

    while ((p = strstr(p, marker)) != NULL) {
        char c = p[mlen];

        if (c == '\n' || c == '\r') {
            return 1;
        }
        p++;
    }
    return 0;
}

TEST(ext_no_false_positive_within_window)
{
    hpulogc_stats_t st;
    char tmpdir[128];

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_log, sizeof(g_log), "%s/hpu_ext_a_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(g_log);
    snprintf(g_moved, sizeof(g_moved), "%s.moved", g_log);
    hpu_test_unlink(g_moved);

    init_file_logger(g_log, HPULOGC_ROTATE_NONE, 0);
    write_lines(0, 5);
    external_rotate();

    /* Within the throttle window the writes continue into the old
     * (renamed) file; the recreated file must stay untouched. */
    write_lines(5, 10);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    hpulogc_shutdown();

    CHECK_EQ(read_file_lines(g_log, g_buf, sizeof(g_buf)), 0);
    CHECK_EQ(read_file_lines(g_moved, g_buf, sizeof(g_buf)), 15);
    CHECK(contains_log_line(g_buf, "line 10"));
    CHECK_EQ(st.dropped, 0);

    hpu_test_unlink(g_log);
    hpu_test_unlink(g_moved);
}

TEST(ext_detected_and_reopened)
{
    hpulogc_stats_t st;
    char tmpdir[128];
    long old_lines;
    long new_lines;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_log, sizeof(g_log), "%s/hpu_ext_b_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(g_log);
    snprintf(g_moved, sizeof(g_moved), "%s.moved", g_log);
    hpu_test_unlink(g_moved);

    init_file_logger(g_log, HPULOGC_ROTATE_NONE, 0);
    /* 5 lines pre-replace; detection fires on the line that makes the
     * counter reach 1024, i.e. line index 1022 (0-based) — that line and
     * everything after go to the recreated file. */
    write_lines(0, 5);
    external_rotate();
    write_lines(5, 1100);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    hpulogc_shutdown();

    old_lines = read_file_lines(g_moved, g_buf, sizeof(g_buf));
    CHECK_EQ(old_lines, INODE_CHECK_EVERY - 1);
    read_file_lines(g_moved, g_buf, sizeof(g_buf));
    CHECK(contains_log_line(g_buf, "line 1022"));
    CHECK(!contains_log_line(g_buf, "line 1023"));

    new_lines = read_file_lines(g_log, g_buf, sizeof(g_buf));
    CHECK_EQ(new_lines, 1105 - (INODE_CHECK_EVERY - 1));
    read_file_lines(g_log, g_buf, sizeof(g_buf));
    CHECK(contains_log_line(g_buf, "line 1023"));
    CHECK(contains_log_line(g_buf, "line 1104"));
    CHECK(!contains_log_line(g_buf, "line 1022"));
    CHECK_EQ(st.dropped, 0);

    hpu_test_unlink(g_log);
    hpu_test_unlink(g_moved);
}

TEST(ext_no_line_lands_in_both_files)
{
    static char old_buf[1024 * 1024];
    static char new_buf[1024 * 1024];
    char tmpdir[128];
    long old_lines;
    long new_lines;
    long i;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_log, sizeof(g_log), "%s/hpu_ext_c_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(g_log);
    snprintf(g_moved, sizeof(g_moved), "%s.moved", g_log);
    hpu_test_unlink(g_moved);

    init_file_logger(g_log, HPULOGC_ROTATE_NONE, 0);
    write_lines(0, 5);
    external_rotate();
    write_lines(5, 1100);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    hpulogc_shutdown();

    old_lines = read_file_lines(g_moved, old_buf, sizeof(old_buf));
    new_lines = read_file_lines(g_log, new_buf, sizeof(new_buf));
    CHECK_EQ(old_lines + new_lines, 1105);

    /* Every line number appears exactly once, in exactly one file: lines
     * 0..1022 in the moved file, 1023..1104 in the recreated one. */
    for (i = 0; i < 1105; i++) {
        char marker[32];

        snprintf(marker, sizeof(marker), "line %ld", i);
        if (i < 1023) {
            CHECK(contains_log_line(old_buf, marker));
            CHECK(!contains_log_line(new_buf, marker));
        } else {
            CHECK(contains_log_line(new_buf, marker));
            CHECK(!contains_log_line(old_buf, marker));
        }
    }

    hpu_test_unlink(g_log);
    hpu_test_unlink(g_moved);
}

#if HPULOGC_ENABLE_ROTATE
TEST(ext_self_rotate_not_misdetected)
{
    hpulogc_stats_t st;
    char tmpdir[128];
    hpu_dir_entry_t* entries = NULL;
    size_t count = 0;
    size_t i;
    long total_lines = 0;
    int archives = 0;

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_dir, sizeof(g_dir), "%s/hpu_ext_d_%d", tmpdir,
             hpu_test_getpid());
    hpu_test_rmtree(g_dir);
    CHECK_EQ(hpu_test_mkdir(g_dir), 0);
    snprintf(g_log, sizeof(g_log), "%s/app.log", g_dir);

    /* Small max_size forces frequent self-rotation; the identity refresh
     * after each rotation must keep the throttled check quiet. */
    init_file_logger(g_log, HPULOGC_ROTATE_SIZE, 1024);
    write_lines(0, 300);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    hpulogc_shutdown();

    CHECK_EQ(st.written, 300);
    CHECK_EQ(st.dropped, 0);

    CHECK_EQ(hpu_fs_list_dir(g_dir, &entries, &count), 0);
    for (i = 0; i < count; i++) {
        char fpath[512];

        if (strcmp(entries[i].name, "app.log") == 0) {
            continue;
        }
        archives++;
        snprintf(fpath, sizeof(fpath), "%s/%s", g_dir, entries[i].name);
        total_lines += read_file_lines(fpath, g_buf, sizeof(g_buf));
    }
    hpu_fs_list_free(entries, count);
    total_lines += read_file_lines(g_log, g_buf, sizeof(g_buf));

    /* No line lost or duplicated by a misdetection-triggered reopen. */
    CHECK(archives >= 1);
    CHECK_EQ(total_lines, 300);

    hpu_test_rmtree(g_dir);
}
#endif /* self-rotate needs HPULOGC_ENABLE_ROTATE */

/** @brief Write-failure injection hook (one op=1 failure, then pass). */
static int g_hook_arm;
static int recovery_fail_hook(int op, const char* path)
{
    (void)path;
    if (g_hook_arm && op == 1) {
        g_hook_arm = 0;
        return 1; /* fail the first write once */
    }
    return 0;
}

TEST(ext_detection_after_write_fail_reopen)
{
    hpulogc_stats_t st;
    char tmpdir[128];
    extern int (*hpu_io_fail_hook)(int op, const char* path);

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_log, sizeof(g_log), "%s/hpu_ext_e_%d.log", tmpdir,
             hpu_test_getpid());
    hpu_test_unlink(g_log);
    snprintf(g_moved, sizeof(g_moved), "%s.moved", g_log);
    hpu_test_unlink(g_moved);

    init_file_logger(g_log, HPULOGC_ROTATE_NONE, 0);

    /* One write failure: reopen-once recovery (spec 9). The recovery
     * reopen must refresh the recorded identity. */
    hpu_io_fail_hook = recovery_fail_hook;
    g_hook_arm = 1;
    write_lines(0, 1);
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    hpu_io_fail_hook = NULL;
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    CHECK_EQ(st.dropped, 0);

    write_lines(1, 4);
    external_rotate();
    write_lines(5, 1100);
    CHECK_EQ(hpulogc_sync(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_stats(&st), HPULOGC_OK);
    hpulogc_shutdown();

    /* Detection still works after the failure reopen: the triggering line
     * is line 1024 (0-based); nothing was lost to a false negative. */
    read_file_lines(g_moved, g_buf, sizeof(g_buf));
    CHECK(contains_log_line(g_buf, "line 1023"));
    CHECK(!contains_log_line(g_buf, "line 1024"));
    CHECK_EQ(read_file_lines(g_moved, g_buf, sizeof(g_buf)), 1024);
    read_file_lines(g_log, g_buf, sizeof(g_buf));
    CHECK(contains_log_line(g_buf, "line 1024"));
    CHECK(contains_log_line(g_buf, "line 1104"));
    CHECK_EQ(st.dropped, 0);

    hpu_test_unlink(g_log);
    hpu_test_unlink(g_moved);
}
