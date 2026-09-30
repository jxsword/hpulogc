/* -*- coding: utf-8 -*- */

/**
 * @file test_http_sink.c
 * @brief HTTP/Webhook sink integration (rd_v0.6 §4.10.10): an in-process
 *        miniature HTTP server (thread + accept loop) asserts request
 *        shape (method/path/Content-Type/headers/NDJSON body), batch
 *        splitting, at-most-once failure accounting (500 / timeout, no
 *        retry), keep-alive reuse, close-triggered reconnect and the
 *        config-level fail-fast paths (start refused, async=off, bad
 *        scheme).
 *
 * POSIX-only (the http sink is POSIX-only in v1); the HTTP server and
 * socket helpers live in this file (the shared portability.h has no
 * network support), following test_net_sinks.c.
 */

#include "test_util.h"
#include "portability.h"
#include "hpulogc.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/** @brief Overall receive timeout per server read step (ms). */
#define HTTP_RECV_TIMEOUT_MS 5000

/** @brief Bound on waiting for sink stats to settle (ms). */
#define HTTP_STATS_WAIT_MS 10000

/** @brief Recorded requests kept per server instance. */
#define HTTP_MAX_REQS 64

#if defined(_WIN32) || !defined(HPULOGC_ENABLE_INI)
TEST(http_sink_unsupported_noop)
{
    /* The http sink is POSIX-only; INI-less builds cannot define it. */
    CHECK(1);
}
#else

/* ---- miniature HTTP server (test-local, POSIX) ---- */

/**
 * @brief One recorded request: header block plus body length and head.
 */
typedef struct http_req_rec {
    char   hdr[1024];  /*!< Header block, NUL-terminated */
    size_t body_len;   /*!< Full body length per Content-Length */
    char   body[1024]; /*!< First bytes of the body, NUL-terminated */
} http_req_rec_t;

/**
 * @brief Programmable miniature HTTP server (single connection at a
 *        time; the sink uses exactly one keep-alive connection).
 */
typedef struct http_server {
    int  lfd;              /*!< Listening socket */
    int  port;             /*!< Bound port */
    int  resp_code;        /*!< Status code to respond with */
    int  send_close;       /*!< Add "Connection: close" to responses */
    int  silent;           /*!< Read the request but never respond */
    int  close_after_first;/*!< Close the connection (without a close
                                header) after the first response */
    /* recorded state (written by the server thread only) */
    http_req_rec_t reqs[HTTP_MAX_REQS];
    int  req_count;        /*!< Requests fully read (atomic: the test
                                thread asserts it while running) */
    int  accept_count;     /*!< Connections accepted (atomic, as above) */
    /* thread plumbing */
    hpu_test_thread_t th;
    int stop;              /*!< Stop flag, accessed atomically */
} http_server_t;

/** @brief Atomic accessors for the cross-thread server fields. */
static int http_srv_load(const int* v)
{
    return __atomic_load_n(v, __ATOMIC_SEQ_CST);
}

static void http_srv_store(int* v, int x)
{
    __atomic_store_n(v, x, __ATOMIC_SEQ_CST);
}

static int http_srv_inc(int* v)
{
    return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST);
}

static http_server_t g_srv;

/**
 * @brief Bind a loopback listener (ephemeral port).
 * @return Bound listening socket, -1 on failure.
 */
static int http_bind_listener(int* out_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0 ||
        listen(fd, 4) != 0 ||
        getsockname(fd, (struct sockaddr*)&sa, &len) != 0) {
        close(fd);
        return -1;
    }
    *out_port = ntohs(sa.sin_port);
    return fd;
}

/**
 * @brief Wait until @p fd is readable (poll with timeout).
 * @return 1 readable, 0 timeout, -1 error.
 */
static int http_wait_readable(int fd, int timeout_ms)
{
    struct pollfd pfd;

    pfd.fd = fd;
    pfd.events = POLLIN;
    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);

        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return rc;
    }
}

/**
 * @brief Recv helper: retries EINTR, returns -1 on timeout/error.
 * @return bytes received (>0), 0 orderly close, -1 timeout or error.
 */
static int http_srv_recv(int fd, void* buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return -1;
        }
        return (int)n;
    }
}

/**
 * @brief Find the first occurrence of a needle in a buffer (memmem for
 *        portability: the test avoids GNU memmem).
 */
static const char* http_memfind(const char* hay, size_t hay_len,
                                const char* needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if (nlen == 0 || hay_len < nlen) {
        return NULL;
    }
    for (i = 0; i + nlen <= hay_len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/**
 * @brief Read one request (headers + Content-Length body) from @p cfd.
 * @return 0 on success, -1 when the peer closed or timed out.
 */
static int http_read_request(int cfd, http_req_rec_t* rec)
{
    char buf[4096];
    size_t len = 0;
    const char* hdr_end;
    long cl = 0;
    const char* clp;
    char tmp[8192];
    size_t have;

    for (;;) {
        int n;

        if (len >= sizeof(buf)) {
            return -1;
        }
        if (http_wait_readable(cfd, HTTP_RECV_TIMEOUT_MS) != 1) {
            return -1;
        }
        n = http_srv_recv(cfd, buf + len, sizeof(buf) - len);
        if (n <= 0) {
            return -1;
        }
        len += (size_t)n;
        hdr_end = http_memfind(buf, len, "\r\n\r\n");
        if (hdr_end != NULL) {
            break;
        }
    }
    /* record the header block (including the terminating CRLFCRLF) */
    {
        size_t hlen = (size_t)(hdr_end - buf) + 4;

        if (hlen >= sizeof(rec->hdr)) {
            hlen = sizeof(rec->hdr) - 1;
        }
        memcpy(rec->hdr, buf, hlen);
        rec->hdr[hlen] = '\0';
    }
    /* Content-Length */
    clp = strstr(rec->hdr, "Content-Length: ");
    if (clp != NULL) {
        cl = strtol(clp + 16, NULL, 10);
        if (cl < 0) {
            cl = 0;
        }
    }
    rec->body_len = (size_t)cl;

    /* copy the body bytes already buffered, then read the rest. The
     * consumed count and the captured (recorded) count are tracked
     * separately: rec->body only holds the first KiB, but every
     * buffered body byte must be consumed from the stream. */
    {
        size_t buffered = len - (size_t)(hdr_end - buf) - 4;
        size_t got = buffered < (size_t)cl ? buffered : (size_t)cl;
        size_t cap = got < sizeof(rec->body) - 1 ? got
                                                 : sizeof(rec->body) - 1;

        memcpy(rec->body, hdr_end + 4, cap);
        rec->body[cap] = '\0';
        have = (size_t)cl - got; /* remaining bytes */
        while (have > 0) {
            int n;

            if (http_wait_readable(cfd, HTTP_RECV_TIMEOUT_MS) != 1) {
                return -1;
            }
            n = http_srv_recv(cfd, tmp,
                              have < sizeof(tmp) ? have : sizeof(tmp));
            if (n <= 0) {
                return -1;
            }
            if (got < sizeof(rec->body) - 1) {
                size_t space = sizeof(rec->body) - 1 - got;
                size_t cpy = (size_t)n < space ? (size_t)n : space;

                memcpy(rec->body + got, tmp, cpy);
                got += cpy;
                rec->body[got] = '\0';
            }
            have -= (size_t)n;
        }
    }
    return 0;
}

/**
 * @brief Respond with the programmed status; small "ok" body unless
 *        silent (never responds).
 */
static void http_respond(int cfd, const http_server_t* s)
{
    char resp[256];
    int n;

    if (s->silent) {
        return;
    }
    n = snprintf(resp, sizeof(resp),
                 "HTTP/1.1 %d %s\r\n"
                 "Content-Length: 2\r\n"
                 "%s"
                 "\r\n"
                 "ok",
                 s->resp_code,
                 s->resp_code >= 500 ? "Server Error" : "OK",
                 s->send_close ? "Connection: close\r\n" : "");
    if (n > 0) {
        size_t off = 0;

        while (off < (size_t)n) {
            ssize_t w = send(cfd, resp + off, (size_t)n - off, 0);

            if (w <= 0) {
                break;
            }
            off += (size_t)w;
        }
    }
}

/**
 * @brief Server thread main: accept loop, one connection served at a
 *        time (keep-alive request loop), recorded per request.
 */
static void* http_server_main(void* arg)
{
    http_server_t* s = (http_server_t*)arg;

    while (!http_srv_load(&s->stop)) {
        int cfd;
        int closed_by_flag = 0;

        cfd = accept(s->lfd, NULL, NULL);
        if (cfd < 0) {
            if (http_srv_load(&s->stop)) {
                break;
            }
            continue;
        }
        http_srv_inc(&s->accept_count);
        for (;;) {
            http_req_rec_t* rec;
            int req_no = http_srv_load(&s->req_count);

            if (req_no >= HTTP_MAX_REQS) {
                break;
            }
            rec = &s->reqs[req_no];
            if (http_read_request(cfd, rec) != 0) {
                break; /* peer closed / timed out: connection over */
            }
            req_no = http_srv_inc(&s->req_count);
            http_respond(cfd, s);
            if (s->close_after_first && req_no == 1) {
                closed_by_flag = 1; /* no close header: stale keep-alive */
            }
            if (s->send_close || closed_by_flag) {
                break;
            }
        }
        close(cfd);
    }
    return NULL;
}

/**
 * @brief Start the server (bind + listen + spawn thread).
 */
static void http_server_init(http_server_t* s)
{
    memset(s, 0, sizeof(*s));
    s->resp_code = 200;
}

/**
 * @brief Start the server (bind + listen + spawn thread).
 */
static void http_server_start(http_server_t* s)
{
    s->lfd = http_bind_listener(&s->port);
    CHECK(s->lfd >= 0);
    CHECK_EQ(hpu_test_thread_create(&s->th, http_server_main, s), 0);
}

/**
 * @brief Stop the server (wakes accept via shutdown + dummy connect).
 */
static void http_server_stop(http_server_t* s)
{
    http_srv_store(&s->stop, 1);
    shutdown(s->lfd, SHUT_RDWR);
    close(s->lfd);
    hpu_test_thread_join(&s->th);
}

/* ---- config + stats helpers ---- */

static char g_base[256];

static void http_setup_base(void)
{
    char tmpdir[128];

    if (g_base[0] == '\0') {
        hpu_test_tmpdir(tmpdir, sizeof(tmpdir));
        snprintf(g_base, sizeof(g_base), "%s/hpu_http_%d", tmpdir,
                 hpu_test_getpid());
    }
}

static void http_write_config(const char* name, const char* text)
{
    char path[300];
    FILE* fp;

    snprintf(path, sizeof(path), "%s_%s", g_base, name);
    fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs(text, fp);
    fclose(fp);
}

/**
 * @brief Build a one-sink http config.
 *
 * Header-only extras (e.g. an auth header) are appended verbatim.
 */
static void http_make_config(char* text, size_t cap, int port,
                             const char* extras, const char* mode,
                             int timeout_ms)
{
    snprintf(text, cap,
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://127.0.0.1:%d/logs, async=on, "
             "queue size=1mb, batch mode=%s, timeout ms=%d, "
             "reconnect backoff=1, reconnect backoff max=5%s\n"
             "[rules]\n"
             "*.* = standard, w0\n",
             port, mode, timeout_ms, extras != NULL ? extras : "");
}

/**
 * @brief Poll per-sink stats until @p min_written / @p min_failed are
 *        reached and then stay stable for ~300 ms.
 *
 * Counting semantics (§4.10.3, hand-off): async instances count
 * `written` when the event is pushed into the second-level queue, and
 * `failed` when the worker's POST ultimately fails — the same event can
 * appear in both. The stability window ensures the worker has finished
 * accounting before the assertions run.
 *
 * @return 1 when reached within the bound, 0 otherwise (stats in @p st).
 */
static int http_wait_settled(const char* name, unsigned long long min_written,
                             unsigned long long min_failed,
                             hpulogc_sink_stats_t* st)
{
    hpulogc_sink_stats_t prev;
    int stable = 0;
    int waited = 0;

    memset(&prev, 0, sizeof(prev));
    for (;;) {
        if (hpulogc_get_sink_stats(name, st) == HPULOGC_OK &&
            st->written >= min_written && st->failed >= min_failed) {
            if (prev.written == st->written && prev.failed == st->failed &&
                prev.dropped == st->dropped) {
                stable++;
                if (stable >= 15) {
                    return 1;
                }
            } else {
                stable = 0;
            }
            prev = *st;
        }
        if (waited >= HTTP_STATS_WAIT_MS) {
            return 0;
        }
        hpu_test_sleep_ms(20);
        waited += 20;
    }
}

#if defined(HPULOGC_SINK_HTTP)

/**
 * @brief The union of recorded bodies contains @p needle.
 *
 * The async worker splits one batch into several POSTs at its own pace
 * (rd_v0.6 §4.10.10 allows a batch to split), so content assertions
 * must scan all recorded requests, not just the first.
 */
static int http_bodies_contain(const char* needle)
{
    int i;
    int n = http_srv_load(&g_srv.req_count);

    for (i = 0; i < n; i++) {
        if (strstr(g_srv.reqs[i].body, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Every recorded request carries the expected request line, the
 *        programmed Content-Type, and a Content-Length matching the
 *        body actually received.
 */
static void http_check_common_shape(const char* ct)
{
    int i;
    int n = http_srv_load(&g_srv.req_count);

    CHECK(n >= 1);
    for (i = 0; i < n; i++) {
        const http_req_rec_t* r = &g_srv.reqs[i];
        const char* clp = strstr(r->hdr, "Content-Length: ");
        long cl = 0;

        CHECK(strstr(r->hdr, "POST /logs HTTP/1.1\r\n") == r->hdr);
        CHECK(strstr(r->hdr, "Host: 127.0.0.1") != NULL);
        CHECK(strstr(r->hdr, ct) != NULL);
        if (clp != NULL) {
            cl = strtol(clp + 16, NULL, 10);
        }
        CHECK_EQ(cl, (long)r->body_len);
    }
}

TEST(http_ndjson_basic)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_basic.ini", g_base);
    http_write_config("basic.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "http hello 0");
    HPULOGC_INFO("t", "http hello 1");
    HPULOGC_INFO("t", "http, \"quoted\" \\backslash\\");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 3, 0, &st), 1);
    CHECK_EQ(st.written, 3);
    CHECK_EQ(st.failed, 0);
    CHECK_EQ(st.dropped, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* request shape: method/path/version, host, content type; the
     * worker may split the batch, so shape/content scan all requests */
    http_check_common_shape("Content-Type: application/x-ndjson\r\n");
    CHECK(http_srv_load(&g_srv.req_count) <= 3);
    /* ndjson bodies: one JSON array, one escaped element per event */
    {
        int dbg_i;
        int dbg_n = http_srv_load(&g_srv.req_count);

        for (dbg_i = 0; dbg_i < dbg_n; dbg_i++) {
            fprintf(stderr, "DBG body[%d]=[%s]\n", dbg_i,
                    g_srv.reqs[dbg_i].body);
        }
    }
    CHECK(http_bodies_contain("http hello 0"));
    CHECK(http_bodies_contain("http hello 1"));
    CHECK(http_bodies_contain("\\\"quoted\\\""));
    CHECK(http_bodies_contain("\\\\backslash\\\\"));
}

TEST(http_lines_mode)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "lines", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_lines.ini", g_base);
    http_write_config("lines.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "lines hello 0");
    HPULOGC_INFO("t", "lines hello 1");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 2, 0, &st), 1);
    CHECK_EQ(st.written, 2);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    http_check_common_shape("Content-Type: text/plain\r\n");
    CHECK(http_srv_load(&g_srv.req_count) <= 2);
    CHECK(http_bodies_contain("lines hello 0\n"));
    CHECK(http_bodies_contain("lines hello 1\n"));
}

TEST(http_custom_headers)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port,
                     ", header=\"X-Auth-Token: abc123\", "
                     "header=X-Route:logs",
                     "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_hdr.ini", g_base);
    http_write_config("hdr.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "auth hello");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 1, 0, &st), 1);
    CHECK_EQ(st.written, 1);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* the configured headers must ride on every POST */
    {
        int i;
        int n = http_srv_load(&g_srv.req_count);

        CHECK(n >= 1);
        for (i = 0; i < n; i++) {
            CHECK(strstr(g_srv.reqs[i].hdr,
                         "X-Auth-Token: abc123\r\n") != NULL);
            CHECK(strstr(g_srv.reqs[i].hdr, "X-Route: logs\r\n") != NULL);
        }
    }
}

TEST(http_batch_split)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;
    char big[512];
    int i;
    int total_body = 0;

    http_setup_base();
    http_server_init(&g_srv);
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "lines", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_split.ini", g_base);
    http_write_config("split.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    for (i = 0; i < 600; i++) {
        HPULOGC_INFO("t", "%04d %s", i, big);
    }
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 600, 0, &st), 1);
    CHECK_EQ(st.written, 600);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* ~511-byte lines: the 64 KiB body buffer must split the batch */
    CHECK(http_srv_load(&g_srv.req_count) >= 2);
    for (i = 0; i < http_srv_load(&g_srv.req_count); i++) {
        CHECK(g_srv.reqs[i].body_len <= 65536);
        total_body += (int)g_srv.reqs[i].body_len;
    }
    CHECK(total_body >= 600 * 516); /* every event left the process */
}

TEST(http_500_no_retry)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;
    int served;

    http_setup_base();
    http_server_init(&g_srv);
    g_srv.resp_code = 500;
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_500.ini", g_base);
    http_write_config("500.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "will fail 0");
    HPULOGC_INFO("t", "will fail 1");
    HPULOGC_INFO("t", "will fail 2");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 3, 3, &st), 1);
    /* hand-off accounting (§4.10.3): written counted at queue push,
     * the POST failures surface via failed */
    CHECK_EQ(st.written, 3);
    CHECK_EQ(st.failed, 3);
    /* at-most-once: well past the 1 s backoff, no further POST (the
     * batch may have split into several, but the count is frozen) */
    hpu_test_sleep_ms(1300);
    served = http_srv_load(&g_srv.req_count);
    CHECK(served >= 1 && served <= 3);
    CHECK_EQ(http_srv_load(&g_srv.req_count), served);
    CHECK_EQ(st.failed, 3);
    hpulogc_shutdown();
    http_server_stop(&g_srv);
}

TEST(http_timeout_no_response)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    g_srv.silent = 1;
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 200);
    snprintf(cfgpath, sizeof(cfgpath), "%s_timeout.ini", g_base);
    http_write_config("timeout.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "no reply 0");
    HPULOGC_INFO("t", "no reply 1");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 2, 2, &st), 1);
    CHECK_EQ(st.written, 2); /* hand-off at queue push */
    CHECK_EQ(st.failed, 2);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* the request did reach the server (sent, response never came);
     * the batch may have split, but the server read every POST */
    CHECK(http_srv_load(&g_srv.req_count) >= 1);
    CHECK(http_srv_load(&g_srv.req_count) <= 2);
}

TEST(http_keepalive_reuse)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_keepalive.ini", g_base);
    http_write_config("keepalive.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "ka 0");
    HPULOGC_INFO("t", "ka 1");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 2, 0, &st), 1);
    HPULOGC_INFO("t", "ka 2");
    HPULOGC_INFO("t", "ka 3");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 4, 0, &st), 1);
    CHECK_EQ(st.written, 4);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* both batches travelled one connection (keep-alive reuse; the
     * server responds with a Content-Length body, so reuse requires
     * the bounded body drain). Each batch may split into several
     * POSTs, but never a second connection. */
    CHECK_EQ(http_srv_load(&g_srv.accept_count), 1);
    CHECK(http_srv_load(&g_srv.req_count) >= 2);
    CHECK(http_srv_load(&g_srv.req_count) <= 4);
}

TEST(http_conn_close_reconnect)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    g_srv.send_close = 1;
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_close.ini", g_base);
    http_write_config("close.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "cc 0");
    HPULOGC_INFO("t", "cc 1");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 2, 0, &st), 1);
    CHECK_EQ(st.written, 2);
    HPULOGC_INFO("t", "cc 2");
    HPULOGC_INFO("t", "cc 3");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 4, 0, &st), 1);
    CHECK_EQ(st.written, 4);
    CHECK_EQ(st.failed, 0);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* Connection: close forces a fresh connection per batch; each
     * batch may split into several POSTs, each getting its own
     * connection (every response carries Connection: close) */
    CHECK(http_srv_load(&g_srv.accept_count) >= 2);
    CHECK(http_srv_load(&g_srv.accept_count) <= 4);
    CHECK(http_srv_load(&g_srv.req_count) >= 2);
    CHECK(http_srv_load(&g_srv.req_count) <= 4);
}

TEST(http_stale_keepalive)
{
    char cfgpath[300];
    char text[1024];
    hpulogc_sink_stats_t st;

    http_setup_base();
    http_server_init(&g_srv);
    g_srv.close_after_first = 1;
    http_server_start(&g_srv);

    http_make_config(text, sizeof(text), g_srv.port, "", "ndjson", 1000);
    snprintf(cfgpath, sizeof(cfgpath), "%s_stale.ini", g_base);
    http_write_config("stale.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_OK);

    HPULOGC_INFO("t", "stale 0");
    HPULOGC_INFO("t", "stale 1");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 2, 0, &st), 1);
    CHECK_EQ(st.written, 2);
    CHECK_EQ(st.failed, 0);
    /* the server dropped the keep-alive connection behind our back:
     * the next batch must fail once (at-most-once, no silent resend) */
    HPULOGC_INFO("t", "stale 2");
    HPULOGC_INFO("t", "stale 3");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 4, 2, &st), 1);
    CHECK_EQ(st.written, 4); /* hand-off at queue push */
    CHECK_EQ(st.failed, 2);
    /* after the 1 s backoff the instance reconnects and recovers */
    hpu_test_sleep_ms(1300);
    HPULOGC_INFO("t", "stale 4");
    HPULOGC_INFO("t", "stale 5");
    CHECK_EQ(hpulogc_flush(), HPULOGC_OK);
    CHECK_EQ(http_wait_settled("w0", 6, 2, &st), 1);
    CHECK_EQ(st.written, 6); /* hand-off at queue push */
    CHECK_EQ(st.failed, 2);
    hpulogc_shutdown();
    http_server_stop(&g_srv);

    /* conn1 is closed by the server right after the first response, so
     * the second batch's POST never reaches the server (it dies with
     * the RST) — the server sees req#1 (phase 1) plus the reconnected
     * phase-3 POST(s) on conn2. */
    CHECK_EQ(http_srv_load(&g_srv.accept_count), 2);
    CHECK(http_srv_load(&g_srv.req_count) >= 2);
    CHECK(http_srv_load(&g_srv.req_count) <= 4);
}

TEST(http_start_resolve_fails_fast)
{
    char cfgpath[300];
    char text[1024];

    http_setup_base();
    /* start() fail-fast via an unresolvable host (same rationale as
     * net_tcp_start_refused_fails_fast: a closed loopback port is not
     * reliably refused under the WSL2 localhost relay) */
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://nosuchhost.invalid:80/logs, async=on\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_refused.ini", g_base);
    http_write_config("refused.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_IO);
    hpulogc_shutdown(); /* idempotent after failed init */
}

TEST(http_async_off_rejected)
{
    char cfgpath[300];
    char text[1024];

    http_setup_base();
    /* D-R22: async=on is mandatory — the default (off) is rejected */
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://127.0.0.1:1/logs\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_syncoff.ini", g_base);
    http_write_config("syncoff.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);

    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://127.0.0.1:1/logs, async=off\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_syncoff2.ini", g_base);
    http_write_config("syncoff2.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);
    hpulogc_shutdown();
}

TEST(http_bad_url_rejected)
{
    char cfgpath[300];
    char text[1024];

    http_setup_base();
    /* https:// is rejected (D-R21: zero-dependency, plaintext only) */
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=https://example.com/logs, async=on\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_https.ini", g_base);
    http_write_config("https.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);

    /* malformed port */
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://127.0.0.1:notaport/logs, async=on\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_badport.ini", g_base);
    http_write_config("badport.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);

    /* unknown private key */
    snprintf(text, sizeof(text),
             "[global]\n"
             "default outputs = w0\n"
             "[outputs]\n"
             "w0 = http, url=http://127.0.0.1:1/logs, async=on, "
             "nosuchkey=1\n"
             "[rules]\n"
             "*.* = standard, w0\n");
    snprintf(cfgpath, sizeof(cfgpath), "%s_badkey.ini", g_base);
    http_write_config("badkey.ini", text);
    CHECK_EQ(hpulogc_init_from_file(cfgpath), HPULOGC_ERR_CONFIG);
    hpulogc_shutdown();
}

#else /* !HPULOGC_SINK_HTTP */

TEST(http_not_compiled_noop)
{
    /* HPULOGC_SINKS trims the http sink: registration-free pass. */
    CHECK(1);
}

#endif /* HPULOGC_SINK_HTTP */

#endif /* _WIN32 / HPULOGC_ENABLE_INI */
