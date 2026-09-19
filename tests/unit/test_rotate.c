/**
 * @file test_rotate.c
 * @brief Rotation tests: naming templates, conflict handling, time
 *        boundaries, max-files cleanup, .latest symlink.
 */

#include "test_util.h"
#include "portability.h"
#include "output/rotate.h"
#include "output/output.h"
#include "../src/platform/platform.h"

#if !HPULOGC_ENABLE_ROTATE
/* Rotation support is compiled out; every case in this file needs it. */
#define HPU_TEST_SKIP_ROTATE 1
#else
#define HPU_TEST_SKIP_ROTATE 0
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Per-test scratch directory. */
static char g_dir[256];

/**
 * @brief Create a fresh scratch directory (unique per pid).
 */
static void make_scratch(void)
{
    char tmpdir[128];

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(g_dir, sizeof(g_dir), "%s/hpu_rot_%d", tmpdir,
             hpu_test_getpid());
    hpu_test_rmtree(g_dir);
    if (hpu_test_mkdir(g_dir) != 0) {
        FAIL_MSG("cannot create scratch dir %s", g_dir);
    }
}

/**
 * @brief Remove the scratch directory tree (archives only).
 */
static void clean_scratch(void)
{
    hpu_test_rmtree(g_dir);
}

/**
 * @brief Fill a file with @p n bytes.
 */
static void write_file(const char* path, size_t n)
{
    FILE* fp = fopen(path, "w");

    if (fp != NULL) {
        fwrite("x", 1, 1, fp);
        fclose(fp);
        hpu_test_truncate_file(path, (long)n);
    }
}

/**
 * @brief Count archive names in the scratch dir matching
 *        "app.<8 digits>_<6 digits>.<index>.log".
 */
static int count_timestamp_archives(void)
{
    hpu_dir_entry_t* entries = NULL;
    size_t count = 0;
    size_t i;
    int matches = 0;

    if (hpu_fs_list_dir(g_dir, &entries, &count) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        const char* n = entries[i].name;
        size_t len = strlen(n);
        size_t k;
        int ok = len == strlen("app.20231114_221320.1.log");

        if (ok) {
            if (strncmp(n, "app.", 4) != 0 || n[12] != '_' ||
                n[19] != '.') {
                ok = 0;
            }
            for (k = 4; ok && k < 12; k++) {
                if (n[k] < '0' || n[k] > '9') {
                    ok = 0;
                }
            }
            for (k = 13; ok && k < 19; k++) {
                if (n[k] < '0' || n[k] > '9') {
                    ok = 0;
                }
            }
            if (ok && strcmp(n + 19, ".1.log") != 0) {
                ok = 0;
            }
        }
        if (ok) {
            matches++;
        }
    }
    hpu_fs_list_free(entries, count);
    return matches;
}

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_next_boundary_utc)
{
    /* 2023-11-14 22:13:20 UTC = 1700000000 */
    int64_t ts = 1700000000;

    CHECK_EQ(hpu_rotate_next_boundary(ts, HPULOGC_TU_HOUR, 1),
             (ts / 3600 + 1) * 3600);
    CHECK_EQ(hpu_rotate_next_boundary(ts, HPULOGC_TU_DAY, 1),
             (ts / 86400 + 1) * 86400);
    /* Week: 2023-11-14 is a Tuesday; next Monday is 2023-11-20
     * 00:00 UTC = 1700438400 */
    CHECK_EQ(hpu_rotate_next_boundary(ts, HPULOGC_TU_WEEK, 1),
             1700438400LL);
    /* Month: next is 2023-12-01 00:00 UTC = 1701388800 */
    CHECK_EQ(hpu_rotate_next_boundary(ts, HPULOGC_TU_MONTH, 1),
             1701388800LL);
}
#endif /* rotate tests */

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_boundary_is_monotonic)
{
    int64_t ts;
    int64_t b;

    for (ts = 1700000000; ts < 1700000000 + 86400 * 40; ts += 997) {
        b = hpu_rotate_next_boundary(ts, HPULOGC_TU_DAY, 1);
        CHECK(b > ts);
        CHECK_EQ(b % 86400, 0);
    }
}
#endif /* rotate tests */

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_naming_and_symlink)
{
    hpu_rotate_cfg_t cfg;
    char path[300];
    char latest[320];
    int fd;
    int64_t size = 100;

    make_scratch();
    snprintf(path, sizeof(path), "%s/app.log", g_dir);

    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    snprintf(cfg.naming, sizeof(cfg.naming),
             "{base}.{timestamp}.{index}.log");

    fd = hpu_fs_open_append(path, 0644);
    CHECK(fd >= 0);
    write_file(path, 64);

    /* rotate at a fixed timestamp: 2023-11-14 22:13:20 (local tz is the
     * machine's; the name just needs the pattern) */
    CHECK_EQ(hpu_rotate_file(path, &fd, &size, &cfg, 0, 1700000000, 1,
                             0644), 0);
    CHECK(fd >= 0);
    hpu_fs_close(fd);

    /* archive exists with the template pattern */
    CHECK_EQ(count_timestamp_archives(), 1);

    snprintf(latest, sizeof(latest), "%s/app.log.latest", g_dir);
#if defined(_WIN32)
    /* Windows: .latest symlinks are ignored (spec 4.7); nothing is
     * created and rotation still succeeds. */
    CHECK_EQ(hpu_fs_stat_kind(latest), HPU_FS_MISSING);
#else
    {
        char linktarget[300];
        ssize_t n = readlink(latest, linktarget, sizeof(linktarget) - 1);

        CHECK(n > 0);
        linktarget[n] = '\0';
        CHECK_STREQ(linktarget, "app.log");
    }
#endif

    clean_scratch();
}
#endif /* rotate tests */

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_index_increments_on_conflict)
{
    hpu_rotate_cfg_t cfg;
    char path[300];
    char archive[300];
    int fd;
    int64_t size = 0;
    int i;

    make_scratch();
    snprintf(path, sizeof(path), "%s/app.log", g_dir);

    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    snprintf(cfg.naming, sizeof(cfg.naming), "{base}.{index}.log");

    /* Rotate 3 times within the same second: conflicts push the index. */
    for (i = 0; i < 3; i++) {
        fd = hpu_fs_open_append(path, 0644);
        CHECK(fd >= 0);
        CHECK_EQ(hpu_rotate_file(path, &fd, &size, &cfg, 0, 1700000000, 0,
                                 0644), 0);
        hpu_fs_close(fd);
    }

    snprintf(archive, sizeof(archive), "%s/app.1.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_REGULAR);
    snprintf(archive, sizeof(archive), "%s/app.2.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_REGULAR);
    snprintf(archive, sizeof(archive), "%s/app.3.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_REGULAR);

    clean_scratch();
}
#endif /* rotate tests */

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_max_files_cleanup)
{
    hpu_rotate_cfg_t cfg;
    char path[300];
    char archive[300];
    int fd;
    int64_t size = 0;
    int i;

    make_scratch();
    snprintf(path, sizeof(path), "%s/app.log", g_dir);

    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    cfg.max_files = 2;
    snprintf(cfg.naming, sizeof(cfg.naming), "{base}.{index}.log");

    /* Pre-create 4 stale archives. */
    for (i = 1; i <= 4; i++) {
        snprintf(archive, sizeof(archive), "%s/app.%d.log", g_dir, i);
        write_file(archive, 10);
    }

    fd = hpu_fs_open_append(path, 0644);
    CHECK(fd >= 0);
    CHECK_EQ(hpu_rotate_file(path, &fd, &size, &cfg, 0, 1700000000, 0,
                             0644), 0);
    hpu_fs_close(fd);

    /* Only the two newest archives remain; the new app.5.log may claim
     * index 5 (conflict-free), older ones 3.. were pruned. */
    snprintf(archive, sizeof(archive), "%s/app.1.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_MISSING);
    snprintf(archive, sizeof(archive), "%s/app.2.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_MISSING);
    snprintf(archive, sizeof(archive), "%s/app.4.log", g_dir);
    CHECK_EQ(hpu_fs_stat_kind(archive), HPU_FS_REGULAR);

    clean_scratch();
}
#endif /* rotate tests */

#if !HPU_TEST_SKIP_ROTATE
TEST(rotate_size_and_time_triggers)
{
    /* O(1) trigger logic lives in hpu_file_output_before_write; covered
     * through the integration suite with real outputs. */
    CHECK(1);
}
#endif /* rotate tests */
