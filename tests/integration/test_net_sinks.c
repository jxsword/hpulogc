/* -*- coding: utf-8 -*- */

/**
 * @file test_net_sinks.c
 * @brief Network sink integration (rd_v0.6 §4.10.8): loopback self-send
 *        for udp/tcp, MTU truncation, start fail-fast, reconnect backoff
 *        timing and async queue-full drops.
 *
 * POSIX-only (the tcp/udp sinks are POSIX-only in v1): the test spins up
 * its own loopback listeners; sockets helpers live in this file (the
 * shared portability.h has no network support).
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>

/** @brief Overall receive timeout per wait step (ms). */
#define NET_RECV_TIMEOUT_MS 3000

#if defined(_WIN32) || !defined(HPULOGC_ENABLE_INI)
TEST(net_sinks_unsupported_noop)
{
    /* tcp/udp sinks are POSIX-only; INI-less builds cannot define them. */
    CHECK(1);
}
#else

/* ---- loopback listener helpers (test-local, POSIX) ---- */

/**
 * @brief Bind a loopback listener.
 * @param type  SOCK_STREAM or SOCK_DGRAM.
 * @param want_port  Port to bind (0 = pick an ephemeral port). Rebinding
 *                   a previously bound port needs SO_REUSEADDR (set).
 * @param out_port  Filled with the bound port.
 * @return  Bound socket (listening for SOCK_STREAM), -1 on failure.
 */
static int net_bind_listener(int type, int want_port, int* out_port)
{
    int fd = socket(AF_INET, type, 0);
    struct sockaddr_in sa;
    int on = 1;
    socklen_t len = sizeof(sa);

    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)want_port);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0 ||
        getsockname(fd, (struct sockaddr*)&sa, &len) != 0) {
        close(fd);
        return -1;
    }
    *out_port = ntohs(sa.sin_port);
    if (type == SOCK_STREAM && listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/**
 * @brief Wait for @p fd readability (bounded).
 * @return 1 ready, 0 timeout, -1 error.
 */
static int net_wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = POLLIN;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

/**
 * @brief Receive one datagram (bounded).
 * @return Byte count, 0 on timeout, -1 on error.
 */
static int net_recv_dgram(int fd, void* buf, size_t buflen)
{
    int rc = net_wait_readable(fd, NET_RECV_TIMEOUT_MS);

    if (rc <= 0) {
        return 0;
    }
    return (int)recvfrom(fd, buf, buflen, 0, NULL, NULL);
}

/**
 * @brief Accept one pending connection (bounded). The sinks connect at
 *        start, so the connection is already in the backlog.
 * @return Accepted fd, -1 on timeout/error.
 */
static int net_accept(int lfd)
{
    int rc = net_wait_readable(lfd, NET_RECV_TIMEOUT_MS);

    if (rc <= 0) {
        return -1;
    }
    return accept(lfd, NULL, NULL);
}

/**
 * @brief Read from @p fd until @p want line terminators arrived (bounded).
 * @return Total bytes read, -1 on timeout/error.
 */
static int net_read_lines(int fd, char* buf, size_t buflen, int want)
{
    size_t len = 0;
    int lines = 0;

    while (lines < want) {
        ssize_t got;
        size_t i;

        if (net_wait_readable(fd, NET_RECV_TIMEOUT_MS) <= 0) {
            return -1;
        }
        got = recv(fd, buf + len, buflen - len, 0);
        if (got <= 0) {
            return -1;
        }
        for (i = 0; i < (size_t)got; i++) {
            if (buf[len + i] == '\n') {
                lines++;
            }
        }
        len += (size_t)got;
        if (len >= buflen) {
            return -1;
        }
    }
    buf[len] = '\0';
    return (int)len;
}

/* ---- config helpers ---- */

static char g_base[256];

static void net_setup_base(void)
{
    char tmpdir[128];

    if (g_base[0] == '\0') {
        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(g_base, sizeof(g_base), "%s/hpu_net_%d", tmpdir,
                 hpu_test_getpid());
    }
}

static void net_write_config(const char* name, const char* text)
{
    char path[300];
    FILE* fp;

    snprintf(path, sizeof(path), "%s_%s", g_base, name);
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs(text, fp);
    fclose(fp);
}

#if defined(HPULOGC_SINK_UDP)

TEST(net_udp_loopback_basic)
{
    char cfgpath[300];
    char text[512];
    char buf[2048];
    hpulogc_sink_stats_t st;
    int lfd;
    int port;
    int i;
    int seen;

    net_setup_base();
    lfd = net_bind_listener(SOCK_DGRAM, 0, &port);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_udp.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = u0\n"
             "[outputs]\n"
             "u0 = udp, host=127.0.0.1, port=%d\n"
             "[rules]\n"
             "*.* = standard, u0\n",
             port);
    net_write_config("udp.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 5; i++) {
        HPULOGC_INFO("t", "udp hello %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("u0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    CHECK_EQ(st.failed, 0);
    CHECK_EQ(st.dropped, 0);
    hpulogc_shutdown();

    seen = 0;
    for (i = 0; i < 5; i++) {
        int n = net_recv_dgram(lfd, buf, sizeof(buf) - 1);

        if (n <= 0) {
            break;
        }
        buf[n] = '\0';
        CHECK(strstr(buf, "udp hello") != NULL);
        seen++;
    }
    CHECK_EQ(seen, 5);
    close(lfd);
}

TEST(net_udp_mtu_truncate)
{
    char cfgpath[300];
    char text[512];
    char bigmsg[2048];
    char buf[2048];
    hpulogc_sink_stats_t st;
    int lfd;
    int port;
    int n;

    net_setup_base();
    lfd = net_bind_listener(SOCK_DGRAM, 0, &port);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_udpx.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = u0\n"
             "[outputs]\n"
             "u0 = udp, host=127.0.0.1, port=%d, mtu=576\n"
             "[rules]\n"
             "*.* = standard, u0\n",
             port);
    net_write_config("udpx.ini", text);
    memset(bigmsg, 'A', sizeof(bigmsg) - 1);
    bigmsg[sizeof(bigmsg) - 1] = '\0'; /* 2000 'A's: rendered line >> mtu */

    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    HPULOGC_INFO("t", "%s", bigmsg); /* rendered line >> mtu=576 */
    /* hpulogc_flush only surfaces the ring-drain handshake result (core.c
     * consumer flush, 5 s bound); a CHECK failure here aborts the case
     * before shutdown, so later cases cascade with ERR_STATE and must not
     * be read as their own root cause. */
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("u0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 1); /* handed off, but degraded (below) */
    CHECK_EQ(st.failed, 1);  /* truncation counts as failed (§4.10.8) */
    hpulogc_shutdown();

    /* The truncated datagram (exactly mtu bytes) still arrives. */
    n = net_recv_dgram(lfd, buf, sizeof(buf) - 1);
    CHECK_EQ(n, 576);
    close(lfd);
}

#else /* !HPULOGC_SINK_UDP */

TEST(net_udp_not_compiled_noop)
{
    CHECK(1);
}

#endif /* HPULOGC_SINK_UDP */

#if defined(HPULOGC_SINK_TCP)

TEST(net_tcp_loopback_basic)
{
    char cfgpath[300];
    char text[512];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int lfd;
    int port;
    int cfd;
    int i;
    int rc;

    net_setup_base();
    lfd = net_bind_listener(SOCK_STREAM, 0, &port);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_tcp.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = tcp, host=127.0.0.1, port=%d\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             port);
    net_write_config("tcp.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 5; i++) {
        HPULOGC_INFO("t", "tcp hello %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();

    cfd = net_accept(lfd);
    CHECK(cfd >= 0);
    rc = net_read_lines(cfd, buf, sizeof(buf) - 1, 5);
    CHECK(rc > 0);
    CHECK(strstr(buf, "tcp hello 0") != NULL);
    CHECK(strstr(buf, "tcp hello 4") != NULL);
    close(cfd);
    close(lfd);
}

TEST(net_tcp_start_refused_fails_fast)
{
    char cfgpath[300];
    char text[512];

    net_setup_base();
    /* Fail-fast via an unresolvable host (start-time resolution, §4.10.8):
     * .invalid is guaranteed not to resolve. A connection-refused variant
     * is not portable (WSL2 localhost relay accepts, then resets). */
    snprintf(cfgpath, sizeof(cfgpath), "%s_tcpf.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = tcp, host=nosuchhost.invalid, port=1\n"
             "[rules]\n"
             "*.* = standard, t0\n");
    net_write_config("tcpf.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_IO);
    hpulogc_shutdown(); /* idempotent after failed init */
}

TEST(net_tcp_reconnect_backoff)
{
    char cfgpath[300];
    char text[512];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int lfd;
    int port;
    int cfd;
    int i;
    int rc;

    net_setup_base();
    lfd = net_bind_listener(SOCK_STREAM, 0, &port);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_tcpr.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = tcp, host=127.0.0.1, port=%d, reconnect backoff=1,"
             " reconnect backoff max=2\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             port);
    net_write_config("tcpr.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    /* Phase 1: healthy connection delivers. */
    for (i = 0; i < 3; i++) {
        HPULOGC_INFO("t", "reconn phase1 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    /* Kill the peer: close listener and the accepted connection. */
    cfd = net_accept(lfd);
    CHECK(cfd >= 0);
    close(cfd);
    close(lfd);

    /* Phase 2: sends hit the dead connection (RST arrives after the
     * first, kernel-buffered, send) -> at least one failed event. */
    for (i = 0; i < 4; i++) {
        HPULOGC_INFO("t", "reconn phase2 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    hpu_test_sleep_ms(150);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "reconn phase2 more %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK(st.failed >= 1);
    CHECK_EQ(st.written, 9); /* hand-off accounting: 3 + 4 + 2 */

    /* Phase 3: past the 1s backoff window, rebind the same port; the
     * next event drives the reconnect and delivery resumes. */
    hpu_test_sleep_ms(1300);
    /* Rebind the SAME port so the reconnect lands on our listener. */
    lfd = net_bind_listener(SOCK_STREAM, port, &port);
    CHECK(lfd >= 0);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "reconn phase3 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 11);
    hpulogc_shutdown();

    cfd = net_accept(lfd); /* the reconnect's connection */
    CHECK(cfd >= 0);
    rc = net_read_lines(cfd, buf, sizeof(buf) - 1, 2);
    CHECK(rc > 0);
    CHECK(strstr(buf, "reconn phase3 0") != NULL);
    CHECK(strstr(buf, "reconn phase3 1") != NULL);
    close(cfd);
    close(lfd);
}

TEST(net_tcp_async_queue_overflow_drops)
{
    char cfgpath[300];
    char text[512];
    hpulogc_sink_stats_t st;
    int lfd;
    int port;
    int i;

    net_setup_base();
    /* A bound-but-never-accepting receiver: the connection completes
     * (backlog) and the worker's blocking send stalls once the kernel
     * buffers fill -> the 64kb async queue overflows deterministically
     * under the burst below. */
    lfd = net_bind_listener(SOCK_STREAM, 0, &port);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_tcpq.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = tcp, host=127.0.0.1, port=%d, async=on, queue size=64kb\n"
             "[async]\n"
             "shutdown timeout = 1000\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             port);
    net_write_config("tcpq.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 8000; i++) {
        HPULOGC_INFO("t",
                     "overflow padding line to fill queue and socket "
                     "buffers quickly %010d %010d %010d",
                     i, i, i);
    }
    hpu_test_sleep_ms(400);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK(st.dropped > 0); /* constant-discard, D-S5 */

    /* Unblock the stalled worker before shutdown: accept the queued
     * connection and drain until quiet, so the bounded shutdown drain
     * does not resort to its timeout path. */
    {
        int cfd = net_accept(lfd);
        char tmp[65536];

        if (cfd >= 0) {
            while (net_wait_readable(cfd, 200) == 1) {
                if (recv(cfd, tmp, sizeof(tmp), 0) <= 0) {
                    break;
                }
            }
            close(cfd);
        }
    }
    hpulogc_shutdown();
    close(lfd);
}

#else /* !HPULOGC_SINK_TCP */

TEST(net_tcp_not_compiled_noop)
{
    CHECK(1);
}

#endif /* HPULOGC_SINK_TCP */

#endif /* _WIN32 / HPULOGC_ENABLE_INI */
