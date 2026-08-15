/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_ssh_glue.h - OVMX port layer wiring OpenSSH's transport + event loop onto
 * the OVMX BSD-sockets veneer over BGn: (item vms-22a, parent vms-843,
 * docs/design-openssh-port-ovmx.md). OpenSSH keeps its STANDARD BSD calls; this
 * glue is where they resolve to the veneer -- there is NO $QIO / vms_kif code in
 * OpenSSH, and NO raw Linux socket(): everything routes veneer -> BGn: ->
 * executive.
 *
 * HOW THE SUBSTITUTION WORKS. OpenSSH's connection fd is used two ways: its
 * event loop poll()/ppoll()s it for readiness (packet.c ppoll, clientloop.c
 * poll set), and its packet layer read()/write()s it for data. So this glue
 * returns, as OpenSSH's connection fd, the veneer's REAL readiness fd
 * (ovmx_pollfd / VMS_IOCTL_BG_POLLFD) -- a genuine pollable Linux fd -- so ALL
 * of OpenSSH's poll paths work UNCHANGED. Only the three data-I/O sites in
 * packet.c are shimmed to move bytes through the veneer (ovmx_send/ovmx_recv)
 * instead of read()/write() on that readiness fd, which carries no data. The
 * glue keeps a small fd->handle map so those shims find the veneer handle for a
 * given connection fd.
 *
 * INV-6 / Rule 9. ovmx_ssh_connect() fails (returns -1) when /dev/vms is absent
 * (the veneer's ovmx_socket returns ENODEV) -- never a raw socket() fallback.
 */

#ifndef _OVMX_SSH_GLUE_H
#define _OVMX_SSH_GLUE_H

#include <sys/types.h>
#include <sys/socket.h>

struct sshbuf;   /* opaque; the sshbuf shim is defined in the glue TU */

/* Open + connect a veneer transport to the AF_INET peer `sa`, and return a REAL,
 * pollable Linux fd (the veneer readiness fd) for OpenSSH to use as its
 * connection fd. -1 with errno on failure (ENODEV = no executive). Substitutes
 * ssh_create_socket()+timeout_connect() in sshconnect.c. */
int ovmx_ssh_connect(const struct sockaddr *sa, socklen_t salen);

/* True if `fd` is a veneer connection fd this glue handed out. */
int ovmx_ssh_is_conn(int fd);

/* Data shims for the three packet.c I/O sites. For a veneer connection fd they
 * move bytes through the veneer (ovmx_send/ovmx_recv); otherwise they fall back
 * to the ordinary read()/write()/sshbuf_read so non-connection fds are
 * unaffected. Semantics mirror read()/write(): 0 = EOF, -1 = error (errno set). */
ssize_t ovmx_ssh_read(int fd, void *buf, size_t n);
ssize_t ovmx_ssh_write(int fd, const void *buf, size_t n);
/* sshbuf variant used by ssh_packet_process_read(): reads up to maxlen into buf,
 * returns an SSH_ERR_* code (0 = ok), *rlen = bytes read. */
int ovmx_ssh_sshbuf_read(int fd, struct sshbuf *buf, size_t maxlen, size_t *rlen);

#endif /* _OVMX_SSH_GLUE_H */
