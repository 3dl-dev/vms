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
 * 2. THE DIRECTORY RESOLVER -- the Rule-8 boundary, in one function
 *
 * `dir_resolve(name) -> csid` answers "which member is the directory node for
 * this ROOT resource?" (sub-resources never hash: a whole lock tree is mastered
 * where its root is, so only root names reach here).
 *
 * WHAT THIS FUNCTION MAY NOT DO. It may not compute a hash of its own. OVMX
 * once did exactly that -- exec_jhash(name) % n over a static vector -- and
 * pointing it at a real cluster caused a reformation: a wrong directory is not a
 * local error, it is a cluster-breaker. The DLM directory algorithm is
 * unpublished-as-code and CLAUDE.md Rule 8 forbids recomputing it.
 *
 * WHAT IT MAY DO, in the order the design's ladder tries them (SS3.6, D-DLM-2):
 *   Rung A  implement the algorithm as PUBLISHED -- the lock-management chapter
 *           of VAX/VMS Internals and Data Structures documents the directory
 *           vector (each node with LOCKDIRWT>0 occupying LOCKDIRWT entries,
 *           rebuilt at every transition) and FC-P4.1 answers whether the book
 *           gives the hash and the ordering at the bit level. Implementing a
 *           published description IS clean-room; the lab capture then becomes a
 *           CONFORMANCE CHECK (predicted vs observed over ~100 root names, zero
 *           residuals), not a derivation. This is the expected outcome.
 *   Rung B  never compute -- ASK. Probe W[0], W[1], ... and cache the answer in
 *           the RSB's dir_csid, invalidating every cache at every transition.
 *           Every routing decision is then a value the cluster returned.
 *   Rung C  operator ruling only (FC-P4.3), if the protocol does not guard a
 *           mis-addressed lookup.
 *
 * The three rungs are one function behind one signature, so choosing among them
 * is a configuration of this call, not a redesign. `out_csid` is written ONLY
 * when the answer is real; a nonzero return means "not resolved" and the caller
 * must not proceed with a guess.
 * ========================================================================== */
typedef int (*dlm_dir_resolve_fn)(void *ctx, const uint8_t *resnam,
				  uint8_t namelen, vms_csid_t *out_csid);

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

/* Install the directory resolver (SS2). Until one is installed this node
 * resolves only resources it masters itself; a lookup that would need a
 * directory is DECLINED rather than guessed. */
void vms_dlm_scs_set_dir_resolve(struct vms_cluster *cl,
				 dlm_dir_resolve_fn fn, void *ctx);

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
