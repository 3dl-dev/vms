/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bgsock.h - OVMX BSD-sockets RTL veneer over BGn: (the DECC$SOCKET-
 * equivalent). Item: the "BSD-sockets RTL veneer over BGn:" prerequisite for the
 * OpenSSH clients (vms-22a) / networking lane (vms-67f). See
 * docs/design-openssh-port-ovmx.md §7.1 and docs/design-bgsockets-veneer-ovmx.md.
 *
 * THE LAYER THIS IS. An application (ssh, telnet, any TCP client) opens a socket
 * to a remote IP:port and lets the stack route; it NEVER touches the transport
 * device. So the correct OVMX layering is:
 *
 *   app  ->  socket()/connect()   [STANDARD BSD sockets]
 *         ->  THIS veneer          (translates sockets calls into $QIO ops)
 *              ->  $QIO to BGn:  ->  vms.ko  ->  host kernel TCP/IP  ->  iface
 *
 * This veneer is the missing middle layer. It translates the standard BSD
 * sockets calls into the public $ASSIGN TCPIP$DEVICE: + $QIO ops the executive
 * BGn: driver understands -- exactly the ops src/vmstcpip/services/tcpip_client.h
 * already shows (IO$_SETMODE create / IO$_ACCESS connect w/ sockaddr_in /
 * IO$_READVBLK / IO$_WRITEVBLK / IO$_DEACCESS). The application speaks only
 * sockets; it never mentions BGn:/$QIO/TCPIP$DEVICE:. This is the OpenVMS TCP/IP
 * Services sockets-library model (DECC$SOCKET): the socket is executive-resident
 * (vms.ko, over the host in-kernel socket API); there is NO userspace socket
 * stack in the app.
 *
 * POLLABLE FDS. ovmx_socket()/ovmx_connect() return an ORDINARY, POLLABLE fd
 * (one end of a socketpair); a pair of pump threads shuttle every byte between
 * that fd and the executive BGn: channel via IO$_READVBLK/IO$_WRITEVBLK. So the
 * app's ordinary read()/write()/poll()/select() on the fd work UNCHANGED (this
 * is what lets a large consumer like OpenSSH use the veneer with only minimal
 * porting -- map socket/connect/close to the ovmx_* entries). Every wire byte
 * still transits $QIO into the executive.
 *
 * BRIDGE CAVEAT (OPEN -- docs/design-bgsockets-veneer-ovmx.md §4). The current
 * two-thread pump issues concurrent BLOCKING $QIOW read and write on the SAME
 * BGn: channel; a first QEMU proof showed this WEDGES in-guest (the proven raw
 * path test_syssvc_bg_echo only ever does write-THEN-read on one thread). The
 * next increment reworks the bridge to async $QIO + AST multiplex (or confirms
 * concurrent per-channel read/write in vms_bg.c) and lands the green Rule-9
 * proof + its negctl. Until then the verified surface is compile + the host-side
 * honest-skip / resolver, not the /dev/vms byte round-trip.
 *
 * INV-6 / Rule 9 (HONEST FAILURE). If /dev/vms is absent the executive INET
 * device does not exist: $ASSIGN TCPIP$DEVICE: returns SS$_NOSUCHDEV and
 * ovmx_socket() fails with errno=ENODEV -- it NEVER falls back to a raw Linux
 * socket() that would connect while sharing nothing with the executive. A silent
 * userspace fallback is exactly the LARP bug class the authenticity invariants
 * exist to kill.
 *
 * SCOPE (this increment). CLIENT path over loopback / a localhost peer, IPv4.
 * ovmx_socket/ovmx_connect/ovmx_send/ovmx_recv/ovmx_shutdown/ovmx_socket_close +
 * numeric-IPv4 resolution (ovmx_inet_pton / ovmx_getaddrinfo_numeric). The
 * SERVER path (bind/listen/accept) needs the BGn: server path (vms-698, in
 * progress) and returns ENOSYS honestly until then. DNS resolution is a later
 * phase; a host argument must be a dotted-quad IPv4 literal for now.
 *
 * CLEAN-ROOM (Rule 8). The BSD sockets API is POSIX; the VMS-facing surface this
 * veneer speaks (TCPIP$DEVICE:, the QIO function map, the 8-byte BGn: sockaddr)
 * is from public VSI OpenVMS TCP/IP Services docs + the already-landed BGn:
 * driver, never from VSI/HPE source. This veneer is labeled an OVMX design
 * choice; no VMS-authentic byte-layout claim is made for it.
 */

#ifndef _OVMX_VMS_BGSOCK_H
#define _OVMX_VMS_BGSOCK_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open a BGn:-backed socket. domain must be AF_INET, type SOCK_STREAM (TCP);
 * anything else fails EAFNOSUPPORT/EPROTONOSUPPORT. On success returns an
 * ordinary pollable fd; on failure -1 with errno (ENODEV = no /dev/vms). */
int ovmx_socket(int domain, int type, int protocol);

/* Connect the BGn: socket to an AF_INET peer (IO$_ACCESS). Starts the pump
 * threads so read()/write()/poll() then work on the fd. -1/errno on failure. */
int ovmx_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);

/* SERVER path -- needs the BGn: bind/listen/accept executive path (vms-698).
 * Until that lands these fail honestly with ENOSYS (never a userspace fake). */
int ovmx_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);
int ovmx_listen(int fd, int backlog);
int ovmx_accept(int fd, struct sockaddr *addr, socklen_t *addrlen);

/* Thin wrappers over the pollable fd (the pump does the $QIO). Provided for API
 * completeness / the eventual standard-name mapping; read()/write() on the fd
 * returned by ovmx_socket() are equivalent. */
ssize_t ovmx_send(int fd, const void *buf, size_t len, int flags);
ssize_t ovmx_recv(int fd, void *buf, size_t len, int flags);

/* Shut down / close the BGn: socket: tears down the pumps and IO$_DEACCESS +
 * $DASSGN the channel, then closes the fd. Use ovmx_socket_close() in place of
 * close() on a veneer fd so the executive channel is released. */
int ovmx_shutdown(int fd, int how);
int ovmx_socket_close(int fd);

/* Numeric IPv4 helpers (no DNS yet). ovmx_inet_pton: "a.b.c.d" -> network-order
 * in_addr; returns 1 on success, 0 on a non-literal, -1 on bad af.
 * ovmx_getaddrinfo_numeric: fill *out (AF_INET) from a dotted-quad host + port;
 * returns 0 on success, or a negative error (host not a numeric IPv4 literal). */
int ovmx_inet_pton(int af, const char *src, void *dst);
int ovmx_getaddrinfo_numeric(const char *host, uint16_t port,
                             struct sockaddr_in *out);

#ifdef __cplusplus
}
#endif

#endif /* _OVMX_VMS_BGSOCK_H */
