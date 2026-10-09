/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_echo_guard.h - THE ECHO GUARD: this node will not answer the same
 * lock request from the same system the same way without end.
 *
 * rd vms-b5b0. Substrate-portable kernel-core: no backend, no time, no
 * allocation, no globals -- a pure decision over bytes the caller already has.
 *
 * WHY IT EXISTS, MEASURED. In the 2026-10-09 evacuation lab an OVMX master's
 * grant carried the two lock handles in each other's slots, so the requesting
 * VAX could not correlate the completion to its own lock. It re-sent one
 * op-0x01 ENQ 65,356 times in 63.7 s (1026/s, median gap 0.362 ms) and OVMX
 * answered 65,340 of them with identical bytes. Neither executive stopped it;
 * an operator did. A peer melting under a frame we keep handing it is the
 * never-crash-a-peer class whether or not it bugchecks.
 *
 * The underlying bug is fixed (the codec reproduces a real master's grant
 * byte-for-byte now). This guard is the TEETH that makes the FAILURE MODE
 * self-limiting whatever causes it next: a reply loop is a loop whether the
 * repeated answer is a grant, a deny or a directory answer.
 *
 * THE RULE, and it is about SAMENESS rather than rate: a reply is admitted
 * while the (requester, request bytes, reply bytes) triple keeps CHANGING. Once
 * that exact triple repeats more than VMS_DLM_ECHO_MAX_SAME times in a row,
 * this node STOPS ANSWERING that requester's identical request -- counted, and
 * the caller says so once. ANY change -- a different request, a different
 * answer, a different requester -- resets the run, so ordinary traffic (a
 * retransmit answered identically a few times; a rebuild burst of hundreds of
 * DIFFERENT records) is untouched.
 *
 * WHY SAMENESS AND NOT A RATE LIMIT. A rate limit has to guess what "too fast"
 * is for a VAX on a quiet LAN, and guesses about peers are how you strand one.
 * "I have told you this nine times and nothing has changed" needs no guess: it
 * is a loop by definition, and the honest act is to stop talking.
 *
 * WHAT IT IS NOT: not a membership decision, not a quorum input, not a reason
 * to drop a connection. The requester is left alone -- its own retransmit
 * ladder and its own timeout decide what happens next, exactly as they would
 * if this node had crashed. Refusing to answer is the one action here that
 * asserts nothing about another system's state (INV-6).
 */
#ifndef VMS_DLM_ECHO_GUARD_H
#define VMS_DLM_ECHO_GUARD_H

/* The fixed-width types, the way every pure cluster header here gets them:
 * vms_cluster.h selects <stdint.h> on the host and the substrate's own on a
 * kernel (OVMX_CLUSTER_HOST), so this TU stays substrate-portable. */
#include "vms_cluster.h"

/*
 * THE BOUND IS AN OVMX DESIGN VALUE, not a VMS one: no published VMS limit
 * exists, and this is not a VMS algorithm being reproduced (Rule 8). Eight sits
 * above every retransmit ladder observed on this cluster's wire (a real VAX
 * re-sends a handful of times, seconds apart) and three orders of magnitude
 * below a storm.
 */
#define VMS_DLM_ECHO_MAX_SAME 8u

/*
 * One slot per conversing system. A storm is ONE peer repeating ONE request,
 * so a small fixed table is enough -- and fixed is the point: no allocation on
 * a receive path, and no unbounded state a peer could grow by varying its
 * frames. When every slot is live the one with the SHORTEST run is reused: the
 * slots that matter are the ones with a run building towards the bound.
 */
#define VMS_DLM_ECHO_SLOTS 8u

/*
 * THE MOST BYTES THIS GUARD WILL EVER READ from either buffer. A DLM SYSAP body
 * is 132 bytes, and this is the only loop on the receive path with a
 * CALLER-SUPPLIED bound, so it is capped here rather than trusted: a length
 * that ever arrived wrong would otherwise be an unbounded loop on the fork
 * thread -- which on Linux is an RCU stall and on NetBSD a wedged softint, in
 * the one place a peer's frame controls the count. Comparing the first 256
 * bytes is as good a sameness test as comparing all of them.
 */
#define VMS_DLM_ECHO_SIG_MAX 256u

struct vms_dlm_echo_slot {
	uint32_t from;          /* the requester's CSID                      */
	uint32_t req_sig;       /* private signature of the request's bytes   */
	uint32_t ans_sig;       /* ... and of the answer we staged for it     */
	uint32_t run;           /* consecutive identical (req, ans) answers   */
	uint8_t  used;
};

struct vms_dlm_echo_guard {
	struct vms_dlm_echo_slot slot[VMS_DLM_ECHO_SLOTS];
	uint32_t refused;       /* answers withheld by this guard             */
	uint32_t runs_capped;   /* distinct conversations that hit the bound  */
};

/* Clear the guard. Safe on a NULL pointer. */
void vms_dlm_echo_guard_init(struct vms_dlm_echo_guard *g);

/*
 * vms_dlm_echo_admit - may this answer go out?
 *
 * @g:      the guard's state (NULL admits: a guard that is not there cannot
 *          refuse a reply -- this is a safety net, never a gate on service)
 * @from:   the requester's CSID
 * @req:    the request's body bytes, as received (NULL/0 admits: with nothing
 *          to compare there is no loop to see). Lengths are capped at
 *          VMS_DLM_ECHO_SIG_MAX; no caller's number sets a loop bound here.
 * @ans:    the answer's body bytes, as staged
 *
 * Returns 1 to send, 0 to withhold. On the transition to withholding,
 * `*first_cap` (when non-NULL) is set to 1 exactly once per conversation, so
 * the caller can say it on the console without repeating itself 65,000 times.
 *
 * The signatures are PRIVATE: computed here, never sent, never compared against
 * another system's value, never written into a frame.
 */
int vms_dlm_echo_admit(struct vms_dlm_echo_guard *g, uint32_t from,
		       const uint8_t *req, uint32_t req_len,
		       const uint8_t *ans, uint32_t ans_len,
		       uint8_t *first_cap);

#endif /* VMS_DLM_ECHO_GUARD_H */
