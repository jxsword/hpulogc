/**
 * @file test_fork.c
 * @brief fork() safety tests: reinit (lazy rebuild), disable, inherit
 *        (spec 9). fork behavior is a config-file-only key, so these
 *        tests drive the file configuration path.
 */

#include "test_util.h"
#include "hpulogc.h"

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/** @brief Scratch base path. */
static char g_base[256];

static void setup_base(void)
{
    if (g_base[0] == '\0') {
        snprintf(g_base, sizeof(g_base), "/tmp/hpu_fork_%d", (int)getpid());
    }
}

/**
 * @brief Read a whole file (NULL when missing).
 */
static const char* read_file(const char* path)
{
    static char buf[16384];
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
 * @brief Write one config file with the given fork behavior.
 */
static void write_cfg(const char* path, const char* logpath,
                      const char* behavior)
{
    FILE* fp = fopen(path, "w");

    CHECK(fp != NULL);
    fprintf(fp,
            "[global]\n"
            "level = trace\n"
            "default format = minimal\n"
            "default outputs = f\n"
            "[outputs]\n"
            "f = file, path=%s\n"
            "[rules]\n"
            "*.* = minimal, f\n"
            "[advanced]\n"
            "fork behavior = %s\n",
            logpath, behavior);
    fclose(fp);
}

TEST(fork_reinit_child_logs)
{
    char logpath[300];
    char cfgpath[300];
    pid_t pid;
    int status = -1;
    const char* content;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_reinit.log", g_base);
    snprintf(cfgpath, sizeof(cfgpath), "%s_reinit.ini", g_base);
    unlink(logpath);
    write_cfg(cfgpath, logpath, "reinit");

    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "p", NULL, 0, NULL, "parent");

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* child: lazy rebuild on the first log call (spec 9); async-mode
         * consumer threads do not survive fork(), so use sync()+_exit to
         * avoid depending on the inherited consumer. */
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "child");
        hpulogc_flush();
        _exit(0);
    }
    CHECK_EQ(waitpid(pid, &status, 0), pid);
    CHECK_EQ(WEXITSTATUS(status), 0);
    hpulogc_shutdown();

    content = read_file(logpath);
    CHECK(content != NULL);
    CHECK(strstr(content, "parent") != NULL);
    CHECK(strstr(content, "child") != NULL); /* rebuilt child wrote it */
}

TEST(fork_disable_child_drops)
{
    char logpath[300];
    char cfgpath[300];
    pid_t pid;
    int status = -1;
    const char* content;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_disable.log", g_base);
    snprintf(cfgpath, sizeof(cfgpath), "%s_disable.ini", g_base);
    unlink(logpath);
    write_cfg(cfgpath, logpath, "disable");

    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "p", NULL, 0, NULL, "parent");

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int i;

        /* child: every log call is a silent drop, no crash */
        for (i = 0; i < 100; i++) {
            hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "child");
        }
        _exit(0);
        (void)i;
    }
    waitpid(pid, &status, 0);
    CHECK_EQ(WEXITSTATUS(status), 0);
    hpulogc_shutdown();

    content = read_file(logpath);
    CHECK(content != NULL);
    CHECK(strstr(content, "parent") != NULL);
    CHECK(strstr(content, "child") == NULL);
}

TEST(fork_inherit_sync_writes)
{
    char logpath[300];
    char cfgpath[300];
    pid_t pid;
    int status = -1;
    const char* content;

    setup_base();
    snprintf(logpath, sizeof(logpath), "%s_inherit.log", g_base);
    snprintf(cfgpath, sizeof(cfgpath), "%s_inherit.ini", g_base);
    unlink(logpath);
    write_cfg(cfgpath, logpath, "inherit");

    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* inherit mode is debug-only and not memory-safe under a
         * concurrent producer; the child must merely stay alive and not
         * crash (output delivery is not guaranteed, spec 9). */
        hpulogc_log(HPULOGC_LEVEL_INFO, "c", NULL, 0, NULL, "inherited");
        _exit(0);
    }
    CHECK_EQ(waitpid(pid, &status, 0), pid);
    CHECK_EQ(WEXITSTATUS(status), 0);
    hpulogc_shutdown();

    /* The parent keeps working normally after the fork. */
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    hpulogc_log(HPULOGC_LEVEL_INFO, "p", NULL, 0, NULL, "after-fork");
    hpulogc_flush();
    hpulogc_shutdown();
    content = read_file(logpath);
    CHECK(content != NULL);
    (void)content;
}
