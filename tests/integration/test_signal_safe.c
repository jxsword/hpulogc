/**
 * @file test_signal_safe.c
 * @brief Signal-safe channel tests: async-signal-safe writes from a real
 *        signal handler, the signal_safe gate, and errno preservation.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>



/**
 * @brief Signal handler: only the signal-safe API may run here.
 *
 * Phase 2 (Windows): SIGUSR1 does not exist in the MSVC CRT, so the
 * handler is installed for SIGABRT instead (decision 31).
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
#if defined(_WIN32)
    int saved = _dup(2);
    int fd = _open(path, _O_CREAT | _O_TRUNC | _O_WRONLY, _S_IREAD |
                   _S_IWRITE);

    if (fd < 0) {
        _close(saved);
        return -1;
    }
    _dup2(fd, 2);
    _close(fd);
    return saved;
#else
    int saved = dup(2);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);

    if (fd < 0) {
        close(saved);
        return -1;
    }
    dup2(fd, 2);
    close(fd);
    return saved;
#endif
}

/**
 * @brief Restore a previously redirected stderr.
 */
static void restore_stderr(int saved)
{
#if defined(_WIN32)
    if (saved >= 0) {
        _dup2(saved, 2);
        _close(saved);
    }
#else
    if (saved >= 0) {
        dup2(saved, 2);
        close(saved);
    }
#endif
}

TEST(signal_safe_disabled_drops)
{
    char path[256];
    char tmpdir[128];
    int saved;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg); /* signal_safe = 0 */
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(path, sizeof(path), "%s/hpu_ss_%d_a.log", tmpdir,
             hpu_test_getpid());
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
        hpu_test_unlink(path);
    }
}

TEST(signal_safe_enabled_from_handler)
{
    char path[256];
    char tmpdir[128];
    int saved;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg);
    cfg.signal_safe = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(path, sizeof(path), "%s/hpu_ss_%d_b.log", tmpdir,
             hpu_test_getpid());
    saved = redirect_stderr(path);
    CHECK(saved >= 0);

#if defined(_WIN32)
    /* No SIGUSR1 in the MSVC CRT; SIGABRT carries the same semantics for
     * this test (decision 31). */
    {
        void (*old)(int) = signal(SIGABRT, test_signal_handler);

        CHECK(old != SIG_ERR);
        raise(SIGABRT);
        signal(SIGABRT, old);
    }
#else
    {
        struct sigaction sa;
        struct sigaction old;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = test_signal_handler;
        sigemptyset(&sa.sa_mask);
        CHECK_EQ(sigaction(SIGUSR1, &sa, &old), 0);
        raise(SIGUSR1);
        sigaction(SIGUSR1, &old, NULL);
    }
#endif

    restore_stderr(saved);
    hpulogc_shutdown();

    {
        FILE* fp = fopen(path, "r");
        char buf[256];

        CHECK(fp != NULL);
        CHECK(fgets(buf, sizeof(buf), fp) != NULL);
        CHECK(strstr(buf, "[hpulogc][FATAL] from handler") != NULL);
        fclose(fp);
        hpu_test_unlink(path);
    }
}

TEST(signal_safe_direct_call_and_errno)
{
    char path[256];
    char tmpdir[128];
    int saved;
    hpulogc_config_t cfg;

    hpulogc_config_default(&cfg);
    cfg.signal_safe = 1;
    CHECK_EQ(hpulogc_init(&cfg), HPULOGC_OK);

    hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
    snprintf(path, sizeof(path), "%s/hpu_ss_%d_c.log", tmpdir,
             hpu_test_getpid());
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
        hpu_test_unlink(path);
    }
}
