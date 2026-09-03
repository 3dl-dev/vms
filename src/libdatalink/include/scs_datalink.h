/*
 * scs_datalink.h - vms-838: thin raw-L2 Ethernet datalink abstraction.
 *
 * RELOCATED HERE BY FC-P3.9. This lived in src/vmsscs/ and was built for
 * SCSD, the userspace SCS cluster daemon. That daemon is RETIRED (the
 * operator's 2026-09-02 clustering reset: the cluster port, SCS and the
 * connection manager are executive-resident, so nothing in the cluster path
 * opens a raw socket from userspace any more, and there is no SCS datalink
 * to abstract). The file survives its original caller because it was written
 * ENGINE-AGNOSTIC for exactly the second consumer it now has: DECnet Phase IV
 * (src/vmsdecnet/engine/decnetd.c, rd vms-30e), whose daemon puts DECnet
 * frames on the same kind of raw-L2 endpoint. The NAME is kept so the DECnet
 * call sites and the clean-room provenance below stay readable against the
 * history; nothing in it knows anything about SCS.
 *
 * WHY IT EXISTS AT ALL. A userspace daemon that puts frames on the wire needs
 * a raw-L2 endpoint, and the two substrates offer materially different ones:
 * Linux has AF_PACKET/SOCK_RAW; NetBSD -- the substrate the vax port builds
 * on (docs/design-ovmx-netbsd-syskrnl.md) -- has no AF_PACKET at all, only
 * bpf(4) (a cloning character device + ioctl(2) configuration + a
 * captured-packet framing on read). rd vms-838 closed the resulting
 * cross-arch parity gap by giving both platforms ONE header and swapping the
 * implementation underneath, rather than scattering #ifdef __linux__ /
 * #ifdef __NetBSD__ through the daemon itself.
 *
 * SURFACE, deliberately small: open a raw-L2 endpoint bound to an
 * interface, learn its hardware address, send a fully-built frame, receive
 * one, and bound how long a receive may block. That is everything a
 * frame-pushing daemon's transport layer needs and nothing else -- no
 * framing, no protocol knowledge, no policy, no ethertype baked in (the
 * caller passes its own), which is why it outlived the daemon it was written
 * for.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): the Linux backend is the original,
 * unmodified AF_PACKET behavior. The NetBSD backend is
 * derived solely from the public bpf(4) man page and the public
 * <net/bpf.h>/<net/if_dl.h> system headers (BIOCSETIF, BIOCIMMEDIATE,
 * BIOCSHDRCMPLT, BIOCSETF, BIOCGBLEN, BIOCSRTIMEOUT, struct bpf_hdr,
 * BPF_WORDALIGN, AF_LINK/struct sockaddr_dl) -- no VSI/HPE source, no
 * leaked material.
 *
 * WHAT THIS DOES NOT DO. It does not prove bpf send/recv against a live
 * interface under SIMH -- that needs a running NetBSD/vax instance (lab-2 /
 * lab-Alpha do not carry NetBSD-vax; the VAX session's SIMH lab does) and is
 * a follow-up. This header/implementation is scoped to make SCSD.EXE a
 * real, buildable, Decision-A-conformant elf32-vax executable, closing the
 * `ovmx-images` gap; see scs_datalink.c's file header for what IS proven
 * (that it builds and links) versus what remains a design-level claim about
 * bpf semantics (that it will interoperate on the wire).
 */
#ifndef SCS_DATALINK_H
#define SCS_DATALINK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * scs_datalink_open - open a raw-L2 endpoint bound to `ifname`, ready to
 * send/receive Ethernet frames of `ethertype`.
 *
 *   Linux:  socket(AF_PACKET, SOCK_RAW, htons(ethertype)) + bind(2) to
 *           ifname -- byte-for-byte the pre-vms-838 scsd.c sequence.
 *   NetBSD: opens /dev/bpf (a cloning device; falls back to /dev/bpfN for
 *           older kernels), BIOCSETIF to ifname, BIOCIMMEDIATE=1 (frames
 *           are delivered as they arrive -- SCS/HELLO timing is real-time
 *           sensitive, not throughput-batched), BIOCSHDRCMPLT=1 (scsd.c's
 *           frame builders already set the source MAC; this stops bpf from
 *           overwriting it on write), and a BIOCSETF filter restricting
 *           captured frames to `ethertype` (the read-side equivalent of
 *           the Linux bind's protocol filter).
 *
 * Returns an fd >= 0 on success, or -1 with errno set on failure.
 */
int scs_datalink_open(const char *ifname, uint16_t ethertype);

/*
 * scs_datalink_close - release a datalink fd opened by scs_datalink_open().
 *
 *   Linux:  close(2).
 *   NetBSD: releases this fd's internal read-buffer table entry (see
 *           scs_datalink_recv()'s header), then close(2).
 *
 * scsd.c's own close(sock) call sites were routed through this rather than
 * left as bare close(2) so the NetBSD backend's per-fd receive buffer never
 * outlives its fd -- harmless for a daemon that opens one datalink and
 * exits, but load-bearing the moment a caller (e.g. a future DECnet client,
 * or a test harness that opens/closes datalinks in a loop) reuses fd
 * numbers within one process lifetime.
 */
void scs_datalink_close(int fd);

/*
 * scs_datalink_get_hwaddr - resolve ifname's hardware (MAC) address.
 *
 *   Linux:  SIOCGIFHWADDR on a throwaway AF_INET socket -- unchanged from
 *           the pre-vms-838 scsd.c get_iface_hwaddr().
 *   NetBSD: getifaddrs(3) + the AF_LINK entry for ifname (NetBSD has no
 *           SIOCGIFHWADDR; this is the documented NetBSD way to read a
 *           link-layer address).
 *
 * Returns 0 on success, -1 with errno set on failure.
 */
int scs_datalink_get_hwaddr(const char *ifname, uint8_t mac_out[6]);

/*
 * scs_datalink_primary_iface - vms-5ad: resolve the host's PRIMARY (first,
 * enumeration order) non-loopback Ethernet net device -- the userspace twin
 * of the executive's exec_netdev_primary() (src/kernel-core/exec_kbackend.h
 * sec 11, src/kernel/exec_kbackend_linux.h's for_each_netdev/ARPHRD_ETHER
 * walk), used by SCSD's boot-cluster mode (scsd.c) to bind the same NIC the
 * device table names ETH0: without SCSD having to ask the executive or take
 * any argv (OVMX RUN /DETACHED passes none).
 *
 *   Linux:  if_nameindex(3) walk + SIOCGIFFLAGS (skip IFF_LOOPBACK) +
 *           SIOCGIFHWADDR (require ARPHRD_ETHER) -- the userspace mirror of
 *           for_each_netdev's IFF_LOOPBACK/ARPHRD_ETHER filter.
 *   NetBSD: getifaddrs(3) walk + the AF_LINK entry per interface, skipping
 *           IFF_LOOPBACK (ifa_flags) and requiring sdl_type == IFT_ETHER --
 *           the userspace mirror of IFNET_READER_FOREACH/IFT_ETHER.
 *
 * Copies the interface name (NUL-terminated, truncated to n-1) into `out`
 * when one exists and `out`/`n` are given. Returns 0 on success, -1 with
 * errno set (or ENODEV if no such interface exists) on failure -- the honest
 * "no NIC" case; the caller must never invent an interface name (INV-6).
 */
int scs_datalink_primary_iface(char *out, size_t n);

/*
 * scs_datalink_send - transmit a fully-built Ethernet frame (scsd.c's
 * frame builders already set every header field, including the source and
 * destination MAC) out `fd`.
 *
 *   Linux:  sendto() with a struct sockaddr_ll naming `ifindex`/`dst_mac` --
 *           unchanged from the pre-vms-838 scsd.c send_frame_raw().
 *   NetBSD: write(2). bpf has no per-packet destination address (unlike
 *           AF_PACKET's sockaddr_ll) -- the destination lives in the
 *           frame's own Ethernet header, which is already there, so
 *           `ifindex`/`ethertype`/`dst_mac` are accepted only so this
 *           signature matches the Linux backend and scsd.c's call site
 *           needs no per-platform branch; they are unused on this backend.
 *
 * Returns the byte count written (same contract as sendto(2)/write(2)), or
 * -1 with errno set on failure.
 */
ssize_t scs_datalink_send(int fd, int ifindex, uint16_t ethertype,
                          const uint8_t dst_mac[6],
                          const uint8_t *frame, size_t len);

/*
 * scs_datalink_recv - receive one frame into buf (up to buf_len bytes).
 *
 *   Linux:  recv(fd, buf, buf_len, 0) -- unchanged.
 *   NetBSD: read(2) into this fd's internal bpf buffer (sized from
 *           BIOCGBLEN at open time) and unwrap ONE struct bpf_hdr-framed
 *           capture from it, advancing past it with BPF_WORDALIGN(). bpf
 *           can return several captures in one read(); the rest stay
 *           queued in the fd's buffer and are drained on subsequent calls
 *           before the next real read() -- so, like Linux's recv(), each
 *           call to this function returns exactly one frame.
 *
 * Returns the byte count received, or -1 with errno set on failure
 * (including EAGAIN/EWOULDBLOCK on a receive-timeout expiry -- see
 * scs_datalink_set_recv_timeout() -- which scsd.c already treats as
 * "nothing pending, re-check the HELLO timer" on both backends).
 */
ssize_t scs_datalink_recv(int fd, uint8_t *buf, size_t buf_len);

/*
 * scs_datalink_set_recv_timeout - bound how long scs_datalink_recv() may
 * block, so a caller's poll loop wakes periodically even on an idle wire.
 *
 *   Linux:  setsockopt(SOL_SOCKET, SO_RCVTIMEO) -- unchanged.
 *   NetBSD: BIOCSRTIMEOUT.
 *
 * Returns 0 on success, -1 with errno set on failure.
 */
int scs_datalink_set_recv_timeout(int fd, int seconds);

#ifdef __cplusplus
}
#endif

#endif /* SCS_DATALINK_H */
