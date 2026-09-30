/**
 * @file posix_net.c
 * @brief POSIX backend of the hpu_net platform contract (socket
 *        primitives for network sinks, rd_v0.6 §4.10.8).
 */

#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(SO_NOSIGPIPE)
#define HPU_NET_HAVE_SO_NOSIGPIPE 1
#endif
#if defined(MSG_NOSIGNAL)
#define HPU_NET_SEND_FLAGS MSG_NOSIGNAL
#else
#define HPU_NET_SEND_FLAGS 0
#endif

int hpu_net_resolve(const char* host, const char* port,
                    hpu_net_socktype_t type, void* addr_buf, size_t* out_len)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (type == HPU_NET_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = 0;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == NULL) {
        /* Collapse all resolution failures into one errno: the sink
         * surfaces a single HPULOGC_ERR_IO either way. */
        errno = EIO;
        return -1;
    }
    if (res->ai_addrlen > HPU_NET_ADDR_MAX) {
        freeaddrinfo(res);
        errno = ENOBUFS;
        return -1;
    }
    memcpy(addr_buf, res->ai_addr, res->ai_addrlen);
    *out_len = res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

int hpu_net_stream_open(const void* addr, size_t addrlen)
{
    const struct sockaddr* sa = (const struct sockaddr*)addr;
    int fd;

    fd = socket(sa->sa_family, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
#if defined(HPU_NET_HAVE_SO_NOSIGPIPE)
    {
        /* SIGPIPE suppression for platforms without MSG_NOSIGNAL
         * (macOS/BSD); Linux uses MSG_NOSIGNAL per send instead. */
        int on = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    if (connect(fd, sa, (socklen_t)addrlen) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int hpu_net_dgram_open(const void* addr, size_t addrlen)
{
    const struct sockaddr* sa = (const struct sockaddr*)addr;
    int fd;
    int flags;

    /* The unconnected socket keeps no peer; the caller passes the address
     * to hpu_net_send_dgram() per datagram. */
    (void)addrlen;

    fd = socket(sa->sa_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int hpu_net_send(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, HPU_NET_SEND_FLAGS);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int hpu_net_send_dgram(int fd, const void* buf, size_t len,
                       const void* addr, size_t addrlen)
{
    ssize_t n;

    do {
        n = sendto(fd, buf, len, 0, (const struct sockaddr*)addr,
                   (socklen_t)addrlen);
    } while (n < 0 && errno == EINTR);
    /* EINTR with no bytes sent is transparent; a datagram is atomic, so
     * a partial result cannot occur for len <= mtu. */
    return (n == (ssize_t)len) ? 0 : -1;
}

void hpu_net_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

int hpu_net_unix_stream_open(const char* path)
{
    struct sockaddr_un sa;
    size_t len;
    int fd;

    len = strlen(path);
    if (len == 0 || len >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
#if defined(HPU_NET_HAVE_SO_NOSIGPIPE)
    {
        /* Same SIGPIPE suppression as the TCP path (macOS/BSD). */
        int on = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, len + 1);
    if (connect(fd, (const struct sockaddr*)&sa, sizeof(sa)) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int hpu_net_unix_dgram_open(const char* path)
{
    struct sockaddr_un sa;
    size_t len;
    int fd;
    int flags;

    len = strlen(path);
    if (len == 0 || len >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* Non-blocking: hpu_net_send() must surface EAGAIN when the peer's
     * receive queue is full instead of blocking the emit callback. */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
#if defined(HPU_NET_HAVE_SO_NOSIGPIPE)
    {
        int on = 1;

        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    }
#endif
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, path, len + 1);
    if (connect(fd, (const struct sockaddr*)&sa, sizeof(sa)) != 0) {
        int saved = errno;

        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
