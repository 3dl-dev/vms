/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bgsock.h - OVMX BSD-sockets RTL veneer over BGn: (the DECC$SOCKET-
 * equivalent). Item: the "BSD-sockets RTL veneer over BGn:" prerequisite for the
 * OpenSSH clients (vms-22a) / networking lane (vms-67f). See
 * docs/design-bgsockets-veneer-ovmx.md.
 *
 * THE LAYER THIS IS. An application (ssh, telnet, any TCP client) opens a socket
 * to a remote IP:port and lets the stack route; it NEVER touches the transport
 * device. So the correct OVMX layering is:
 *
 *   app  ->  socket()/connect()/send()/recv()   [STANDARD BSD sockets]
 *         ->  THIS veneer   (translates each sockets call into a $QIO op)
 *              ->  $QIO to BGn:  ->  vms.ko  ->  host kernel TCP/IP  ->  iface
 *
 * This veneer translates the standard BSD sockets calls into the public
 * $ASSIGN TCPIP$DEVICE: + $QIO ops the executive BGn: driver understands -- the
 * same ops src/vmstcpip/services/tcpip_client.h shows (IO$_SETMODE create /
 * IO$_ACCESS connect w/ sockaddr_in / IO$_READVBLK / IO$_WRITEVBLK /
 * IO$_DEACCESS). The application speaks only sockets; it never mentions
 * BGn:/$QIO/TCPIP$DEVICE:. This is the OpenVMS TCP/IP Services sockets-library
 * model (DECC$SOCKET): the socket is executive-resident (vms.ko, over the host
 * in-kernel socket API); there is NO userspace socket stack in the app.
 *
 * FULL-DUPLEX, SIMPLE BLOCKING SEMANTICS. ovmx_send() and ovmx_recv() each do
 * one BLOCKING $QIO (IO$_WRITEVBLK / IO$_READVBLK) directly on the channel. The
 * executive supports a blocking recv and a blocking send OUTSTANDING AT THE SAME
 * TIME on one channel: nothing in the $QIO path holds a lock across the socket
 * op (kernel unlocked_ioctl; vms_bg.c lifts the socket out from under
 * proc->chan_lock before the possibly-sleeping kernel_recvmsg/kernel_sendmsg;
 * sys_qio.c holds no lock across the kif call), and the host kernel socket is
 * inherently full-duplex. So an app (SSH above all) can read and write the same
 * connection concurrently from two threads -- proven by test_syssvc_bgsock_echo.
 * An earlier socketpair+pump-thread bridge that tried to expose a pollable fd
 * wedged on its own lifecycle; it was replaced by these direct blocking calls.
 *
 * INV-6 / Rule 9 (HONEST FAILURE). If /dev/vms is absent the executive INET
 * device does not exist: $ASSIGN TCPIP$DEVICE: returns SS$_NOSUCHDEV and
 * ovmx_socket() fails with errno=ENODEV -- it NEVER falls back to a raw Linux
 * socket() that would connect while sharing nothing with the executive.
 *
 * SCOPE. CLIENT path (ovmx_socket/connect/send/recv/shutdown/socket_close,
 * numeric-IPv4) AND SERVER path (ovmx_bind/listen/accept over the BGn: server
 * path, vms-698/#810). accept mints a new veneer handle for the connection, so a
 * --wrap-linked server accepts inbound the same way the client connects out. The
 * POLLABLE-fd form for OpenSSH's poll()/select() multiplexing landed too
 * (ovmx_pollfd -> VMS_IOCTL_BG_POLLFD, readiness-only). DNS is a later phase.
 * (There is NO "fixed fd bridge": a socketpair/pump would be a fabricated fd, the
 * exact veneer excised in vms-9ac -- data moves only through $QIO on the handle.)
 *
 * HANDLES, NOT libc fds. ovmx_socket() returns an OVMX socket HANDLE (>= 0, but
 * offset by OVMX_BGSOCK_BASE so it never collides with a real fd number). It is
 * used ONLY with the ovmx_* calls below -- never passed to libc read()/write()/
 * close()/poll(). (That is what the pollable-fd form above will add.)
 *
 * CLEAN-ROOM (Rule 8). The BSD sockets API is POSIX; the VMS-facing surface this
 * veneer speaks (TCPIP$DEVICE:, the QIO function map, the 8-byte BGn: sockaddr)
 * is from public VSI OpenVMS TCP/IP Services docs + the already-landed BGn:
 * driver, never from VSI/HPE source. Labeled an OVMX design choice.
 */

#ifndef _OVMX_VMS_BGSOCK_H
#define _OVMX_VMS_BGSOCK_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OVMX socket handles are offset by this base so they can never be mistaken for
 * a real libc fd (a caller that wrongly close()s one gets EBADF, not a silent
 * clobber of fd N). */
#define OVMX_BGSOCK_BASE 0x40000000

/* Open a BGn:-backed socket. domain must be AF_INET, type SOCK_STREAM (TCP).
 * On success returns an OVMX socket handle (>= OVMX_BGSOCK_BASE); on failure -1
 * with errno (ENODEV = no /dev/vms). */
int ovmx_socket(int domain, int type, int protocol);

/* Connect the BGn: socket to an AF_INET peer (IO$_ACCESS). -1/errno on failure. */
int ovmx_connect(int s, const struct sockaddr *addr, socklen_t addrlen);

/* Blocking send/recv -- each one BLOCKING $QIO (IO$_WRITEVBLK / IO$_READVBLK).
 * Full-duplex: a recv and a send may be outstanding at once on one handle from
 * two threads. ovmx_recv returns 0 on an orderly peer close (EOF). Both return
 * the byte count, or -1 with errno on error. */
ssize_t ovmx_send(int s, const void *buf, size_t len, int flags);
ssize_t ovmx_recv(int s, void *buf, size_t len, int flags);

/* SERVER path -- needs the BGn: server executive path (vms-698). Honest ENOSYS
 * until then (never a userspace fake, Rule 9/INV-6). */
int ovmx_bind(int s, const struct sockaddr *addr, socklen_t addrlen);
int ovmx_listen(int s, int backlog);
int ovmx_accept(int s, struct sockaddr *addr, socklen_t *addrlen);

/* IO$_DEACCESS (graceful shutdown) / IO$_DEACCESS + $DASSGN (release). */
int ovmx_shutdown(int s, int how);
int ovmx_socket_close(int s);

/* Return a REAL Linux readiness-only pollable fd for the connection, so an event
 * loop can poll()/select() on it to wait for readability/writability (this is
 * what lets OpenSSH's clientloop/serverloop poll the connection fd). The fd
 * exposes only readiness -- data still moves through ovmx_send()/ovmx_recv(); do
 * NOT read()/write() it. The caller close()s the returned fd when done. Returns
 * the fd (>= 0), or -1 with errno (ENODEV = no /dev/vms). */
int ovmx_pollfd(int s);

/* Return the channel's CACHED executive readiness fd (created once, owned by the
 * veneer socket, closed at ovmx_socket_close). This is what a poll()/select()
 * --wrap layer folds into a real fd set for a veneer handle; unlike ovmx_pollfd
 * the caller must NOT close it. Returns the fd (>= 0) or -1 with errno. */
int ovmx_readyfd(int s);

/* Socket-name + option surface (the OpenSSH de-veneer, vms-4bf). These read/
 * write the REAL executive-resident host kernel socket via new IO$_SENSEMODE /
 * IO$_SETMODE ops, so an UNMODIFIED OpenSSH's getpeername()/setsockopt() calls
 * touch VMS ($QIO on BGn:) rather than a bypassed AF_UNIX socketpair.
 *
 * ovmx_getpeername returns the TRUE remote IP:port of the connected peer (the
 * anti-veneer bar: known_hosts records the right host); ovmx_getsockname the
 * local endpoint. ovmx_set/getsockopt carry an integer option to/from the live
 * kernel socket (TCP_NODELAY / SO_KEEPALIVE / IP_TOS / SO_REUSEADDR / SO_ERROR)
 * -- honored for real, not swallowed; an unsupported option fails ENOPROTOOPT.
 * All return 0 on success, -1 with errno on failure (ENODEV = no /dev/vms). */
int ovmx_getpeername(int s, struct sockaddr *addr, socklen_t *addrlen);
int ovmx_getsockname(int s, struct sockaddr *addr, socklen_t *addrlen);
int ovmx_setsockopt(int s, int level, int optname, const void *optval,
                    socklen_t optlen);
int ovmx_getsockopt(int s, int level, int optname, void *optval,
                    socklen_t *optlen);

/* F_GETFL / F_SETFL for O_NONBLOCK (what OpenSSH sets on the connection fd).
 * Non-blocking send/recv are gated on the executive readiness fd so a
 * would-block returns EAGAIN without issuing a blocking $QIO. Returns the flags
 * (F_GETFL) or 0 on success, -1 with errno on failure. */
int ovmx_fcntl(int s, int cmd, int arg);

/* Numeric IPv4 helpers (no DNS yet). ovmx_inet_pton: "a.b.c.d" -> network-order
 * in_addr; returns 1 on success, 0 on a non-literal, -1 on bad af.
 * ovmx_getaddrinfo_numeric: fill *out (AF_INET) from a dotted-quad host + port;
 * returns 0 on success, or a negative error. */
int ovmx_inet_pton(int af, const char *src, void *dst);
int ovmx_getaddrinfo_numeric(const char *host, uint16_t port,
                             struct sockaddr_in *out);

#ifdef __cplusplus
}
#endif

#endif /* _OVMX_VMS_BGSOCK_H */
