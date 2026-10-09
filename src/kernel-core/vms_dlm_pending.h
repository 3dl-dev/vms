/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_pending.h - THE REQUESTS THIS MASTER HAS NOT ANSWERED YET, kept so
 * the grant can be the requester's OWN frame when the queue finally advances
 * (rd vms-f87).
 *
 * Substrate-portable kernel-core: no backend, no time, no allocation, no
 * globals -- a fixed table and three operations over it.
 *
 * ===========================================================================
 * WHY IT EXISTS, MEASURED
 *
 * 2026-10-09 10:56Z, vaxlab-3: an OVMX master held EX on EVAC$WORKLOAD, VAX1
 * $ENQW'd EX and was QUEUED at this master (correct -- a real lock on a real
 * waiting queue). OVMX then released its EX, the engine flipped VAX1's request
 * to GRANTED for real... and nothing was sent. VAX1's process sat in RWSCS
 * indefinitely; even STOP could not complete it.
 *
 * The arm's reason for the silence was that answering would mean emitting a
 * cat-0x82 at a system that "did not just ask", and only the answer-to-a-
 * request shape is grounded. That reason has now been MEASURED instead of
 * assumed (tools/cluster/dlm_grant_correlation.py over 129 real captures):
 *
 *   - a real master's grant is correlated to its request by the REQUESTER's own
 *     handle at body[24:28], which the master echoes back -- 27,513 grants in
 *     one reference capture, every one matched that way;
 *   - the grant need NOT be the next frame on the connection: gaps up to 137 ms
 *     with other frames in between are normal;
 *   - and a real VAX master DOES originate grants nobody just asked for: 139 of
 *     them inside one second in the f03 reference capture, same shape as every
 *     other grant (the record at body[28]/body[32:36], body[30] cleared, no
 *     name).
 *
 * So the frame is grounded. What was missing was a way to send the requester's
 * OWN bytes back: this codec builds a grant by ECHOING THE REQUEST (rd
 * vms-b5b0, after a 65,000-frame storm caused by laying the fields out by
 * hand), and by the time the queue advances the request frame is long gone.
 *
 * THIS TABLE KEEPS IT. One entry per cross-node request this master really
 * queued, held exactly as long as the master owes that requester an answer. The
 * deferred grant is then BYTE-FOR-BYTE the grant the requester would have got
 * had it asked again -- which is what the engine's idempotent retransmit path
 * already produces -- and not a frame this executive composed from fields.
 *
 * WHAT IT IS NOT. It is not a retransmit queue, not a timer, and not a record
 * of anything this node was told: every entry is a frame a peer sent us, kept
 * because we have not answered it. When the table is full the request is still
 * QUEUED in the lock database and still answered on the requester's next ask --
 * the pre-existing behaviour -- and the overflow is counted, never silent.
 * ===========================================================================
 */
#ifndef VMS_DLM_PENDING_H
#define VMS_DLM_PENDING_H

#include "vms_cluster.h"            /* the fixed-width types */
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */

/*
 * HOW MANY UNANSWERED CROSS-NODE REQUESTS ONE MASTER HOLDS AT ONCE. An OVMX
 * DESIGN VALUE, not a VMS one: no published limit exists. Sixteen is above
 * anything this cluster has produced (the lab's contention scenarios queue one
 * or two) and it bounds the table at 16 x 132 bytes, which a VAX's non-paged
 * pool can carry without a thought.
 */
#define VMS_DLM_PENDING_MAX 16u

struct vms_dlm_pending_slot {
	uint32_t csid;                      /* the requester's CSID          */
	uint32_t req_lkid;                  /* its own handle, body[24:28]   */
	uint32_t len;                       /* bytes of body actually held   */
	uint8_t  used;
	uint8_t  body[VMS_CM_BODY_LEN];     /* the frame it really sent us   */
};

struct vms_dlm_pending {
	struct vms_dlm_pending_slot slot[VMS_DLM_PENDING_MAX];
	uint32_t kept;        /* requests retained                           */
	uint32_t replaced;    /* a re-send of a request already held         */
	uint32_t overflow;    /* no slot: answered on the next ask instead   */
	uint32_t dropped;     /* retained requests released by an answer     */
};

/* Clear the table. Safe on NULL. */
void vms_dlm_pending_init(struct vms_dlm_pending *p);

/*
 * vms_dlm_pending_keep - remember one unanswered request.
 *
 * `csid` and `req_lkid` identify the requester and ITS OWN lock handle, which
 * together are what the engine queues the lock under. A second copy of the same
 * (csid, req_lkid) REPLACES the first (a retransmit is the same request, and
 * the newest bytes are the ones to echo).
 *
 * Returns 1 when it is held, 0 when there was no room (counted in `overflow`;
 * the caller's behaviour is unchanged by that -- see the header note).
 * A NULL/short body or a zero req_lkid is refused: there is nothing to echo,
 * and a zero handle is the one value this protocol's lock ids never take.
 */
int vms_dlm_pending_keep(struct vms_dlm_pending *p, uint32_t csid,
			 uint32_t req_lkid, const uint8_t *body, uint32_t len);

/*
 * vms_dlm_pending_take - hand back the body kept for (csid, req_lkid) and FREE
 * the slot, because the caller is about to answer it.
 *
 * Returns the number of bytes written to `out` (never more than `cap`), or 0
 * when nothing is held for that pair -- in which case the caller must stay
 * silent rather than invent a frame (INV-6).
 */
uint32_t vms_dlm_pending_take(struct vms_dlm_pending *p, uint32_t csid,
			      uint32_t req_lkid, uint8_t *out, uint32_t cap);

/*
 * vms_dlm_pending_drop - forget what is held for (csid, req_lkid) without
 * reading it: the answer went out some other way (the requester asked again and
 * the idempotent path answered), or the lock is gone. Returns 1 if a slot was
 * freed.
 */
int vms_dlm_pending_drop(struct vms_dlm_pending *p, uint32_t csid,
			 uint32_t req_lkid);

/*
 * vms_dlm_pending_forget_system - free every slot held for `csid`. A system
 * that has left the cluster is owed nothing, and holding its frames would be
 * holding a dead conversation open. Returns the number freed.
 */
uint32_t vms_dlm_pending_forget_system(struct vms_dlm_pending *p,
				       uint32_t csid);

/* How many slots are in use right now (for the diagnostics). */
uint32_t vms_dlm_pending_held(const struct vms_dlm_pending *p);

#endif /* VMS_DLM_PENDING_H */
