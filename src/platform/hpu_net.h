/**
 * @file hpu_net.h
 * @brief Platform contract: network socket primitives (address resolution,
 *        stream/dgram open, send, sendto, close) for network sinks
 *        (rd_v0.6 §4.10.8).
 *
 * The portable sink logic in src/output/ composes these primitives and must
 * not touch OS network headers. Error convention matches hpu_fs.h:
 * 0 (or a descriptor) on success, -1 on failure with errno set.
 * All send paths must never raise SIGPIPE on POSIX.
 *
 * v1 ships the POSIX backend; the Win32 backend (win32_net.c) compiles
 * under MSVC /W4 but runtime support of the tcp/udp sinks on Windows is a
 * follow-up work item (they are excluded from Windows builds in v1, like
 * syslog).
 */

#ifndef HPU_NET_H
#define HPU_NET_H

#include <stddef.h>

/** @brief Address buffer size guaranteeing storage of any resolved
 *         endpoint (covers sockaddr_in/sockaddr_in6 on all targets). */
#define HPU_NET_ADDR_MAX 128

/**
 * @brief Socket semantics requested from hpu_net_resolve()/open calls.
 */
typedef enum {
    HPU_NET_STREAM = 1, /*!< Reliable byte stream (TCP) */
    HPU_NET_DGRAM  = 2  /*!< Connectionless datagram (UDP) */
} hpu_net_socktype_t;

/**
 * @brief Resolve a host/port pair into a connect-ready endpoint address.
 *
 * Accepts numeric addresses and hostnames. The first usable result of the
 * resolution is stored opaquely into @p addr_buf (at most
 * HPU_NET_ADDR_MAX bytes; callers pass a buffer of that size).
 *
 * @param host      Hostname or numeric address (IP/IPv6).
 * @param port      Decimal port string (1-65535; validated by the caller).
 * @param type      Socket semantics (HPU_NET_STREAM or HPU_NET_DGRAM).
 * @param addr_buf  Output buffer, >= HPU_NET_ADDR_MAX bytes.
 * @param out_len   Filled with the stored address length in bytes.
 * @return          0 on success, -1 on failure (errno set; resolution
 *                  failure maps to EAI-style failure -> EIO domain).
 */
int hpu_net_resolve(const char* host, const char* port,
                    hpu_net_socktype_t type, void* addr_buf, size_t* out_len);

/**
 * @brief Open a stream socket and connect it to @p addr (blocking).
 *
 * The descriptor is guaranteed not to raise SIGPIPE on subsequent
 * hpu_net_send() calls.
 *
 * @param addr     Opaque endpoint address from hpu_net_resolve().
 * @param addrlen  Address length in bytes.
 * @return         Descriptor on success, -1 on failure (errno set).
 */
int hpu_net_stream_open(const void* addr, size_t addrlen);

/**
 * @brief Open an unconnected datagram socket (non-blocking).
 *
 * Non-blocking is required so that hpu_net_send_dgram() surfaces EAGAIN
 * instead of blocking when the kernel transmit buffer is full.
 *
 * @param addr     Opaque endpoint address from hpu_net_resolve() (stored
 *                 by the caller and passed to hpu_net_send_dgram()).
 * @param addrlen  Address length in bytes.
 * @return         Descriptor on success, -1 on failure (errno set).
 */
int hpu_net_dgram_open(const void* addr, size_t addrlen);

/**
 * @brief Write the whole buffer to a connected stream socket.
 *
 * Loops over partial sends. Never raises SIGPIPE (EPIPE is reported via
 * errno instead).
 *
 * @param fd   Socket descriptor.
 * @param buf  Data to send.
 * @param len  Number of bytes.
 * @return     0 on success, -1 on failure (errno set; EPIPE/ECONNRESET
 *             indicate a lost connection, EAGAIN a full send buffer).
 */
int hpu_net_send(int fd, const void* buf, size_t len);

/**
 * @brief Send exactly one datagram to @p addr (unconnected UDP socket).
 *
 * Single sendto attempt, no retry loop: the caller treats EAGAIN as a
 * discard-with-failed-accounting event (rd_v0.6 §4.10.8).
 *
 * @param fd       Socket descriptor.
 * @param buf      Datagram payload.
 * @param len      Payload length (caller-bounded, e.g. by the sink mtu).
 * @param addr     Opaque destination address from hpu_net_resolve().
 * @param addrlen  Address length in bytes.
 * @return         0 on success, -1 on failure (errno set; EAGAIN = would
 *                 block, EMSGSIZE = payload too large for one datagram).
 */
int hpu_net_send_dgram(int fd, const void* buf, size_t len,
                       const void* addr, size_t addrlen);

/**
 * @brief Close a socket descriptor. -1 is a no-op.
 * @param fd  Socket descriptor.
 */
void hpu_net_close(int fd);

#endif /* HPU_NET_H */
