/**
 * @file win32_net.c
 * @brief Win32 backend of the hpu_net platform contract (socket
 *        primitives for network sinks, rd_v0.6 §4.10.8).
 *
 * Compiles under MSVC /W4 so the contract stays platform-complete, but
 * the tcp/udp sinks are POSIX-only in v1 (rd_v0.6 §4.10.8): nothing in
 * the v1 build calls into this file. Runtime support is a follow-up
 * work item. WSAStartup is issued lazily by the open/resolve calls.
 */

#include "platform/platform.h"

#if defined(_WIN32)

#include <errno.h>
#include <winsock2.h>
#include <ws2tcpip.h>

/* Winsock Sockets are unsigned handles; the contract uses int (the v1
 * sinks are POSIX-only, so the narrowing below is never exercised in
 * shipped configurations). */
static int sock_to_fd(SOCKET s)
{
    return (int)(intptr_t)s;
}

static SOCKET fd_to_sock(int fd)
{
    return (SOCKET)(intptr_t)fd;
}

/* Lazy WSAStartup: safe to call on every entry, kept once per process. */
static int winsock_ensure(void)
{
    static long initialized; /* 0 = not yet, 1 = ready, -1 = failed */
    WSADATA wsa;

    if (initialized == 1) {
        return 0;
    }
    if (initialized == -1) {
        errno = EIO;
        return -1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        initialized = -1;
        errno = EIO;
        return -1;
    }
    initialized = 1;
    return 0;
}

int hpu_net_resolve(const char* host, const char* port,
                    hpu_net_socktype_t type, void* addr_buf, size_t* out_len)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    int rc;

    if (winsock_ensure() != 0) {
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = (type == HPU_NET_DGRAM) ? SOCK_DGRAM : SOCK_STREAM;
    hints.ai_flags = 0;

    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == NULL) {
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
    SOCKET s;

    if (winsock_ensure() != 0) {
        return -1;
    }
    s = socket(sa->sa_family, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        errno = EIO;
        return -1;
    }
    if (connect(s, sa, (int)addrlen) != 0) {
        closesocket(s);
        errno = EIO;
        return -1;
    }
    return sock_to_fd(s);
}

int hpu_net_dgram_open(const void* addr, size_t addrlen)
{
    const struct sockaddr* sa = (const struct sockaddr*)addr;
    SOCKET s;
    u_long mode = 1; /* FIONBIO: non-blocking */

    if (winsock_ensure() != 0) {
        return -1;
    }
    s = socket(sa->sa_family, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        errno = EIO;
        return -1;
    }
    if (ioctlsocket(s, FIONBIO, &mode) != 0) {
        closesocket(s);
        errno = EIO;
        return -1;
    }
    return sock_to_fd(s);
}

int hpu_net_send(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    size_t off = 0;
    SOCKET s = fd_to_sock(fd);

    while (off < len) {
        int chunk = (len - off > 0x7FFFFFFFu) ? 0x7FFFFFFF : (int)(len - off);
        int n = send(s, p + off, chunk, 0);

        if (n == SOCKET_ERROR) {
            int wsa = WSAGetLastError();

            /* Surface POSIX-style errno so callers can share one error
             * classification (EPIPE/EAGAIN) with the POSIX backend. */
            if (wsa == WSAEWOULDBLOCK) {
                errno = EAGAIN;
            } else if (wsa == WSAECONNRESET || wsa == WSAECONNABORTED) {
                errno = ECONNRESET;
            } else {
                errno = EIO;
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
    SOCKET s = fd_to_sock(fd);
    int n = sendto(s, (const char*)buf, (int)len, 0,
                   (const struct sockaddr*)addr, (int)addrlen);

    if (n == SOCKET_ERROR) {
        int wsa = WSAGetLastError();

        if (wsa == WSAEWOULDBLOCK) {
            errno = EAGAIN;
        } else if (wsa == WSAEMSGSIZE) {
            errno = EMSGSIZE;
        } else {
            errno = EIO;
        }
        return -1;
    }
    return 0;
}

void hpu_net_close(int fd)
{
    if (fd >= 0) {
        closesocket(fd_to_sock(fd));
    }
}

#endif /* defined(_WIN32) */
