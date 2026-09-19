/**
 * @file posix_signal.c
 * @brief POSIX implementation of SIGHUP registration for hot reload.
 *
 * The handler only stores into a volatile sig_atomic_t flag (async-signal-
 * safe); the watcher thread converts it into a reload request.
 */

#include "platform/platform.h"

#include <errno.h>
#include <signal.h>
#include <string.h>

/** @brief Pending SIGHUP flag; written by the signal handler only. */
static volatile sig_atomic_t g_hup_pending = 0;

/**
 * @brief Signal handler: flag the pending reload, nothing else.
 * @param sig  Signal number (unused).
 */
static void hpu_hup_handler(int sig)
{
    (void)sig;
    g_hup_pending = 1;
}

int hpu_signal_install_hup(void)
{
    struct sigaction sa;
    struct sigaction old;
    int rc;

    /* Respect an existing application handler (spec 10.3). */
    rc = sigaction(SIGHUP, NULL, &old);
    if (rc != 0) {
        return -1;
    }
    if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
        errno = EBUSY;
        return -1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = hpu_hup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

int hpu_signal_take_hup(void)
{
    sig_atomic_t v = g_hup_pending;
    g_hup_pending = 0;
    return v != 0;
}
