/* -*- coding: utf-8 -*- */

/**
 * @file sink_http.c
 * @brief HTTP/Webhook sink (POSIX only): hand-written HTTP/1.1 POST with
 *        keep-alive reuse and at-most-once batching (rd_v0.6 §4.10.10).
 *
 * The client is composed only from the hpu_net contract primitives (zero
 * third-party dependencies, rd_v0.6 §6); only the plaintext http://
 * scheme is accepted (D-R21 — TLS is terminated by a front reverse proxy
 * on the server side). Events are batched by the async worker (async=on
 * is mandatory, D-R22): emit_batch assembles the request body into the
 * pre-allocated priv buffer and issues one POST whenever the buffer runs
 * full, so one batch may split into several POSTs. Responses are
 * consumed only down to the status line + headers (draining a declared
 * Content-Length body with a bounded cap); 2xx counts the batch as
 * delivered, any other outcome (3xx/4xx/5xx, timeout, connection loss)
 * counts it failed with no retry (at-most-once).
 */

#include "output.h"

#if !defined(_WIN32)

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../platform/platform.h"

/** @brief Coalesced body buffer size (pre-allocated in priv). */
#define HPU_HTTP_BODY_SIZE (64U * 1024U)

/** @brief Request header block budget (stack; bounded by config limits:
 *         path <= 511, host <= 255, 8 headers x (127+255)). */
#define HPU_HTTP_REQHDR_MAX 4096U

/** @brief Response read buffer (status line + headers; responses whose
 *         header block exceeds this are treated as a failed request). */
#define HPU_HTTP_RESPBUF_MAX 2048U

/** @brief Cap on the declared response body drained for keep-alive. */
#define HPU_HTTP_DRAIN_MAX (64U * 1024U)

/** @brief Default/limit values for `timeout ms` (rd_v0.6 §4.10.10). */
#define HPU_HTTP_TIMEOUT_DEFAULT_MS 1000
#define HPU_HTTP_TIMEOUT_MIN_MS     100
#define HPU_HTTP_TIMEOUT_MAX_MS     60000

/** @brief Default/limit values for the reconnect backoff (seconds). */
#define HPU_HTTP_BACKOFF_DEFAULT_S   1
#define HPU_HTTP_BACKOFF_MAX_DEFAULT_S 30
#define HPU_HTTP_BACKOFF_MAX_LIMIT_S   3600

/** @brief Maximum accepted url host / path configuration lengths. */
#define HPU_HTTP_HOST_MAX 256
#define HPU_HTTP_PATH_MAX 512

/** @brief Maximum attached request headers and their key/value sizes. */
#define HPU_HTTP_HEADER_MAX     8
#define HPU_HTTP_HEADER_KEY_MAX 128
#define HPU_HTTP_HEADER_VAL_MAX 256

/**
 * @brief One attached request header (deep-copied at configure time).
 */
typedef struct hpu_http_header {
    char key[HPU_HTTP_HEADER_KEY_MAX]; /*!< Header field name */
    char val[HPU_HTTP_HEADER_VAL_MAX]; /*!< Header field value (trimmed) */
} hpu_http_header_t;

/**
 * @brief HTTP sink private data (allocated by the core, zeroed).
 */
typedef struct hpu_http_priv {
    char     host[HPU_HTTP_HOST_MAX]; /*!< Host parsed from url */
    char     path[HPU_HTTP_PATH_MAX]; /*!< Request path parsed from url */
    int      port;                    /*!< Port parsed from url (default 80) */
    hpu_http_header_t headers[HPU_HTTP_HEADER_MAX]; /*!< Attached headers */
    int      header_count;            /*!< Attached header count */
    int      ndjson;                  /*!< batch mode: 1 = ndjson, 0 = lines */
    int      is_v6;                   /*!< url host was an [IPv6] literal */
    int      timeout_ms;              /*!< Per-request send/recv timeout */
    int      backoff_init_s;          /*!< reconnect backoff (seconds) */
    int      backoff_max_s;           /*!< reconnect backoff max (seconds) */
    int      fd;                      /*!< Socket, -1 when disconnected */
    int      connected;               /*!< fd is a live connection */
    uint64_t backoff_ns;              /*!< Current backoff window */
    uint64_t retry_deadline_ns;       /*!< Monotonic reconnect gate */
    size_t   addrlen;                 /*!< Resolved endpoint length */
    unsigned char addr[HPU_NET_ADDR_MAX]; /*!< Resolved endpoint (start) */
    size_t   body_len;                /*!< Bytes pending in buf */
    size_t   body_events;             /*!< Events pending in buf */
    unsigned char buf[HPU_HTTP_BODY_SIZE]; /*!< Pre-allocated body buffer */
} hpu_http_priv_t;

/**
 * @brief Strict decimal parser for bounded non-negative ints.
 * @return 0 on success, -1 on malformed input or range violation.
 */
static int http_parse_uint(const char* val, long lo, long hi, int* out)
{
    char* end = NULL;
    long v;

    if (val == NULL || val[0] == '\0') {
        return -1;
    }
    v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < lo || v > hi) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/**
 * @brief Reset the pending body to the empty state.
 *
 * In ndjson mode buf[0] stays reserved for the opening '[' (written at
 * flush time); elements start at offset 1.
 */
static void http_reset_body(hpu_http_priv_t* p)
{
    p->body_len = p->ndjson ? 1U : 0U;
    p->body_events = 0;
}

/**
 * @brief Parse and validate the `url` value (http:// scheme only).
 *
 * Accepts http://host[:port][/path] and http://[ipv6-literal][:port][/path]
 * (brackets are stripped from the stored host — hpu_net_resolve must not
 * see them — and re-added for the Host header). Userinfo ('@') is not
 * supported. Whitespace anywhere in the URL is rejected. The default
 * port is 80 and the default path is "/" (rd_v0.6 §4.10.10, D-R21).
 *
 * @return 0 on success, -1 on any violation (-> HPULOGC_ERR_CONFIG).
 */
static int http_parse_url(hpu_http_priv_t* p, const char* val)
{
    const char* rest;
    const char* host_end;
    size_t host_len;
    size_t i;

    if (val == NULL || strncmp(val, "http://", 7) != 0) {
        return -1; /* plaintext http only (D-R21) */
    }
    rest = val + 7;
    if (rest[0] == '\0') {
        return -1;
    }
    p->is_v6 = 0;
    if (rest[0] == '[') {
        const char* close = strchr(rest, ']');

        if (close == NULL) {
            return -1;
        }
        rest++;          /* past '[': host bytes up to ']' */
        host_end = close;
        p->is_v6 = 1;
    } else {
        host_end = rest;
        while (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
            host_end++;
        }
    }
    host_len = (size_t)(host_end - rest);
    if (host_len == 0 || host_len >= sizeof(p->host)) {
        return -1;
    }
    for (i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)rest[i];

        if (c == '@' || c == '[' || c == ']' || c <= ' ' || c == 0x7F) {
            return -1;
        }
    }
    memcpy(p->host, rest, host_len);
    p->host[host_len] = '\0';
    p->port = 80;

    if (p->is_v6) {
        if (*host_end != ']') {
            return -1;
        }
        host_end++;
        if (*host_end != '\0' && *host_end != ':' && *host_end != '/') {
            return -1;
        }
    }
    if (*host_end == ':') {
        const char* digits = host_end + 1;
        char* end = NULL;
        long v;

        v = strtol(digits, &end, 10);
        if (end == digits || (*end != '\0' && *end != '/') ||
            v < 1 || v > 65535) {
            return -1;
        }
        p->port = (int)v;
        host_end = end;
    }

    if (*host_end == '\0') {
        memcpy(p->path, "/", 2);
        return 0;
    }
    if (*host_end != '/') {
        return -1;
    }
    if (strlen(host_end) >= sizeof(p->path)) {
        return -1;
    }
    for (i = 0; host_end[i] != '\0'; i++) {
        unsigned char c = (unsigned char)host_end[i];

        if (c < 0x20 || c == 0x7F) {
            return -1; /* no whitespace/control bytes in the path */
        }
    }
    memcpy(p->path, host_end, strlen(host_end) + 1);
    return 0;
}

/**
 * @brief Attach one `header` value ("k:v", repeatable key).
 *
 * The value is split at the first ':'; leading spaces/tabs of the value
 * are trimmed. CR/LF anywhere is rejected (header injection guard).
 *
 * @return 0 on success, -1 on any violation or when more than
 *         HPU_HTTP_HEADER_MAX headers were attached.
 */
static int http_add_header(hpu_http_priv_t* p, const char* val)
{
    const char* colon;
    const char* v;
    size_t klen;
    size_t vlen;
    size_t i;
    hpu_http_header_t* h;

    if (val == NULL || p->header_count >= HPU_HTTP_HEADER_MAX) {
        return -1;
    }
    colon = strchr(val, ':');
    if (colon == NULL) {
        return -1;
    }
    for (i = 0; val[i] != '\0'; i++) {
        unsigned char c = (unsigned char)val[i];

        if (c == '\r' || c == '\n') {
            return -1;
        }
    }
    klen = (size_t)(colon - val);
    if (klen == 0 || klen >= HPU_HTTP_HEADER_KEY_MAX) {
        return -1;
    }
    v = colon + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    vlen = strlen(v);
    if (vlen >= HPU_HTTP_HEADER_VAL_MAX) {
        return -1;
    }
    h = &p->headers[p->header_count];
    memcpy(h->key, val, klen);
    h->key[klen] = '\0';
    memcpy(h->val, v, vlen + 1);
    p->header_count++;
    return 0;
}

static int http_configure(hpulogc_sink_t* sink, const char* key,
                          const char* val)
{
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);

    if (strcmp(key, "url") == 0) {
        return http_parse_url(p, val);
    }
    if (strcmp(key, "header") == 0) {
        return http_add_header(p, val);
    }
    if (strcmp(key, "batch mode") == 0) {
        if (val != NULL && strcmp(val, "ndjson") == 0) {
            p->ndjson = 1;
            return 0;
        }
        if (val != NULL && strcmp(val, "lines") == 0) {
            p->ndjson = 0;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "timeout ms") == 0) {
        return http_parse_uint(val, HPU_HTTP_TIMEOUT_MIN_MS,
                               HPU_HTTP_TIMEOUT_MAX_MS, &p->timeout_ms);
    }
    if (strcmp(key, "reconnect backoff") == 0) {
        return http_parse_uint(val, 1, 60, &p->backoff_init_s);
    }
    if (strcmp(key, "reconnect backoff max") == 0) {
        return http_parse_uint(val, 1, HPU_HTTP_BACKOFF_MAX_LIMIT_S,
                               &p->backoff_max_s);
    }
    return -1; /* unknown key */
}

static int http_init(hpulogc_sink_t* sink)
{
    hpu_output_base_t* b = hpu_sink_base(sink);
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);

    /* async=on is mandatory for this type (rd_v0.6 §4.10.10, D-R22):
     * a synchronous HTTP POST would block the caller's thread for up
     * to timeout ms per record. The guard maps to HPULOGC_ERR_CONFIG. */
    if (!b->async) {
        return -1;
    }
    if (p->timeout_ms == 0) {
        p->timeout_ms = HPU_HTTP_TIMEOUT_DEFAULT_MS;
    }
    if (p->backoff_init_s == 0) {
        p->backoff_init_s = HPU_HTTP_BACKOFF_DEFAULT_S;
    }
    if (p->backoff_max_s == 0) {
        p->backoff_max_s = HPU_HTTP_BACKOFF_MAX_DEFAULT_S;
    }
    if (p->host[0] == '\0' || p->path[0] != '/' ||
        p->backoff_max_s < p->backoff_init_s) {
        return -1; /* required/invalid config -> HPULOGC_ERR_CONFIG */
    }
    http_reset_body(p);
    return 0;
}

/**
 * @brief Open a new connection and arm the request timeouts.
 * @return descriptor on success, -1 on failure.
 */
static int http_connect(hpu_http_priv_t* p)
{
    int fd = hpu_net_stream_open(p->addr, p->addrlen);

    if (fd < 0) {
        return -1;
    }
    if (hpu_net_set_timeout(fd, p->timeout_ms, p->timeout_ms) != 0) {
        int saved = errno;

        hpu_net_close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int http_start(hpulogc_sink_t* sink)
{
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);
    char port_str[8];

    snprintf(port_str, sizeof(port_str), "%d", p->port);
    if (hpu_net_resolve(p->host, port_str, HPU_NET_STREAM, p->addr,
                        &p->addrlen) != 0) {
        return -1;
    }
    /* Fail-fast first connect (rollingfile open semantics): the returned
     * error maps to HPULOGC_ERR_IO and fails init. */
    p->fd = http_connect(p);
    if (p->fd < 0) {
        return -1;
    }
    p->connected = 1;
    return 0;
}

/**
 * @brief Close the connection and reset the connection state without
 *        touching the reconnect backoff (planned non-reuse, e.g. the
 *        response declared `Connection: close`).
 */
static void http_conn_close(hpu_http_priv_t* p)
{
    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
    p->backoff_ns = 0;
    p->retry_deadline_ns = 0;
    http_reset_body(p);
}

/**
 * @brief Drop the connection after a failure and arm the backoff window.
 *
 * The retry gate is set to now + current backoff, then the backoff
 * doubles (capped at reconnect backoff max); a successful reconnect
 * resets it to the initial value. Pending body events are lost here —
 * the caller has already taken them out of its delivered count
 * (at-most-once, rd_v0.6 §4.10.10).
 */
static void http_mark_disconnected(hpu_http_priv_t* p)
{
    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
    http_reset_body(p);
    if (p->backoff_ns == 0) {
        p->backoff_ns = (uint64_t)p->backoff_init_s * 1000000000ULL;
    }
    p->retry_deadline_ns = hpu_now_ns() + p->backoff_ns;
    p->backoff_ns *= 2;
    if (p->backoff_ns > (uint64_t)p->backoff_max_s * 1000000000ULL) {
        p->backoff_ns = (uint64_t)p->backoff_max_s * 1000000000ULL;
    }
}

/**
 * @brief Attempt a reconnect when the backoff window has elapsed.
 * @return 0 connected, -1 still disconnected (backoff or connect error).
 */
static int http_reconnect(hpu_http_priv_t* p)
{
    if (hpu_now_ns() < p->retry_deadline_ns) {
        return -1;
    }
    /* Address comes from start-time resolution: re-resolving here would
     * mean blocking + allocating inside the emit callback (§4.10.4). */
    p->fd = http_connect(p);
    if (p->fd < 0) {
        p->retry_deadline_ns = hpu_now_ns() + p->backoff_ns;
        p->backoff_ns *= 2;
        if (p->backoff_ns > (uint64_t)p->backoff_max_s * 1000000000ULL) {
            p->backoff_ns = (uint64_t)p->backoff_max_s * 1000000000ULL;
        }
        return -1;
    }
    p->connected = 1;
    p->backoff_ns = (uint64_t)p->backoff_init_s * 1000000000ULL;
    p->retry_deadline_ns = 0;
    return 0;
}

/**
 * @brief Number of bytes event @p s adds to the ndjson body as one
 *        string element (quotes + escaping + separating comma).
 */
static size_t http_json_element_len(hpu_http_priv_t* p, const char* s,
                                    size_t n)
{
    size_t len = 2; /* enclosing quotes */
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':
        case '\\':
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            len += 2;
            break;
        default:
            len += (c < 0x20) ? 6U : 1U; /* \u00xx or passthrough */
            break;
        }
    }
    if (p->body_len > 1) {
        len += 1; /* separating comma between elements */
    }
    return len;
}

/**
 * @brief Append the escaped JSON string form of @p s at dst[*pos]
 *        (same semantics as the renderer's JSON escaping: '"', '\',
 *        the five short control escapes, \u00xx for other control
 *        bytes, UTF-8 bytes passed through).
 */
static void http_json_escape_append(unsigned char* dst, size_t* pos,
                                    const char* s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '"';
            break;
        case '\\':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = '\\';
            break;
        case '\b':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'b';
            break;
        case '\f':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'f';
            break;
        case '\n':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'n';
            break;
        case '\r':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'r';
            break;
        case '\t':
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 't';
            break;
        default:
            if (c < 0x20) {
                dst[(*pos)++] = '\\';
                dst[(*pos)++] = 'u';
                dst[(*pos)++] = '0';
                dst[(*pos)++] = '0';
                dst[(*pos)++] = (unsigned char)hex[(c >> 4) & 0xF];
                dst[(*pos)++] = (unsigned char)hex[c & 0xF];
            } else {
                dst[(*pos)++] = c;
            }
            break;
        }
    }
}

/**
 * @brief Case-insensitively find a request header name in the response
 *        header block; returns the byte after the ':' or NULL.
 */
static const unsigned char* http_find_header(const unsigned char* hdrs,
                                             size_t len, const char* name)
{
    size_t nlen = strlen(name);
    size_t i;

    for (i = 0; i + nlen + 1 <= len; i++) {
        if ((i == 0 || hdrs[i - 1] == '\n') &&
            strncasecmp((const char*)&hdrs[i], name, nlen) == 0 &&
            hdrs[i + nlen] == ':') {
            return &hdrs[i + nlen + 1];
        }
    }
    return NULL;
}

/**
 * @brief Check a `Connection:` header value (up to CR) for "close".
 */
static int http_header_says_close(const unsigned char* v, size_t max)
{
    size_t len = 0;

    while (len < max && v[len] != '\r' && v[len] != '\n' && v[len] != '\0') {
        len++;
    }
    /* token scan: any comma-separated token equal to "close" */
    {
        size_t i = 0;

        while (i < len) {
            while (i < len && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) {
                i++;
            }
            if (i + 5 <= len && strncasecmp((const char*)&v[i], "close",
                                            5) == 0 &&
                (i + 5 == len || v[i + 5] == ',' || v[i + 5] == ' ' ||
                 v[i + 5] == '\t')) {
                return 1;
            }
            while (i < len && v[i] != ',') {
                i++;
            }
        }
    }
    return 0;
}

/**
 * @brief Read the response status line and headers; drain a declared
 *        body (bounded) so the connection can be reused.
 *
 * @param p        Sink private state (fd must be connected).
 * @param status   Filled with the parsed status code on success.
 * @param reusable Filled with 1 when the connection may serve the next
 *                 request (2xx, not closed, body boundary known).
 * @return         0 when a status line was parsed, -1 on I/O failure,
 *                 timeout (errno ETIMEDOUT) or a runaway header block
 *                 (the caller must treat the connection as dead).
 */
static int http_read_response(hpu_http_priv_t* p, int* status, int* reusable)
{
    unsigned char rbuf[HPU_HTTP_RESPBUF_MAX];
    size_t len = 0;
    size_t hdr_end = 0;
    size_t i;
    int got_headers = 0;
    int code = 0;
    const unsigned char* v;
    long cl = -1;
    int close_flag;

    *reusable = 0;

    while (!got_headers) {
        int n;

        if (len >= sizeof(rbuf)) {
            return -1; /* runaway header block: treat as failed request */
        }
        n = hpu_net_recv(p->fd, rbuf + len, sizeof(rbuf) - len);
        if (n <= 0) {
            return -1; /* timeout / reset / orderly close: dead */
        }
        len += (size_t)n;
        for (i = 0; i + 3 < len; i++) {
            if (rbuf[i] == '\r' && rbuf[i + 1] == '\n' &&
                rbuf[i + 2] == '\r' && rbuf[i + 3] == '\n') {
                hdr_end = i + 4;
                got_headers = 1;
                break;
            }
        }
    }

    /* Status line: "HTTP/1.x NNN ..." */
    if (len < 12 || memcmp(rbuf, "HTTP/", 5) != 0) {
        return -1;
    }
    {
        const unsigned char* sp = memchr(rbuf, ' ', len);

        if (sp == NULL || (size_t)(sp - rbuf) + 4 > len ||
            sp[1] < '0' || sp[1] > '9' ||
            sp[2] < '0' || sp[2] > '9' ||
            sp[3] < '0' || sp[3] > '9') {
            return -1;
        }
        code = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    *status = code;

    v = http_find_header(rbuf, hdr_end, "content-length");
    if (v != NULL) {
        cl = strtol((const char*)v, NULL, 10);
        if (cl < 0) {
            cl = -1;
        }
    }
    close_flag = 0;
    v = http_find_header(rbuf, hdr_end, "connection");
    if (v != NULL) {
        close_flag = http_header_says_close(v, hdr_end - (size_t)(v - rbuf));
    }

    if (code < 200 || code > 299) {
        return 0; /* caller marks the batch failed and reconnects */
    }
    if (close_flag || cl < 0) {
        /* Body boundary unknown / peer closes: do not reuse. */
        return 0;
    }
    {
        size_t body_total = (size_t)cl;
        size_t buffered = len - hdr_end;
        size_t drained = buffered < body_total ? buffered : body_total;

        if (body_total > HPU_HTTP_DRAIN_MAX || buffered > body_total) {
            return 0; /* oversized or pipelined remnants: do not reuse */
        }
        while (drained < body_total) {
            int n = hpu_net_recv(p->fd, rbuf, sizeof(rbuf));

            if (n <= 0) {
                return -1; /* timeout mid-body: connection is dead */
            }
            drained += (size_t)n;
        }
        *reusable = 1;
    }
    return 0;
}

/**
 * @brief Build the request header block into @p out.
 * @return 0 on success, -1 when the block would overflow @p cap.
 */
static int http_build_request(const hpu_http_priv_t* p, size_t body_len,
                              char* out, size_t cap, size_t* out_len)
{
    char hostport[HPU_HTTP_HOST_MAX + 8];
    const char* ct = p->ndjson ? "application/x-ndjson" : "text/plain";
    size_t off;
    int n;
    int i;

    if (p->port == 80) {
        snprintf(hostport, sizeof(hostport),
                 p->is_v6 ? "[%s]" : "%s", p->host);
    } else {
        snprintf(hostport, sizeof(hostport),
                 p->is_v6 ? "[%s]:%d" : "%s:%d", p->host, p->port);
    }
    n = snprintf(out, cap,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lu\r\n",
                 p->path, hostport, ct, (unsigned long)body_len);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    off = (size_t)n;
    for (i = 0; i < p->header_count; i++) {
        n = snprintf(out + off, cap - off, "%s: %s\r\n",
                     p->headers[i].key, p->headers[i].val);
        if (n < 0 || (size_t)n >= cap - off) {
            return -1;
        }
        off += (size_t)n;
    }
    if (off + 2 >= cap) {
        return -1;
    }
    out[off] = '\r';
    out[off + 1] = '\n';
    *out_len = off + 2;
    return 0;
}

/**
 * @brief Send the pending body as one POST and consume the response.
 *
 * On any failure (send error, timeout, non-2xx status) the connection is
 * marked down and the pending events are taken out of @p done so the
 * caller's return value reflects only records that did not end up
 * accounted failed (at-most-once: no retry, rd_v0.6 §4.10.10).
 *
 * @return 0 when the caller may continue, -1 when disconnected/failed.
 */
static int http_flush_post(hpu_http_priv_t* p, size_t* done)
{
    char hdr[HPU_HTTP_REQHDR_MAX];
    size_t hdr_len;
    size_t body_len;
    size_t events;
    int status = 0;
    int reusable = 0;

    if (p->body_events == 0) {
        return 0;
    }
    body_len = p->body_len;
    events = p->body_events;

    if (p->ndjson) {
        p->buf[0] = '[';         /* reserved slot */
        p->buf[body_len] = ']';  /* body_len >= 1: room is budgeted */
        body_len++;
    }

    if (http_build_request(p, body_len, hdr, sizeof(hdr), &hdr_len) != 0 ||
        hpu_net_send(p->fd, hdr, hdr_len) != 0 ||
        hpu_net_send(p->fd, p->buf, body_len) != 0) {
        http_mark_disconnected(p);
        *done -= events;
        return -1;
    }
    if (http_read_response(p, &status, &reusable) != 0 || status < 200 ||
        status > 299) {
        /* At-most-once: 3xx/4xx/5xx, timeout and connection loss all
         * count the batch failed without retry. */
        http_mark_disconnected(p);
        *done -= events;
        return -1;
    }
    if (!reusable) {
        /* Planned non-reuse (e.g. Connection: close): no backoff. */
        http_conn_close(p);
        return 0;
    }
    p->body_len = p->ndjson ? 1U : 0U;
    p->body_events = 0;
    return 0;
}

static void http_emit(hpulogc_sink_t* sink, const hpulogc_event_t* ev)
{
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);
    size_t done = 0;
    size_t need;
    size_t budget = sizeof(p->buf) - (p->ndjson ? 1U : 0U);

    if (!p->connected && http_reconnect(p) != 0) {
        hpu_sink_account_failed(sink, 1); /* inside the backoff window */
        return;
    }
    /* Defensive single-event POST: the sync path is rejected at init
     * (async=on is mandatory, D-R22), so this only serves direct emit
     * calls that the core no longer generates for this type. */
    need = p->ndjson ? http_json_element_len(p, ev->line, ev->line_len)
                     : ev->line_len + 1;
    if (p->body_len + need > budget) {
        hpu_sink_account_failed(sink, 1); /* oversized: not delivered */
        return;
    }
    if (p->ndjson) {
        if (p->body_len > 1) {
            p->buf[p->body_len++] = ',';
        }
        p->buf[p->body_len++] = '"';
        http_json_escape_append(p->buf, &p->body_len, ev->line,
                                ev->line_len);
        p->buf[p->body_len++] = '"';
    } else {
        if (ev->line_len > 0) {
            memcpy(p->buf + p->body_len, ev->line, ev->line_len);
        }
        p->buf[p->body_len + ev->line_len] = '\n';
        p->body_len += ev->line_len + 1;
    }
    p->body_events = 1;
    done = 1;
    if (http_flush_post(p, &done) != 0) {
        hpu_sink_account_failed(sink, 1);
    }
}

static int http_emit_batch(hpulogc_sink_t* sink,
                           const hpulogc_event_t* const* evs, size_t n)
{
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);
    size_t i;
    size_t done = 0;
    size_t budget = sizeof(p->buf) - (p->ndjson ? 1U : 0U);

    if (!p->connected && http_reconnect(p) != 0) {
        return 0; /* whole batch lands in the backoff window */
    }
    for (i = 0; i < n; i++) {
        const hpulogc_event_t* ev = evs[i];
        size_t need;

        if (p->ndjson) {
            size_t esc = http_json_element_len(p, ev->line, ev->line_len);

            /* A lone element never fits: '[' + quotes + escaped + ']' */
            if (2 + esc + 1 > budget) {
                continue; /* oversized: left out of done -> failed */
            }
            need = esc;
        } else {
            if (ev->line_len + 1 > budget) {
                continue; /* oversized: left out of done -> failed */
            }
            need = ev->line_len + 1;
        }
        if (p->body_len + need > budget) {
            if (http_flush_post(p, &done) != 0) {
                return (done > 0) ? (int)done : -1;
            }
            /* After the flush the comma is not needed; recompute. */
            if (p->ndjson) {
                need = http_json_element_len(p, ev->line, ev->line_len);
            }
        }
        if (p->ndjson) {
            if (p->body_len > 1) {
                p->buf[p->body_len++] = ',';
            }
            p->buf[p->body_len++] = '"';
            http_json_escape_append(p->buf, &p->body_len, ev->line,
                                    ev->line_len);
            p->buf[p->body_len++] = '"';
        } else {
            if (ev->line_len > 0) {
                memcpy(p->buf + p->body_len, ev->line, ev->line_len);
            }
            p->buf[p->body_len + ev->line_len] = '\n';
            p->body_len += ev->line_len + 1;
        }
        p->body_events++;
        done++;
    }
    if (http_flush_post(p, &done) != 0) {
        return (done > 0) ? (int)done : -1;
    }
    return (int)done;
}

static void http_destroy(hpulogc_sink_t* sink)
{
    hpu_http_priv_t* p = hpulogc_sink_priv(sink);

    if (p->fd >= 0) {
        hpu_net_close(p->fd);
        p->fd = -1;
    }
    p->connected = 0;
}

/** @brief HTTP sink type (SYNC|ASYNC|LINE_ATOMIC; async=on enforced at
 *         init; POSIX builds). */
static const hpulogc_sink_ops_t g_http_ops = {
    "http",
    HPULOGC_SINK_ABI_VERSION,
    HPULOGC_CAP_SYNC | HPULOGC_CAP_ASYNC | HPULOGC_CAP_LINE_ATOMIC,
    sizeof(hpu_http_priv_t),
    http_configure,
    http_init,
    http_start,
    http_emit,
    http_emit_batch,
    NULL, /* flush: the body buffer is drained per emit_batch call */
    NULL, /* sync: no FSYNC capability */
    NULL, /* periodic: reconnect is emit-path driven */
    http_destroy,
    { NULL, NULL, NULL, NULL }
};

const hpulogc_sink_ops_t* hpu_http_sink_ops(void)
{
    return &g_http_ops;
}

#endif /* !defined(_WIN32) */
