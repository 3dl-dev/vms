/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_scs.h - the lock manager's DISTRIBUTED ARM: the role operations that
 * bind vms_lock.c's real lock database to the cluster's `VMS$VAXcluster`
 * connection (FC-P0.1).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.6 (three roles, the
 * Rule-8 directory question, D-DLM-1..5), SS3.2 (vms_dlm_scs.c "delivers inbound
 * requests to vms_lock_dlm_xnode_dispatch AS A DIRECT CALL -- no ioctl").
 *
 * THE LAYER IN ONE PARAGRAPH. vms_lock.c is, and stays, the lock ENGINE: modes,
 * conversion, LVB, blocking ASTs, deadlock search, and the master-side receive
 * path that is already real. This layer is its WIRE ARM. It marshals the
 * engine's three roles -- requester, master, directory node -- to and from cat-02
 * frames, and it owns the rebuild FSM's wire side. It holds no lock state of its
 * own: every value it puts on the wire is read out of an LKB or an RSB at the
 * moment it builds the frame.
 *
 * ===========================================================================
 * THE TWO RULES THAT DEFINE THIS INTERFACE
 *
 * RULE A -- A REPLY NEVER LEAVES BY ITSELF. Look at `struct dlm_scs_role_ops`
 * below: it has no `send`. An inbound request is answered by FILLING A REPLY
 * BUFFER the connection manager supplied, and the connection manager sends it on
 * the same connection, correlated with the transaction it answers. This is not
 * style. A reply emitted outside that correlated response machinery arrives at a
 * real VAX without the request's transaction and sequence correlation, the VAX
 * rejects it as ungrounded, the requester retries, and the result is a retry
 * storm that looks like throughput. The shape of this interface makes that
 * failure unrepresentable: this layer cannot send a grant, so it cannot send an
 * ungrounded one.
 *
 * RULE B -- EVERY FIELD COMES FROM A REAL OBJECT (INV-6). A grant is built from
 * a LKB that is actually on a granted queue: the mode comes off that queue, the
 * master handle IS that LKB's lock id, the value block IS the RSB's. A
 * completion is sent only from a proxy LKB that holds a master handle it
 * received. A request for a resource this node neither masters nor is directory
 * for is DECLINED -- the honest answer -- never granted. A placeholder lock id
 * bugchecked a real VAX (INVLOCKID) and a corrupted echoed resource name
 * produced LOCKMGRERR; both were frames with no object behind them.
 * ===========================================================================
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_DLM_SCS_H
#define OVMX_VMS_DLM_SCS_H

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"

struct vms_dlm_scs;

/* ==========================================================================
 * 1. The three roles
 *
 * Public model (IDSM lock-management chapter; Davis ch. 7): for a $ENQ on
 * resource R -- (1) R is already mastered locally, or (2) this node is R's
 * DIRECTORY node and consults its directory (becoming master if there is none),
 * or (3) the request goes to R's directory node, which answers with the master,
 * and the lock request then goes to that master.
 * ========================================================================== */
enum dlm_role {
	DLM_ROLE_REQUESTER = 0,  /* holds a proxy LKB for a lock mastered elsewhere */
	DLM_ROLE_MASTER    = 1,  /* owns the RSB and grants */
	DLM_ROLE_DIRECTORY = 2,  /* stores resource -> master CSID for others */
	DLM_ROLE__COUNT
};

/* ==========================================================================
 * 2. THE DIRECTORY RESOLVER -- the Rule-8 boundary, RESOLVED (FC-P4.3)
 *
 * "Which member is the directory node for this ROOT resource?" -- and it is
 * answered in src/kernel-core/vms_dlm_ldwv.h, which is where the whole
 * contract, the page cites and the ladder's outcome now live. Read that file;
 * this note records only WHY there is no `dir_resolve` function pointer here.
 *
 * WHAT THE LADDER RESOLVED TO (design SS3.6 D-DLM-2, research note
 * docs/research-dlm-directory-algorithm.md):
 *
 *   RUNG A, taken.  The Lock Directory Weight Vector and the `hash mod n`
 *     index rule are FULLY PUBLISHED (Davis pp. 6-31/6-32, Fig. 6-18 p. 6-33,
 *     pp. 7-40..7-42), so OVMX implements them from the book. The vector lives
 *     in the CLUB, is resized at Phase 1 and filled at Phase 2, and every
 *     change discards all cached directory information.
 *
 *   RUNG A', taken.  The hash FUNCTION is not published at the bit level, so
 *     OVMX never computes one. It does not need to: p. 6-50 documents that
 *     every directory lookup carries the SENDER'S 16-bit hash on the wire and
 *     that the directory node uses the received value. OVMX learns it
 *     (vms_lock.c `hash16`/`hash_known`, fed from the codec's
 *     vms_dlm_dir_hash_parse) and sends a lookup ONLY with a value it received.
 *     A root name OVMX is the first in the cluster to touch has no hash and is
 *     refused SS$_UNSUPPORTED -- the honest floor, and a narrow one.
 *
 *   RUNG B (probe) is expected UNSAFE and is NOT built; rung C(i) is an
 *     operator matter. Neither is a code path here.
 *
 * WHY NO FUNCTION POINTER IN THIS HEADER. The resolver used to be typed
 * `(resnam, namelen) -> csid`, and an interface that takes a NAME is an
 * interface somebody can implement by hashing the name -- which is exactly the
 * thing that broke a real cluster (commit 90b3bbbd) and later produced the
 * grant storm. The seam the lock engine actually uses takes the HASH
 * (src/kernel-core/vms_dlm_proxy.h `dir_resolve(ctx, hash16, &csid)`), so the
 * fabrication is not merely forbidden, it is unrepresentable.
 * ========================================================================== */

/* ==========================================================================
 * 3. An inbound request and the reply it may produce
 *
 * `body` is the frame body as received; this layer reads it ONLY through codec
 * accessors (design SS3.9 rule 2: no raw byte offsets outside the codec -- two
 * crashes came from body[N] arithmetic in orchestration code).
 * ========================================================================== */
struct dlm_scs_request {
	vms_csid_t     from_csid;    /* the sender, as the CM identified it */
	vms_scs_sysid_t from_sysid;
	uint8_t        category;     /* SCA category, e.g. 0x02 */
	uint8_t        opcode;       /* e.g. 0x01 ENQ/lookup, 0x0d rebuild record */
	uint8_t        pad[2];
	const uint8_t *body;
	uint32_t       len;
};

struct dlm_scs_reply {
	uint8_t *body;   /* buffer OWNED BY THE CONNECTION MANAGER (see RULE A) */
	uint32_t cap;    /* its capacity */
	uint32_t len;    /* bytes written; 0 == no response, the honest silence */
};

/* ==========================================================================
 * 4. The role ops -- what the connection manager calls INTO this layer
 *
 * Note the absence of a `send`: see RULE A.
 * ========================================================================== */
struct cnxman_transition;   /* vms_cnxman.h; opaque here on purpose */

struct dlm_scs_role_ops {
	/*
	 * A transition has begun: FREEZE lock activity that would cross the
	 * wire. Called on the fork thread before the barrier runs.
	 */
	void (*transition_begin)(void *ctx, const struct cnxman_transition *tr);

	/*
	 * One cat-02 message arrived on the VMS$VAXcluster connection. Answer it
	 * from REAL lock state (RULE B) by filling `reply`, or leave reply->len
	 * at 0 for the honest silence. Returns 0 when the request was handled
	 * (with or without a reply) and an SS$_ status when it was DECLINED --
	 * declines are counted in the snapshot, never hidden.
	 */
	int  (*handle_request)(void *ctx, const struct dlm_scs_request *req,
			       struct dlm_scs_reply *reply);

	/*
	 * The transition finished. `completed` is nonzero if the barrier ran to
	 * its end; zero means it was abandoned and the rebuild must unwind.
	 * THAW happens here.
	 */
	void (*transition_end)(void *ctx, const struct cnxman_transition *tr,
			       int completed);

	/*
	 * A member left: remaster what it mastered, invalidate directory
	 * entries that named it, and fail proxy LKBs whose master is gone. The
	 * engine already has this path (the DLM_MEMBER_DEPART lineage); this is
	 * how the connection manager reaches it as a direct call rather than
	 * through an ioctl.
	 */
	void (*member_departed)(void *ctx, vms_csid_t csid);

	void *ctx;
};

/* ==========================================================================
 * 5. Lifecycle, the outbound direction, and readback
 * ========================================================================== */

/* Start / stop the DLM's wire arm. Starting registers the role ops with the
 * connection manager and connects the engine's outbound path; stopping restores
 * the engine to purely local operation (which remains fully functional -- a
 * node with no cluster still locks). */
int  vms_dlm_scs_start(struct vms_cluster *cl);
void vms_dlm_scs_stop(struct vms_cluster *cl);

/*
 * The requester side, called BY vms_lock.c from process context: post the
 * request that a proxy LKB in `waiting` represents and let the caller sleep on
 * the LKB's condition variable (the existing enq_wait_sync pattern). Returns 0
 * once the request is queued to the fork thread, or an SS$_ status. It does NOT
 * wait: the engine owns the wait, because the engine owns the LKB.
 *
 * `lkid` is the requester's own lock id -- half of the (req_csid, req_lkid)
 * idempotency key every retransmit is matched on (D-DLM-5), so a duplicate
 * request can never mint a second lock.
 */
int vms_dlm_scs_post_request(struct vms_cluster *cl, vms_csid_t dst_csid,
			     uint32_t lkid, uint8_t opcode,
			     const uint8_t *body, uint32_t len);

/* Project this layer's state (INV-6: read from real objects under the fork
 * mutex). */
int vms_dlm_scs_snapshot(struct vms_cluster *cl, struct vms_dlm_scs_view *out);

#endif /* OVMX_VMS_DLM_SCS_H */
