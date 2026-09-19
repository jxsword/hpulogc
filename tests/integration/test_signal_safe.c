/**
 * @file test_signal_safe.c
 * @brief Signal-safe channel tests: async-signal-safe writes from a real
 *        signal handler, the signal_safe gate, and errno preservation.
 */

#include "test_util.h"
#include "hpulogc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/**
 * @brief Signal handler: only the signal-safe API may run here.
 */
static void test_signal_handler(int sig)
{
    hpulogc_log_signal_safe(HPULOGC_LEVEL_FATAL, "from handler");
    (void)sig;
}

/**
 * @brief Redirect fd 2 to a file; returns the saved fd (or -1).
 */
static int redirect_stderr(const char* path)
{
    int saved = dup(2);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);

    if (fd < 0) {
        close(saved);
        return -1;
    }
    dup2(fd, 2);
    close(fd);
    return saved;
}

/**
 * @brief Restore a previously redirected stderr.
 */
static void restore_stderr(int saved)
{
    if (saved >= 0) {
        dup2(saved, 2);
        close(saved);
    }
}

TEST(signal_safe_disabled_drops)
{
    char path[256];
    int saved;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg); /* signal_safe = 0 */
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    snprintf(path, sizeof(path), "/tmp/hpu_ss_%d_a.log", (int)getpid());
    saved = redirect_stderr(path);
    CHECK(saved >= 0);
    hpulogc_log_signal_safe(HPULOGC_LEVEL_FATAL, "should not appear");
    restore_stderr(saved);
    hpulogc_shutdown();

    {
        FILE* fp = fopen(path, "r");
        char buf[256] = { 0 };

        CHECK(fp != NULL);
        CHECK(fgets(buf, sizeof(buf), fp) == NULL); /* empty file */
        fclose(fp);
        unlink(path);
    }
}

TEST(signal_safe_enabled_from_handler)
{
    char path[256];
    int saved;
    struct sigaction sa;
    struct sigaction old;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg);
    cfg.signal_safe = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    snprintf(path, sizeof(path), "/tmp/hpu_ss_%d_b.log", (int)getpid());
    saved = redirect_stderr(path);
    CHECK(saved >= 0);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = test_signal_handler;
    sigemptyset(&sa.sa_mask);
    CHECK_EQ(sigaction(SIGUSR1, &sa, &old), 0);
    raise(SIGUSR1);
    sigaction(SIGUSR1, &old, NULL);

    restore_stderr(saved);
    hpulogc_shutdown();

    {
        FILE* fp = fopen(path, "r");
        char buf[256];

        CHECK(fp != NULL);
        CHECK(fgets(buf, sizeof(buf), fp) != NULL);
        CHECK(strstr(buf, "[hpulogc][FATAL] from handler") != NULL);
        fclose(fp);
        unlink(path);
    }
}

TEST(signal_safe_direct_call_and_errno)
{
    char path[256];
    int saved;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg);
    cfg.signal_safe = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    snprintf(path, sizeof(path), "/tmp/hpu_ss_%d_c.log", (int)getpid());
    saved = redirect_stderr(path);
    CHECK(saved >= 0);

    errno = 99;
    hpulogc_log_signal_safe(HPULOGC_LEVEL_ERROR, "direct");
    CHECK_EQ(errno, 99);
    hpulogc_log_signal_safe((hpulogc_level_t)77, "badlevel"); /* tagged LOG */
    restore_stderr(saved);
    hpulogc_shutdown();

    {
        FILE* fp = fopen(path, "r");
        char line1[256] = { 0 };
        char line2[256] = { 0 };

        CHECK(fp != NULL);
        CHECK(fgets(line1, sizeof(line1), fp) != NULL);
        CHECK(fgets(line2, sizeof(line2), fp) != NULL);
        CHECK(strstr(line1, "[hpulogc][ERROR] direct") != NULL);
        CHECK(strstr(line2, "[hpulogc][LOG] badlevel") != NULL);
        fclose(fp);
        unlink(path);
    }
}
