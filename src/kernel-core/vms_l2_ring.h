/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_l2_ring.h - the receive-ring bookkeeping of an executive L2 handle on a
 * substrate that queues frames itself (rd vms-9394: the NetBSD binding in
 * src/kernel-netbsd/vms_socket_netbsd.c, where there is no AF_PACKET socket
 * queue to lean on). Header-only, dependency-free (needs only unsigned int in
 * scope) and lock-free by itself -- the caller holds its own lock around every
 * call -- so the SAME index arithmetic the kernel runs is unit-tested on the
 * host (tests/vmsdecnet/test_l2_ring.c) without a kernel.
 *
 * POLICY (OVMX's own, stated rather than implied -- CLAUDE.md Rule 8): a full
 * ring DROPS THE OLDEST frame to make room for the newest. A datalink loses
 * frames under overload; DECnet's NSP retransmits, and a reader that fell
 * behind is better served by the most recent routing hellos than by a backlog
 * of stale ones. Receive context never allocates and never blocks on a full
 * ring, which is what the rx-context contract of exec_kbackend.h requires.
 */
#ifndef _VMS_L2_RING_H
#define _VMS_L2_RING_H

struct vms_l2_ring {
	unsigned int head;    /* index of the oldest queued frame */
	unsigned int count;   /* frames queued, 0..cap */
	unsigned int cap;     /* slots (fixed at init) */
	unsigned int drops;   /* frames discarded to make room (drop-oldest) */
};

static inline void vms_l2_ring_init(struct vms_l2_ring *r, unsigned int cap)
{
	r->head = 0;
	r->count = 0;
	r->cap = cap;
	r->drops = 0;
}

/* Reserve the slot the next received frame is copied into, discarding the
 * oldest frame first when the ring is full. Always succeeds (cap >= 1). */
static inline unsigned int vms_l2_ring_push(struct vms_l2_ring *r)
{
	unsigned int slot;

	if (r->count == r->cap) {
		r->head = (r->head + 1) % r->cap;
		r->count--;
		r->drops++;
	}
	slot = (r->head + r->count) % r->cap;
	r->count++;
	return slot;
}

/* Take the oldest queued frame: returns 1 and its slot, or 0 when empty. The
 * caller copies the slot's bytes out under the same lock. */
static inline int vms_l2_ring_pop(struct vms_l2_ring *r, unsigned int *slot)
{
	if (r->count == 0)
		return 0;
	*slot = r->head;
	r->head = (r->head + 1) % r->cap;
	r->count--;
	return 1;
}

/* The ethertype (host order) of an Ethernet II frame, from its first 14 bytes:
 * the big-endian 16-bit field after the two 6-byte addresses. */
static inline unsigned int vms_l2_frame_ethertype(const unsigned char hdr[14])
{
	return ((unsigned int)hdr[12] << 8) | (unsigned int)hdr[13];
}

#endif /* _VMS_L2_RING_H */
