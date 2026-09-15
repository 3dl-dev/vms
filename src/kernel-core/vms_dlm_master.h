/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_master.h - the MASTER-side door onto the lock engine, typed in
 * kernel-core's own vocabulary (rd vms-1ee / vms-c27; design
 * docs/design-faithful-cluster-executive.md §3.6, §3.2 "vms_dlm_scs.c delivers
 * inbound requests to vms_lock_dlm_xnode_dispatch AS A DIRECT CALL -- no
 * ioctl").
 *
 * WHY THIS HEADER EXISTS, AND WHY IT IS NOT vms_dlm_proxy.h. vms_dlm_proxy.h is
 * the REQUESTER seam: this node asking somebody else. This is the other half --
 * somebody else asking THIS node, as the resource's master. The engine already
 * has that path in full (vms_lock_dlm_xnode_dispatch), but it is reached through
 * `struct vms_dlm_xnode_args` and a `struct vms_proc *`, both of which live in
 * the per-substrate vms_ioctl.h / vms_internal.h twins that a kernel-core
 * cluster TU may not include (design §3.9, the include gate's RULE 2). So the
 * wire arm reaches it through the structs below, and vms_lock.c -- the one
 * translation unit that sees both vocabularies -- does the translation. Exactly
 * the pattern vms_dlm_proxy.h's `struct vms_dlm_proxy_grant` already established
 * for the requester-side grant receive.
 *
 * ===========================================================================
 * THE DELIVERY PROC (rd vms-c27, RULED)
 *
 * A lock the cluster asks this node to grant has to be OWNED by some process:
 * the engine's LKB carries a `struct vms_proc *`, its per-process lock list is
 * how $GETLKI and rundown find locks, and a lock owned by nobody is a lock
 * nothing can account for. The ruling names the CLUSTER_START-issuing process
 * (STARTUP.EXE, process-permanent) as that owner, and binds it with four
 * conditions. Three of them are visible in this header:
 *
 *   1. ACCESS MODE. The delivery proc is the OWNER, NOT the MODE SOURCE. There
 *      is deliberately NO access-mode field below: the engine stamps a
 *      cross-node LKB PSL_C_KERNEL (process-permanent, outside every local
 *      image's rundown scope) and never `proc->current_mode`, so a local image
 *      rundown on this node cannot release a grant another node still holds.
 *      The REQUESTER's own access mode is an honest omission -- no grounded
 *      cat-0x02 field carries it (INV-6). Teeth:
 *      tests/cluster/host/test_lock_host.c.
 *   2. TAGGING. `req_csid` and `req_lkid` are REQUIRED, not optional: the LKB
 *      the engine creates is stamped with both, so a lock-database read
 *      (GET_RESMASTER.remote_holder_csid, GET_GRANTED) names the TRUE remote
 *      owner rather than the delivery proc.
 *   4. NO FABRICATION. With no delivery proc registered, a request is REFUSED
 *      (VMS_DLM_MASTER_REFUSED) and counted -- never served from thin air,
 *      never granted against a process that does not exist.
 *
 * (Condition 3, per-CSID cleanup when a member departs, is its own rung --
 * rd vms-4d3. `vms_lock_dlm_member_departed` below is the door it will grow
 * into; today it does what the departure path has always done and no more,
 * which is stated rather than implied.)
 * ===========================================================================
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_DLM_MASTER_H
#define OVMX_VMS_DLM_MASTER_H

#include "vms_wire_types.h"   /* fixed-width types, substrate-selected once */
#include "vms_dlm_proxy.h"    /* VMS_DLM_VALBLK_LEN, the shared post vocabulary */

/* ==========================================================================
 * 1. THE DELIVERY PROC
 * ========================================================================== */

/*
 * Register / clear the process that OWNS the master-side LKBs this node creates
 * on behalf of remote requesters. `proc` is a `struct vms_proc *` carried as
 * void * because this header is substrate-free (the same idiom
 * vms_cnxman.h's `vms_cnxman_cluevt_set` already uses); vms_lock.c casts it
 * back. NULL clears the registration, and then every inbound request is refused
 * honestly rather than served against a process that has gone.
 *
 * Called from VMS_IOCTL_CLUSTER_START, with that ioctl's own caller.
 */
void vms_lock_dlm_set_delivery_proc(void *proc);

/* Is one registered? The arm's condition-4 gate reads this before it asks the
 * engine for anything, so a refusal is reported as a refusal (and counted)
 * rather than surfacing as a mysterious engine error. */
int vms_lock_dlm_have_delivery_proc(void);

/* ==========================================================================
 * 1b. THIS NODE'S CLUSTER IDENTITY, AS THE LOCK ENGINE HOLDS IT
 *
 * The engine reads one cluster global, `vms_local_csid`: it is what
 * GET_RESMASTER reports as this node's CSID, what an outbound cross-node
 * request would name as the REQUESTER, and what "is this master us?" compares
 * against. Its definition lives in each substrate's module rind, and its
 * initial value there is an insmod PLACEHOLDER (1) whose own comment says the
 * real CSID is "assigned by the connection manager at cluster join" -- a
 * binding that was never made.
 *
 * MEASURED ON THE 2-NODE RIG (rd vms-1ee): with both executives MEMBERs of a
 * real cluster holding CSIDs 0x00010001 and 0x00010002, GET_RESMASTER reported
 * local_csid=0x00000001 on BOTH. The lock engine did not know who it was, and
 * an outbound request would have asserted CSID 1 on the wire from every node --
 * a fabricated identity (INV-6), and the exact "placeholder that reads like
 * data" class that bugchecked a real VAX.
 *
 * So the DLM's wire arm SYNCS it from the CLUB, which is where the cluster's
 * own assignment lives (cl->club.local_csid, valid only when
 * local_csid_valid). A zero or unlearned CSID does NOT overwrite anything: an
 * identity the cluster has not assigned yet is an identity this node does not
 * have, and the standalone placeholder stays exactly as it was.
 * ========================================================================== */
void vms_lock_dlm_set_local_csid(uint32_t csid);

/* What the engine currently believes, for the same readback discipline. */
uint32_t vms_lock_dlm_local_csid(void);

/* ==========================================================================
 * 2. One inbound request, as the master sees it
 *
 * Every field is read by the wire arm out of a RECEIVED frame through codec
 * accessors, or out of the connection manager's own identification of the
 * sender. Nothing here is defaulted: a field the frame did not carry is not in
 * this struct.
 * ========================================================================== */

#define VMS_DLM_MREQ_ENQ     VMS_DLM_POST_ENQ       /* cat-0x02 op 0x01 */
#define VMS_DLM_MREQ_CONVERT VMS_DLM_POST_CONVERT   /* cat-0x02 op 0x07 */
#define VMS_DLM_MREQ_DEQ     VMS_DLM_POST_DEQ       /* the release       */

struct vms_dlm_master_request {
	uint32_t op;           /* VMS_DLM_MREQ_*                              */
	/*
	 * WHO IS ASKING -- the frame's own cluster-logical source address as
	 * the CONNECTION MANAGER identified it (spec §4(a)), never a body
	 * field. 0 is refused: a request from a system this node cannot name
	 * is a request it cannot tag a lock with (condition 2).
	 */
	uint32_t req_csid;
	uint32_t req_lkid;     /* the requester's OWN handle, body[20:24]     */
	uint32_t master_lkid;  /* OUR handle, body[24:28] -- a DEQ names it   */
	uint32_t lkmode;       /* body[30], LCK$K_ encoding                   */
	uint32_t flags;        /* LCK$M_ the requester supplied               */
	char     resnam[32];   /* body[48..], the root resource name          */

	/* The lock value block the releaser wrote, when it said it wrote one.
	 * `valblk_present` 0 means the frame carried none, and the master's
	 * own block is then left ALONE -- sixteen zeros presented as an LVB is
	 * a placeholder that reads exactly like data (INV-6). */
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];
	uint8_t  valblk_present;
	uint8_t  pad[3];
};

/* ==========================================================================
 * 3. What the master DID -- named as a FACT, not as an SS$_ code
 *
 * The SS$_ vocabulary lives in the per-substrate twin; this side of the seam
 * names the outcome, exactly as vms_dlm_ldwv.h and vms_dlm_scs_fsm.h do, and
 * the arm turns it into a wire shape.
 * ========================================================================== */
enum vms_dlm_master_outcome {
	VMS_DLM_MASTER_GRANTED = 0, /* on a real granted queue, right now     */
	VMS_DLM_MASTER_QUEUED,      /* genuinely queued; a later GRANT follows*/
	VMS_DLM_MASTER_DENIED,      /* NOQUEUE and incompatible -- SS$_NOTQUEUED */
	VMS_DLM_MASTER_REDIRECT,    /* not our tree; the master we DO hold    */
	VMS_DLM_MASTER_RELEASED,    /* a DEQ really released a lock           */
	VMS_DLM_MASTER_REFUSED      /* the honest floor: we cannot serve it   */
};

struct vms_dlm_master_result {
	uint8_t  outcome;            /* enum vms_dlm_master_outcome           */
	uint8_t  granted_mode;       /* GRANTED only, read off the LKB        */
	uint8_t  pad[2];

	/* GRANTED/QUEUED: the handle the ENGINE minted for this lock. It is a
	 * real LKB's lock id -- never a counter, never a placeholder (the
	 * fc8540ae INVLOCKID lesson). */
	uint32_t master_lkid;

	/*
	 * GRANTED: the REQUESTER's own handle, READ BACK OFF THE LKB THE ENGINE
	 * JUST STAMPED -- not echoed from the request. The grant reply's
	 * body[20] is this value, and sourcing it from the lock database is
	 * what makes that field an executive read rather than a byte carried
	 * from one frame to the next. It must equal the request's `req_lkid`;
	 * if the engine ever recorded something else, the reply says what the
	 * engine RECORDED, because that is the lock that exists.
	 */
	uint32_t req_lkid;

	/* REDIRECT: the master this node genuinely holds for the tree (Davis
	 * p. 6-31 outcome 2). 0 never appears here -- "unmastered" is what 0
	 * means in the engine, so it is not an answer. */
	uint32_t redirect_csid;

	/*
	 * QUEUED: the cross-node HOLDER this request is blocked behind, and
	 * that must therefore receive a blocking AST. All three are read off
	 * the blocking LKB. 0 when nothing blocks it across nodes.
	 */
	uint32_t blocking_csid;
	uint32_t blocking_master_lkid;
	uint32_t blocking_req_lkid;

	/*
	 * RELEASED: did this release FLIP a queued cross-node waiter to
	 * granted? Then the master owes THAT requester a deferred GRANT, and
	 * these name it -- its CSID, its own handle, our handle for it, and
	 * the mode it now holds. `deferred_grant` 0 means nothing flipped.
	 */
	uint8_t  deferred_grant;
	uint8_t  deferred_mode;
	uint8_t  pad2[2];
	uint32_t deferred_csid;
	uint32_t deferred_req_lkid;
	uint32_t deferred_master_lkid;

	/*
	 * GRANTED, THE LVB READ CROSSING (vms-727). The master RESOURCE's current
	 * value block, read off the RSB after the grant. `valblk_present` is 1
	 * ONLY when that block is non-zero -- the master then returns it in the
	 * grant reply so the requester's $ENQ(VALBLK)/$GETLKI reads it back. A
	 * resource with no block leaves this 0 and the reply carries none:
	 * sixteen zeros presented as an LVB is the placeholder INV-6 forbids.
	 */
	uint8_t  valblk_present;
	uint8_t  valblk_pad[3];
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];
};

/*
 * SERVE one inbound request as the resource's master.
 *
 * Returns 0 when `*out` describes what the engine really did, and non-zero when
 * the request was malformed (a null argument, no requester CSID, no requester
 * handle, an empty resource name) -- in which case NOTHING was done and nothing
 * may be answered. `VMS_DLM_MASTER_REFUSED` is the other honest floor and it IS
 * a 0 return: the engine was asked and declined, which the arm counts and
 * reports rather than retrying.
 *
 * The LKB this creates is owned by the DELIVERY PROC, stamped with `req_csid`
 * and `req_lkid`, and held at PSL_C_KERNEL -- see this header's own delivery-proc
 * note, and vms_lock.c's comment at the acmode stamp.
 */
uint32_t vms_lock_dlm_master_serve(const struct vms_dlm_master_request *r,
				   struct vms_dlm_master_result *out);

/*
 * The op-0x06 CONVERT-with-VALBLK RECEIVE half (vms-727): replicate a remote
 * holder's flushed value block into the MASTER resource it named. Authorized by
 * cluster identity exactly like the DEQ master-serve -- the block is written
 * only into a lock this node holds FOR `req_csid`; a local lock, a lock held for
 * another CSID, or a proxy LKB is refused SS$_IVLOCKID. Returns SS$_NORMAL on a
 * real write, an error otherwise. Does not release or re-queue the lock.
 */
uint32_t vms_lock_dlm_master_apply_valblk(uint32_t req_csid, uint32_t master_lkid,
					  const uint8_t *valblk);

/* ==========================================================================
 * 4. A member left
 *
 * The kernel-core door onto the departure sweep VMS_IOCTL_DLM_MEMBER_DEPART
 * already performs: drop every cached directory answer (the vector they came
 * from is being rebuilt, Davis p. 6-33), un-master what the departed node
 * mastered, and end -- honestly -- the proxy waits that node was the only
 * possible answer to. `*found` (optional) reports whether it really mastered
 * anything here: a real observation, not a lookup in a configured list.
 *
 * WHAT IT DOES NOT DO, STATED RATHER THAN IMPLIED: it does not yet RELEASE the
 * master-side LKBs this node holds FOR the departed CSID. That is rd vms-4d3
 * (vms-c27 condition 3), a rung of its own with its own proof, and claiming it
 * here would be claiming a cleanup the executive does not perform.
 * ========================================================================== */
void vms_lock_dlm_member_departed(uint32_t departed_csid, uint32_t *found);

#endif /* OVMX_VMS_DLM_MASTER_H */
