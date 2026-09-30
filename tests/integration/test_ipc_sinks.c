/* -*- coding: utf-8 -*- */

/**
 * @file test_ipc_sinks.c
 * @brief IPC sink integration (rd_v0.6 §4.10.9): UDS dgram/stream
 *        loopback, start fail-fast, peer-restart reconnect backoff,
 *        FIFO reader-late (ENXIO) retry, reader-close (EPIPE) reopen
 *        and FIFO-full (EAGAIN) discard accounting.
 *
 * POSIX-only (the unix/fifo sinks are POSIX-only): the test creates its
 * own endpoints in a temp directory — the socket file is bound by an
 * in-process listener, the FIFO by mkfifo — mirroring the external-peer
 * model the spec mandates for the library itself.
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/** @brief Overall receive timeout per wait step (ms). */
#define IPC_RECV_TIMEOUT_MS 3000

#if defined(_WIN32) || !defined(HPULOGC_ENABLE_INI)
TEST(ipc_sinks_unsupported_noop)
{
    /* unix/fifo sinks are POSIX-only; INI-less builds cannot define them. */
    CHECK(1);
}
#else

/* ---- endpoint helpers (test-local, POSIX) ---- */

/**
 * @brief Bind a Unix domain socket at @p path (listening for
 *        SOCK_STREAM). Unlinks any stale endpoint file first.
 * @return Bound socket, -1 on failure.
 */
static int ipc_bind_unix(int type, const char* path)
{
    struct sockaddr_un sa;
    int fd;

    unlink(path);
    fd = socket(AF_UNIX, type, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
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
static int ipc_wait_readable(int fd, int timeout_ms)
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
static int ipc_recv_dgram(int fd, void* buf, size_t buflen)
{
    int rc = ipc_wait_readable(fd, IPC_RECV_TIMEOUT_MS);

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
static int ipc_accept(int lfd)
{
    int rc = ipc_wait_readable(lfd, IPC_RECV_TIMEOUT_MS);

    if (rc <= 0) {
        return -1;
    }
    return accept(lfd, NULL, NULL);
}

/**
 * @brief Read from @p fd until @p want line terminators arrived (bounded).
 * @return Total bytes read, -1 on timeout/error.
 */
static int ipc_read_lines(int fd, char* buf, size_t buflen, int want)
{
    size_t len = 0;
    int lines = 0;

    while (lines < want) {
        ssize_t got;
        size_t i;

        if (ipc_wait_readable(fd, IPC_RECV_TIMEOUT_MS) <= 0) {
            return -1;
        }
        got = read(fd, buf + len, buflen - len);
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

/**
 * @brief Drain whatever is readable from @p fd (bounded, non-fatal).
 */
static void ipc_drain(int fd)
{
    char tmp[65536];

    while (ipc_wait_readable(fd, 200) == 1) {
        if (read(fd, tmp, sizeof(tmp)) <= 0) {
            break;
        }
    }
}

/**
 * @brief Open a FIFO read end, non-blocking (the test acts as the
 *        external reader process the spec hands the responsibility to).
 * @return Reader fd, -1 on failure.
 */
static int fifo_open_reader(const char* path)
{
    return open(path, O_RDONLY | O_NONBLOCK);
}

/* ---- config helpers ---- */

static char g_base[256];

static void ipc_setup_base(void)
{
    char tmpdir[128];

    if (g_base[0] == '\0') {
        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(g_base, sizeof(g_base), "%s/hpu_ipc_%d", tmpdir,
                 hpu_test_getpid());
    }
}

static void ipc_write_config(const char* name, const char* text)
{
    char path[300];
    FILE* fp;

    snprintf(path, sizeof(path), "%s_%s", g_base, name);
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs(text, fp);
    fclose(fp);
}

#if defined(HPULOGC_SINK_UNIX)

TEST(ipc_unix_dgram_loopback_basic)
{
    char cfgpath[300];
    char sockpath[300];
    char text[1024];
    char buf[2048];
    hpulogc_sink_stats_t st;
    int lfd;
    int i;
    int seen;

    ipc_setup_base();
    snprintf(sockpath, sizeof(sockpath), "%s_dgram.sock", g_base);
    lfd = ipc_bind_unix(SOCK_DGRAM, sockpath);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_unixd.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = u0\n"
             "[outputs]\n"
             "u0 = unix, path=%s, socktype=dgram\n"
             "[rules]\n"
             "*.* = standard, u0\n",
             sockpath);
    ipc_write_config("unixd.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 5; i++) {
        HPULOGC_INFO("t", "unix dgram hello %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("u0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    CHECK_EQ(st.failed, 0);
    CHECK_EQ(st.dropped, 0);
    hpulogc_shutdown();

    seen = 0;
    for (i = 0; i < 5; i++) {
        int n = ipc_recv_dgram(lfd, buf, sizeof(buf) - 1);

        if (n <= 0) {
            break;
        }
        buf[n] = '\0';
        CHECK(strstr(buf, "unix dgram hello") != NULL);
        seen++;
    }
    CHECK_EQ(seen, 5);
    close(lfd);
    unlink(sockpath);
}

TEST(ipc_unix_stream_loopback_basic)
{
    char cfgpath[300];
    char sockpath[300];
    char text[1024];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int lfd;
    int cfd;
    int i;
    int rc;

    ipc_setup_base();
    snprintf(sockpath, sizeof(sockpath), "%s_stream.sock", g_base);
    lfd = ipc_bind_unix(SOCK_STREAM, sockpath);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_unixs.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = unix, path=%s, socktype=stream\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             sockpath);
    ipc_write_config("unixs.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 5; i++) {
        HPULOGC_INFO("t", "unix stream hello %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();

    cfd = ipc_accept(lfd);
    CHECK(cfd >= 0);
    rc = ipc_read_lines(cfd, buf, sizeof(buf) - 1, 5);
    CHECK(rc > 0);
    CHECK(strstr(buf, "unix stream hello 0") != NULL);
    CHECK(strstr(buf, "unix stream hello 4") != NULL);
    close(cfd);
    close(lfd);
    unlink(sockpath);
}

TEST(ipc_unix_start_enoent_fails_fast)
{
    char cfgpath[300];
    char sockpath[300];
    char text[1024];

    ipc_setup_base();
    /* No listener ever binds this path: connect fails with ENOENT at
     * start -> HPULOGC_ERR_IO fail-fast (§4.10.9). */
    snprintf(sockpath, sizeof(sockpath), "%s_absent.sock", g_base);
    snprintf(cfgpath, sizeof(cfgpath), "%s_unixf.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = unix, path=%s\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             sockpath);
    ipc_write_config("unixf.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_IO);
    hpulogc_shutdown(); /* idempotent after failed init */
}

TEST(ipc_unix_path_too_long_fails_fast)
{
    char cfgpath[300];
    char sockpath[300];
    char text[1024];
    size_t fill;

    ipc_setup_base();
    /* Longer than any platform sun_path (104/108) but short enough to
     * pass the sink's 128-byte configuration cap: the contract must
     * reject it with ENAMETOOLONG -> HPULOGC_ERR_IO at start. */
    snprintf(sockpath, sizeof(sockpath), "%s_", g_base);
    fill = strlen(sockpath);
    while (fill < 120) {
        sockpath[fill++] = 'p';
    }
    sockpath[fill] = '\0';
    snprintf(cfgpath, sizeof(cfgpath), "%s_unixl.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = unix, path=%s\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             sockpath);
    ipc_write_config("unixl.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_IO);
    hpulogc_shutdown();
}

TEST(ipc_unix_stream_reconnect_backoff)
{
    char cfgpath[300];
    char sockpath[300];
    char text[1024];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int lfd;
    int cfd;
    int i;
    int rc;

    ipc_setup_base();
    snprintf(sockpath, sizeof(sockpath), "%s_reconn.sock", g_base);
    lfd = ipc_bind_unix(SOCK_STREAM, sockpath);
    CHECK(lfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_unixr.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = t0\n"
             "[outputs]\n"
             "t0 = unix, path=%s, socktype=stream, reconnect backoff=1,"
             " reconnect backoff max=2\n"
             "[rules]\n"
             "*.* = standard, t0\n",
             sockpath);
    ipc_write_config("unixr.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    /* Phase 1: healthy connection delivers. */
    for (i = 0; i < 3; i++) {
        HPULOGC_INFO("t", "ureconn phase1 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    /* Kill the peer: close listener and the accepted connection, and
     * remove the endpoint file (a restarted peer rebinds it later). */
    cfd = ipc_accept(lfd);
    CHECK(cfd >= 0);
    close(cfd);
    close(lfd);
    unlink(sockpath);

    /* Phase 2: sends hit the dead connection (the first may land in the
     * kernel buffer, the next gets EPIPE) -> at least one failed event;
     * reconnect attempts hit ENOENT until the endpoint returns. */
    for (i = 0; i < 4; i++) {
        HPULOGC_INFO("t", "ureconn phase2 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    hpu_test_sleep_ms(150);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "ureconn phase2 more %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK(st.failed >= 1);
    CHECK_EQ(st.written, 9); /* hand-off accounting: 3 + 4 + 2 */

    /* Phase 3: past the 1s backoff window, rebind the same path; the
     * next event drives the reconnect and delivery resumes. */
    hpu_test_sleep_ms(1300);
    lfd = ipc_bind_unix(SOCK_STREAM, sockpath);
    CHECK(lfd >= 0);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "ureconn phase3 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("t0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 11);
    hpulogc_shutdown();

    cfd = ipc_accept(lfd); /* the reconnect's connection */
    CHECK(cfd >= 0);
    rc = ipc_read_lines(cfd, buf, sizeof(buf) - 1, 2);
    CHECK(rc > 0);
    CHECK(strstr(buf, "ureconn phase3 0") != NULL);
    CHECK(strstr(buf, "ureconn phase3 1") != NULL);
    close(cfd);
    close(lfd);
    unlink(sockpath);
}

#else /* !HPULOGC_SINK_UNIX */

TEST(ipc_unix_not_compiled_noop)
{
    CHECK(1);
}

#endif /* HPULOGC_SINK_UNIX */

#if defined(HPULOGC_SINK_FIFO)

TEST(ipc_fifo_basic)
{
    char cfgpath[300];
    char fifopath[300];
    char text[1024];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int rfd;
    int i;
    int rc;

    ipc_setup_base();
    snprintf(fifopath, sizeof(fifopath), "%s_basic.pipe", g_base);
    CHECK_EQ(mkfifo(fifopath, 0600), 0);

    /* The external reader is up before the sink starts. */
    rfd = fifo_open_reader(fifopath);
    CHECK(rfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_fifob.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = f0\n"
             "[outputs]\n"
             "f0 = fifo, path=%s\n"
             "[rules]\n"
             "*.* = standard, f0\n",
             fifopath);
    ipc_write_config("fifob.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 5; i++) {
        HPULOGC_INFO("t", "fifo hello %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();

    rc = ipc_read_lines(rfd, buf, sizeof(buf) - 1, 5);
    CHECK(rc > 0);
    CHECK(strstr(buf, "fifo hello 0") != NULL);
    CHECK(strstr(buf, "fifo hello 4") != NULL);
    close(rfd);
    unlink(fifopath);
}

TEST(ipc_fifo_reader_late_enxio_retry)
{
    char cfgpath[300];
    char fifopath[300];
    char text[1024];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int rfd;
    int i;
    int rc;

    ipc_setup_base();
    snprintf(fifopath, sizeof(fifopath), "%s_late.pipe", g_base);
    CHECK_EQ(mkfifo(fifopath, 0600), 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_fifol.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = f0\n"
             "[outputs]\n"
             "f0 = fifo, path=%s, reconnect backoff=1,"
             " reconnect backoff max=2\n"
             "[rules]\n"
             "*.* = standard, f0\n",
             fifopath);
    ipc_write_config("fifol.ini", text);

    /* No reader yet: start must NOT fail (ENXIO -> waiting state,
     * §4.10.9 / D-R20), but the waiting events count failed. */
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    for (i = 0; i < 3; i++) {
        HPULOGC_INFO("t", "late phase1 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 3); /* hand-off accounting is unconditional */
    CHECK(st.failed == 3);

    /* The reader shows up; past the 1s backoff window the next event
     * reopens the write end and delivery resumes. */
    rfd = fifo_open_reader(fifopath);
    CHECK(rfd >= 0);
    hpu_test_sleep_ms(1300);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "late phase2 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 5);
    hpulogc_shutdown();

    /* Nothing from the waiting phase reached the reader. */
    rc = ipc_read_lines(rfd, buf, sizeof(buf) - 1, 2);
    CHECK(rc > 0);
    CHECK(strstr(buf, "late phase2 0") != NULL);
    CHECK(strstr(buf, "late phase2 1") != NULL);
    CHECK(strstr(buf, "phase1") == NULL);
    close(rfd);
    unlink(fifopath);
}

TEST(ipc_fifo_reader_close_epipe_reopen)
{
    char cfgpath[300];
    char fifopath[300];
    char text[1024];
    char buf[4096];
    hpulogc_sink_stats_t st;
    int rfd;
    int i;
    int rc;

    ipc_setup_base();
    snprintf(fifopath, sizeof(fifopath), "%s_epipe.pipe", g_base);
    CHECK_EQ(mkfifo(fifopath, 0600), 0);
    rfd = fifo_open_reader(fifopath);
    CHECK(rfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_fifoe.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = f0\n"
             "[outputs]\n"
             "f0 = fifo, path=%s, reconnect backoff=1,"
             " reconnect backoff max=2\n"
             "[rules]\n"
             "*.* = standard, f0\n",
             fifopath);
    ipc_write_config("fifoe.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    /* Phase 1: healthy delivery. */
    HPULOGC_INFO("t", "epipe phase1 %d", 0);
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);

    /* The reader process "dies": writes now hit EPIPE. Reaching this
     * point at all proves the library suppressed SIGPIPE (an unsuppressed
     * default disposition would have killed the test process). */
    close(rfd);
    hpu_test_sleep_ms(100); /* let the close fully propagate */
    for (i = 0; i < 3; i++) {
        HPULOGC_INFO("t", "epipe phase2 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK(st.failed >= 1);
    CHECK_EQ(st.written, 4); /* hand-off accounting: 1 + 3 */

    /* Reader restarts; past the backoff window the write end reopens. */
    rfd = fifo_open_reader(fifopath);
    CHECK(rfd >= 0);
    hpu_test_sleep_ms(1300);
    for (i = 0; i < 2; i++) {
        HPULOGC_INFO("t", "epipe phase3 %d", i);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 6);
    hpulogc_shutdown();

    rc = ipc_read_lines(rfd, buf, sizeof(buf) - 1, 2);
    CHECK(rc > 0);
    /* Only phase3 reached the (restarted) reader: phase1's line went
     * with the closed reader, phase2's events all failed on EPIPE. */
    CHECK(strstr(buf, "epipe phase3 0") != NULL);
    CHECK(strstr(buf, "epipe phase3 1") != NULL);
    CHECK(strstr(buf, "phase2") == NULL);
    CHECK(strstr(buf, "phase1") == NULL);
    close(rfd);
    unlink(fifopath);
}

TEST(ipc_fifo_full_eagain_discard)
{
    char cfgpath[300];
    char fifopath[300];
    char text[1024];
    char bigmsg[1024];
    hpulogc_sink_stats_t st;
    int rfd;
    int i;

    ipc_setup_base();
    snprintf(fifopath, sizeof(fifopath), "%s_full.pipe", g_base);
    CHECK_EQ(mkfifo(fifopath, 0600), 0);
    /* Reader holds the pipe open but never reads: the pipe fills up and
     * writes surface EAGAIN (constant O_NONBLOCK, §4.10.9). */
    rfd = fifo_open_reader(fifopath);
    CHECK(rfd >= 0);

    snprintf(cfgpath, sizeof(cfgpath), "%s_fifof.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = f0\n"
             "[outputs]\n"
             "f0 = fifo, path=%s\n"
             "[rules]\n"
             "*.* = standard, f0\n",
             fifopath);
    ipc_write_config("fifof.ini", text);
    memset(bigmsg, 'F', sizeof(bigmsg) - 1);
    bigmsg[sizeof(bigmsg) - 1] = '\0';

    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);
    /* 500 records x ~1KB ~= 520KB: under the default 1MB ring buffer
     * (no global overflow) yet far above the 64KB default pipe
     * capacity — reaching the assertions below at all proves emit
     * never blocked. */
    for (i = 0; i < 500; i++) {
        HPULOGC_INFO("t", "full %010d %s", i, bigmsg);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(hpulogc_get_sink_stats("f0", &st), HPULOGC_OK);
    CHECK_EQ(st.written, 500); /* hand-off accounting is unconditional */
    CHECK(st.failed > 0);      /* EAGAIN discards count failed */
    hpulogc_shutdown();

    ipc_drain(rfd);
    close(rfd);
    unlink(fifopath);
}

TEST(ipc_fifo_start_non_fifo_fails_fast)
{
    char cfgpath[300];
    char filepath[300];
    char text[1024];
    FILE* fp;

    ipc_setup_base();
    /* The endpoint exists but is a regular file: fail-fast (§4.10.9). */
    snprintf(filepath, sizeof(filepath), "%s_notfifo.txt", g_base);
    fp = fopen(filepath, "w");
    CHECK(fp != NULL);
    fputs("not a fifo\n", fp);
    fclose(fp);

    snprintf(cfgpath, sizeof(cfgpath), "%s_fifon.ini", g_base);
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = f0\n"
             "[outputs]\n"
             "f0 = fifo, path=%s\n"
             "[rules]\n"
             "*.* = standard, f0\n",
             filepath);
    ipc_write_config("fifon.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_IO);
    hpulogc_shutdown();
    unlink(filepath);
}

#else /* !HPULOGC_SINK_FIFO */

TEST(ipc_fifo_not_compiled_noop)
{
    CHECK(1);
}

#endif /* HPULOGC_SINK_FIFO */

#endif /* _WIN32 / HPULOGC_ENABLE_INI */
