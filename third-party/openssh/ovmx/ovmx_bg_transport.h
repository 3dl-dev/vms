/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_bg_transport.h - OVMX port layer: route OpenSSH's client transport over
 * the executive-resident INET pseudo-device BGn: (vms-527) instead of a raw
 * Linux socket().  Item vms-22a (OpenSSH CLIENTS), parent vms-843,
 * docs/design-openssh-port-ovmx.md §2.3.
 *
 * WHAT THIS IS. Stock OpenSSH's ssh client opens its TCP connection in
 * sshconnect.c: ssh_create_socket() does socket(), ssh_connect_direct() does
 * connect(), and ssh_packet_set_connection() then drives ALL of KEX / auth /
 * channel I/O as read()/write()/poll() on that one fd.  On OVMX a VMS program
 * does NOT reach TCP through a userspace socket -- it $ASSIGNs a channel to
 * TCPIP$DEVICE: and $QIOs on it, and the socket lives in the executive (vms.ko,
 * over the host kernel's in-kernel socket API).  This is the SAME public
 * $ASSIGN/$QIO/$DASSGN path src/vmstcpip/services/tcpip_client.h uses for the
 * DCL TELNET/FTP verbs (vms-dbb) and test_syssvc_bg_echo.c proves (vms-527).
 *
 * THE SUBSTITUTION.  ovmx_bg_connect() returns a *pollable local fd* (one end
 * of a socketpair) that OpenSSH treats exactly as it would a connected TCP
 * socket -- so OpenSSH's buffered/poll I/O loop is UNCHANGED -- while a pair of
 * pump threads shuttle every byte between the other end of the socketpair and a
 * BGn: channel via IO$_WRITEVBLK / IO$_READVBLK.  Every byte on the wire transits
 * $QIO into the executive; the socketpair is purely an in-process fd handoff so
 * the vendored OpenSSH source needs only a minimal, localized edit (see
 * ovmx/sshconnect-bg.patch) rather than a rewrite of its packet layer.  This is
 * the concrete realization of the design's "BSD-socket veneer over BGn:".
 *
 * INV-6 / Rule 9 (HONEST FAILURE).  If /dev/vms is absent the executive INET
 * device does not exist: $ASSIGN TCPIP$DEVICE: returns SS$_NOSUCHDEV and
 * ovmx_bg_connect() fails with errno=ENODEV -- it NEVER falls back to a raw
 * Linux socket() that would connect "successfully" while sharing nothing with
 * the executive.  A silent userspace fallback is exactly the LARP bug class the
 * authenticity invariants exist to kill.
 *
 * Rule 8.  OpenSSH is BSD/ISC OSS we may legitimately port; this shim is our
 * own OVMX code.  The VMS-facing surface it speaks (TCPIP$DEVICE:, the QIO
 * function map, the 8-byte sockaddr the BGn: IO$_ACCESS handler reads) is from
 * public VSI OpenVMS TCP/IP Services docs + the already-landed BGn: driver, not
 * from VSI/HPE source.
 *
 * BUILD.  This file is compiled and linked ONLY into the OVMX-native ssh IMGACT
 * image (which links libvms for the $ASSIGN/$QIO services); the patch that calls
 * it is guarded by -DOVMX_BG_TRANSPORT.  The default static-musl build probe
 * (third-party/openssh/build-openssh.sh, CI job openssh-static-musl) builds
 * stock ssh WITHOUT the shim to prove OpenSSH+LibreSSL+musl links; the shim is
 * compile-checked against the real OVMX headers there as a separate step.
 */

#ifndef _OVMX_BG_TRANSPORT_H
#define _OVMX_BG_TRANSPORT_H

#include <sys/socket.h>

/*
 * Open an OVMX BGn: transport to the peer named by sa (an AF_INET sockaddr; the
 * ssh client resolved the host to this).  On success returns a connected,
 * pollable fd that OpenSSH uses as its transport socket; on failure returns -1
 * with errno set (ENODEV when /dev/vms / the executive INET device is absent,
 * EAFNOSUPPORT for a non-AF_INET peer, ECONNREFUSED / EIO for a connect or
 * setup failure mapped from the VMS status).
 */
int ovmx_bg_connect(const struct sockaddr *sa, socklen_t salen);

#endif /* _OVMX_BG_TRANSPORT_H */
