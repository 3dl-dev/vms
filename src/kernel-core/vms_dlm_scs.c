// SPDX-License-Identifier: GPL-2.0
/*
 * vms_dlm_scs.c - THE LOCK MANAGER'S WIRE ARM (plan item FC-P4.8, the glue;
 * rd vms-1ee blocker 2). Design docs/design-faithful-cluster-executive.md §3.6
 * and §3.2 ("vms_dlm_scs.c delivers inbound requests to
 * vms_lock_dlm_xnode_dispatch AS A DIRECT CALL -- no ioctl").
 *
 * WHAT THIS FILE IS. Everything around it already existed and was already
 * proven; this is the binding that makes the pieces one path:
 *
 *   the ENGINE          src/kernel-core/vms_lock.c -- modes, queues, LVB,
 *                       blocking ASTs, the master-side receive. Untouched here.
 *   the REQUESTER SEAM  vms_dlm_proxy.h -- how a $ENQ for a remote-mastered
 *                       resource leaves the engine (and how its answer gets
 *                       back in).
 *   the MASTER SEAM     vms_dlm_master.h -- how a peer's request reaches the
 *                       engine, owned by the DELIVERY PROC (rd vms-c27).
 *   the REQUESTER FSM   vms_dlm_scs_fsm.c -- pure, table-driven, no globals.
 *   the CODEC           vms_cluster_codec_dlm.c -- the ONLY path to a cat-0x02
 *                       byte.
 *   the CONNECTION MGR  vms_cnxman.c -- the VMS$VAXcluster connection, the
 *                       CSBs, the envelope, and the transition boundaries.
 *
 * This file holds NO lock state, NO wire offset and NO clock. It owns one
 * request table (inside the FSM), a reply scratch, and counters.
 *
 * ===========================================================================
 * THE THREE RULES THIS FILE IS WRITTEN AGAINST
 *
 * RULE A -- A REPLY NEVER LEAVES BY ITSELF (vms_dlm_scs.h). There is no send
 * of a reply here: `handle_request` FILLS a buffer the connection manager
 * owns, and the connection manager sends it correlated with the transaction it
 * answers. An uncorrelated reply is rejected by a real VAX and the requester
 * retries -- the retry storm that looks like throughput.
 *
 * RULE B -- EVERY ASSERTED FIELD IS AN EXECUTIVE READ (INV-6). A grant reply's
 * master handle, granted mode and requester handle all come back out of the LKB
 * the engine just stamped (vms_dlm_master.h `struct vms_dlm_master_result`), not
 * out of the request that produced it. A request frame is built from a FRESH
 * `refill_post` every single time (the FSM's own discipline). A placeholder lock
 * id bugchecked a real VAX with INVLOCKID and took the cluster down.
 *
 * RULE C -- NEVER EMIT WHAT COULD BUGCHECK A PEER. DLM traffic is routed ONLY
 * between systems that have PROVED they run this implementation, by advertising
 * a software-version token byte-identical to our own (the E80 identity,
 * `csb->peer_is_ours`). The gate has two halves and both are counted: this file
 * refuses to SERVE a request from a system that is not proven ours, and
 * vms_cnxman.c refuses to EMIT to one. The Lock Directory Weight Vector's own
 * FOREIGN refusal (vms_dlm_ldwv.h) already stops the ROUTING upstream of both;
 * these are the emission-side teeth for anything that gets past it.
 * ===========================================================================
 *
 * WHAT THIS FILE NOW EMITS, AND WHERE IT STILL HONESTLY STOPS (rd vms-d7a3):
 *   - THE BLOCKING AST (op 0x04) is GROUNDED by the vms-c03 capture and is
 *     SENT: when the engine names a remote holder that must be told, this file
 *     builds the frame from THAT holder's LKB (its two real lock ids, no
 *     invented mode context, no resource name) and originates it at the
 *     holder's CSID -- behind the all-OVMX gate and RULE C. Every way it can
 *     fail to go out still raises `blkasts_no_wire_op` with nothing sent. A
 *     BLKAST invented from a GUESSED opcode is how LOCKMGRERR happened; this
 *     one is read off a real cluster's wire and off a real LKB.
 *   - THE RELEASE ($DEQ, op 0x03) is emitted by the REQUESTER arm behind the
 *     same two gates (vms_dlm_scs_fsm.h), and is now also CONSUMED: an inbound
 *     op-0x03 from a proven-OVMX peer reaches the engine's master-side door and
 *     really releases the LKB this node holds for that peer (rd vms-c72, "THE
 *     RELEASE'S RECEIVE HALF" below). An inbound op-0x04 likewise reaches the
 *     requester FSM and fires the holder's REAL blocking AST. What is still
 *     owed and counted rather than sent is the DEFERRED GRANT a release earns
 *     for a queued waiter -- see dlm_arm_count_deferred_grant.
 *   - THE VALUE BLOCK has no grounded cat-0x02 BUILDER (op 0x06's body[32:36]
 *     is unpinned), so a write crossing is not transmitted and an inbound grant
 *     is handed to the engine with `valblk_present = 0`, which makes the engine
 *     leave the proxy's own block alone rather than overwrite it with zeros.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_internal.h"     /* the SS$_ vocabulary (the glue TU idiom) */
#include "exec_kbackend.h"
#include "vms_cluster.h"
#include "vms_cluster_fork.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_cnxman.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_scs.h"
#include "vms_dlm_scs_fsm.h"

/* ==========================================================================
 * 1. The arm
 * ========================================================================== */

/* The requester arm's retransmit beat. The FSM owns the ladder and the
 * deadlines (DLM_REQ_RETRY_MS); this is only how often it is asked. */
#define DLM_ARM_BEAT_MS   500u
#define DLM_ARM_TIMER_BEAT 1u

struct vms_dlm_scs {
	struct vms_cluster          *cl;

	struct dlm_req_fsm           req;      /* the pure requester FSM      */
	struct dlm_req_ops           req_ops;  /* its doors, bound below      */

	/*
	 * THE RELEASE QUEUE (rd vms-49f8) and the lock that makes it a thread
	 * crossing. Everything else this arm posts is REBUILT on the fork
	 * thread from the lock database; a $DEQ cannot be, because the $DEQ is
	 * what destroys the lock block (vms_dlm_scs_fsm.h §12). So a release is
	 * SNAPSHOTTED here in the releaser's own context -- out of the post the
	 * engine just filled from the live LKB -- and the fork thread emits
	 * from that.
	 *
	 * THE LOCK, AND WHY IT IS THIS CLASS AND NOT THE FORK MUTEX. The
	 * stager runs in process context ($DEQ's own thread, and image
	 * rundown's sweep) and the claimer runs on the fork thread, so the
	 * queue needs mutual exclusion of its own. It may NOT be the fork mutex:
	 * design §3.3 forbids a lock-manager path from taking it, which is the
	 * very reason the post is queued rather than sent inline. exec_lock_t is
	 * the engine's own class (res->lock) and the fork thread already nests
	 * it under the fork mutex on every refill, so the order fork mutex ->
	 * this lock is the established one; nothing takes the fork mutex while
	 * holding this, and the critical sections are a struct copy.
	 */
	struct dlm_relq              relq;
	exec_lock_t                  relq_lock;
	struct vms_dlm_requester_ops eng_ops;  /* the engine's door to us     */
	struct dlm_scs_role_ops      role;     /* the CM's door to us         */

	/*
	 * The frame scratch for a reply this node BUILDS as master. The codec's
	 * builders write at FRAME-absolute offsets, so the buffer carries the
	 * envelope span too and the body is `frame + VMS_OFF_SYSAP_BODY` -- the
	 * same splice the requester FSM uses from the other side, and the reason
	 * no offset arithmetic appears anywhere else in this file.
	 */
	uint8_t  txframe[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];

	/* Counted facts, every one of them a thing that really happened. */
	uint32_t req_received;        /* inbound cat-0x02 requests dispatched */
	uint32_t replies_received;    /* inbound cat-0x82 replies             */
	uint32_t grants_sent;         /* grant replies built from a real LKB  */
	uint32_t denies_sent;
	uint32_t queued_no_reply;     /* genuinely queued: the grant comes later */
	uint32_t redirects_sent;      /* "the master is X", from a real RSB   */
	uint32_t blkasts_sent;        /* op-0x04 blocking ASTs really emitted,*/
				       /* built from the blocking LKB's own two*/
				       /* lock ids (rd vms-d7a3)              */
	uint32_t releases_received;   /* op-0x03 $DEQs that really RELEASED a */
				       /* master-side LKB of ours (rd vms-c72) */
	uint32_t valblk_writes_received; /* op-0x06 CONVERTs that really wrote */
				       /* a master resource's value block (727)*/
	uint32_t declined;            /* the honest floor, per vms_dlm_scs.h  */

	/* The refusals. Each one is a place this file will not fabricate. */
	uint32_t foreign_refused;     /* RULE C: the sender is not proven ours*/
	uint32_t no_delivery_proc;    /* condition 4: nobody to own the LKB   */
	uint32_t blkasts_no_wire_op;  /* a holder we cannot honestly notify:   */
				       /* off-gate, no route, RULE C, or a lock*/
				       /* id the codec refuses. Nothing sent.  */
	uint32_t releases_refused;    /* an inbound $DEQ naming no lock this   */
				       /* node holds FOR THAT SENDER. Nothing  */
				       /* released, nothing answered.          */
	uint32_t deferred_grants_no_wire_op; /* a release really flipped a     */
				       /* queued cross-node waiter to granted, */
				       /* and this master has no grounded way  */
				       /* to ORIGINATE that grant (RULE A).    */
	uint32_t codec_failures;
	uint32_t unparsed;

	/* The engine's posts, at each of the three places one can end. */
	uint32_t posts_queued;        /* handed to the fork thread            */
	uint32_t posts_unqueued;      /* the fork queue would not take it     */
	uint32_t posts_lock_gone;     /* the proxy was released in between    */
	uint32_t posts_refused;       /* the FSM refused: nothing was sent    */

	/* The RELEASE path's own three (rd vms-49f8). A release is staged, not
	 * rebuilt, so it fails in different places than a request does. */
	uint32_t releases_staged;     /* snapshotted from a live proxy LKB    */
	uint32_t releases_no_slot;    /* the queue was full: NOTHING staged,  */
				       /* and the releaser is told so          */
	uint32_t releases_stale;      /* a work item naming no staged release */

	uint32_t transitions_begun;
	uint32_t transitions_ended;
	uint32_t members_departed;
};

/* ==========================================================================
 * 2. The CLUB reads this arm makes -- all of them through cnxman, none of them
 *    re-derived here.
 * ========================================================================== */

/* THE DIRECTORY. `hash16` is a value some system in this cluster put on the
 * wire for the name (Davis p. 6-50) and that the engine learned; this resolves
 * it through the connection manager's weight vector and nothing else. 0 in
 * *out means THIS node (p. 6-32). */
static int dlm_arm_dir_resolve(void *ctx, uint16_t hash16, vms_csid_t *out_csid)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	if (d == NULL || d->cl == NULL || out_csid == NULL)
		return -1;
	return vms_ldwv_resolve(&d->cl->club.ldwv, hash16, out_csid) ==
	       VMS_LDWV_OK ? 0 : -1;
}

static uint32_t dlm_arm_dir_generation(void *ctx)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	if (d == NULL || d->cl == NULL)
		return 0u;
	return vms_ldwv_generation(&d->cl->club.ldwv);
}

static uint32_t dlm_arm_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

static void dlm_arm_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != NULL)
		exec_console_printf("%s\n", msg);
}

/* ==========================================================================
 * 3. The engine's doors, as the FSM's ops
 *
 * Each is one line of translation: the FSM speaks 0/non-zero, the engine speaks
 * SS$_. Nothing here decides anything.
 * ========================================================================== */

static int dlm_arm_refill_post(void *ctx, uint32_t req_lkid, uint32_t op,
			       vms_csid_t dst_csid,
			       struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, dst_csid, out) ==
	       SS__NORMAL ? 0 : -1;
}

static int dlm_arm_record_master(void *ctx, const char *resnam,
				 uint32_t req_lkid, vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid, master_csid) ==
	       SS__NORMAL ? 0 : -1;
}

static int dlm_arm_assume_mastery(void *ctx, const char *resnam,
				  uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) ==
	       SS__NORMAL ? 0 : -1;
}

static int dlm_arm_grant_recv(void *ctx, const struct vms_dlm_proxy_grant *g)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(g) == SS__NORMAL ? 0 : -1;
}

static int dlm_arm_blkast_deliver(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) == SS__NORMAL ? 0 : -1;
}

static int dlm_arm_learn_dir_hash(void *ctx, const char *resnam, uint16_t hash16)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, hash16) == SS__NORMAL ?
	       0 : -1;
}

/*
 * NO ANSWER IS COMING: end the proxy's wait with a REAL terminal status, so a
 * $ENQW returns to its caller instead of hanging. The reason is mapped to a
 * status here -- the FSM names the FACT, this names the code, exactly as
 * vms_dlm_scs_fsm.h says.
 */
static void dlm_arm_fail(void *ctx, uint32_t req_lkid,
			 enum dlm_req_fail_reason why)
{
	uint32_t status;

	(void)ctx;
	switch (why) {
	case DLM_REQ_FAIL_NOTQUEUED:
		status = SS__NOTQUEUED;
		break;
	case DLM_REQ_FAIL_TIMEOUT:
	case DLM_REQ_FAIL_PATHLOST:
	case DLM_REQ_FAIL_UNROUTABLE:
	default:
		/* The honest floor: this executive could not serve the lock.
		 * Never a fabricated grant, and never an eternal wait. */
		status = SS__UNSUPPORTED;
		break;
	}
	(void)vms_lock_dlm_proxy_fail(req_lkid, status);
}

/* THE ONE WAY OUT. The FSM hands a 132-byte body; the connection manager
 * resolves the destination's CSB, applies RULE C, stamps the envelope and
 * sends. This layer cannot send -- it can only ask (vms_dlm_scs.h RULE A). */
static int dlm_arm_send(void *ctx, vms_csid_t dst_csid, const uint8_t *body,
			uint32_t len)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	if (d == NULL || d->cl == NULL)
		return -1;
	return cnxman_dlm_send(d->cl, dst_csid, body, len);
}

/*
 * THE ALL-OVMX GATE, in ONE place, read from the connection manager's own
 * vector every time it is asked (vms-3e3). Two consumers, one fact: the
 * ENGINE's directory grounding (dlm_arm_eng_dir_groundable, below) and the
 * requester FSM's new-shape gate (`dlm_req_ops.all_ovmx`, rd vms-d7a3). A VAX
 * joining closes both and a VAX leaving reopens both, with no cached copy
 * anywhere to go stale.
 */
static int dlm_arm_all_ovmx(struct vms_dlm_scs *d)
{
	if (d == NULL || d->cl == NULL)
		return 0;
	return vms_ldwv_all_ovmx(&d->cl->club.ldwv);
}

static int dlm_arm_all_ovmx_op(void *ctx)
{
	return dlm_arm_all_ovmx((struct vms_dlm_scs *)ctx);
}

static void dlm_arm_bind_req_ops(struct vms_dlm_scs *d)
{
	d->req_ops.send            = dlm_arm_send;
	d->req_ops.all_ovmx        = dlm_arm_all_ovmx_op;
	d->req_ops.refill_post     = dlm_arm_refill_post;
	d->req_ops.dir_resolve     = dlm_arm_dir_resolve;
	d->req_ops.dir_generation  = dlm_arm_dir_generation;
	d->req_ops.record_master   = dlm_arm_record_master;
	d->req_ops.assume_mastery  = dlm_arm_assume_mastery;
	d->req_ops.grant_recv      = dlm_arm_grant_recv;
	d->req_ops.blkast_deliver  = dlm_arm_blkast_deliver;
	d->req_ops.learn_dir_hash  = dlm_arm_learn_dir_hash;
	d->req_ops.fail            = dlm_arm_fail;
	d->req_ops.now_ms          = dlm_arm_now_ms;
	d->req_ops.log             = dlm_arm_log;
	d->req_ops.ctx             = d;
}

/* ==========================================================================
 * 4. The ENGINE's door INTO this arm: a $ENQ that named a remote-mastered
 *    resource (vms_dlm_proxy.h `post`).
 * ========================================================================== */

/*
 * The three work kinds this layer posts to the fork thread -- one per DLM
 * operation, so the operation rides in the work's own `kind` and the two u32
 * arguments carry the only other two values the FSM needs: the KEY and the
 * ROUTING DECISION. Deliberately nothing else: a work item carrying a resource
 * name or a mode would be wire content remembered across a context switch,
 * which is the frame-to-frame plumbing this whole layer is written against.
 */
#define DLM_ARM_WORK_POST_ENQ     1u
#define DLM_ARM_WORK_POST_CONVERT 2u
#define DLM_ARM_WORK_POST_DEQ     3u

static uint32_t dlm_arm_work_kind_to_op(uint16_t kind)
{
	switch (kind) {
	case DLM_ARM_WORK_POST_ENQ:     return VMS_DLM_POST_ENQ;
	case DLM_ARM_WORK_POST_CONVERT: return VMS_DLM_POST_CONVERT;
	case DLM_ARM_WORK_POST_DEQ:     return VMS_DLM_POST_DEQ;
	default:                        return 0u;
	}
}

static uint16_t dlm_arm_op_to_work_kind(uint32_t op)
{
	switch (op) {
	case VMS_DLM_POST_ENQ:     return DLM_ARM_WORK_POST_ENQ;
	case VMS_DLM_POST_CONVERT: return DLM_ARM_WORK_POST_CONVERT;
	case VMS_DLM_POST_DEQ:     return DLM_ARM_WORK_POST_DEQ;
	default:                   return 0u;
	}
}

/* ---- the release queue, each half under the arm's own lock ------------- */

static enum dlm_req_status dlm_arm_relq_stage(struct vms_dlm_scs *d,
					      const struct vms_dlm_proxy_post *p,
					      uint32_t *slot, uint32_t *seq)
{
	enum dlm_req_status st;

	exec_lock(&d->relq_lock);
	st = dlm_relq_stage(&d->relq, p, slot, seq);
	exec_unlock(&d->relq_lock);
	return st;
}

static enum dlm_req_status dlm_arm_relq_claim(struct vms_dlm_scs *d,
					      uint32_t slot, uint32_t seq,
					      struct vms_dlm_proxy_post *out)
{
	enum dlm_req_status st;

	exec_lock(&d->relq_lock);
	st = dlm_relq_claim(&d->relq, slot, seq, out);
	exec_unlock(&d->relq_lock);
	return st;
}

static void dlm_arm_relq_abandon(struct vms_dlm_scs *d, uint32_t slot,
				 uint32_t seq)
{
	exec_lock(&d->relq_lock);
	dlm_relq_abandon(&d->relq, slot, seq);
	exec_unlock(&d->relq_lock);
}

/* Hand ONE work item to the fork thread. Nonzero means it was not taken, and
 * then the caller still owns whatever it staged for that item. */
static int dlm_arm_queue_work(struct vms_dlm_scs *d, uint16_t kind,
			      uint32_t arg0, uint32_t arg1)
{
	struct cf_work w;

	memset(&w, 0, sizeof(w));
	w.owner = (uint16_t)CF_OWNER_DLM;
	w.kind  = kind;
	w.arg0  = arg0;
	w.arg1  = arg1;
	if (cf_post(d->cl->fork, &w) != CF_OK) {
		d->posts_unqueued++;
		return -1;
	}
	d->posts_queued++;
	return 0;
}

/*
 * A RELEASE IS STAGED, NOT REBUILT (rd vms-49f8).
 *
 * `p` is the post vms_lock.c's dlm_proxy_fill_post() filled from the LIVE proxy
 * LKB under res->lock, a few instructions before vms_deq_core tears that LKB
 * down (or, on the rundown path, before lock_teardown_locked does). It is the
 * LAST moment at which this release exists in the lock database, and this is
 * the function that keeps it: the four fields a cat-0x02 op-0x03 asserts are
 * snapshotted into the arm's own queue, and the fork thread emits from that.
 *
 * THIS IS WHY THE op-0x03 EMIT HAS A CALLER AT ALL. Before this, the work item
 * carried only the lock id and the fork thread called `refill_post` -- which
 * answered SS$_IVLOCKID, correctly, because the lock was already gone. The
 * measurement on the live 2-node rig was `releases_sent=0` with
 * `posts_lock_gone=2`: not a gate refusing, a caller that could never arrive.
 *
 * The work item carries the (slot, generation) pair and NOT the release's
 * fields: what crosses to the fork thread is a handle to executive state this
 * arm holds, never wire content copied into a message.
 */
static uint32_t dlm_arm_post_release(struct vms_dlm_scs *d,
				     const struct vms_dlm_proxy_post *p)
{
	uint32_t slot = 0u, seq = 0u;
	enum dlm_req_status st;

	st = dlm_arm_relq_stage(d, p, &slot, &seq);
	if (st == DLM_REQ_E_NOSLOT) {
		/* Nothing staged and nothing sent: the releaser is told, and the
		 * master's belief that it still holds the lock is the
		 * departure/rebuild machinery's to reconcile -- exactly as
		 * vms_lock.c's $DEQ already documents for a failed post. */
		d->releases_no_slot++;
		return SS__INSFMEM;
	}
	if (st != DLM_REQ_OK)
		return SS__BADPARAM;
	d->releases_staged++;

	if (dlm_arm_queue_work(d, DLM_ARM_WORK_POST_DEQ, slot, seq) != 0) {
		dlm_arm_relq_abandon(d, slot, seq);
		return SS__INSFMEM;
	}
	return SS__NORMAL;
}

/*
 * THE ENGINE POSTED A REQUEST, FROM PROCESS CONTEXT ($ENQ's own thread).
 *
 * It is QUEUED to the fork thread and nothing else happens here: vms_dlm_scs.h
 * §5 and vms_dlm_proxy.h both state the contract ("It does NOT wait: the engine
 * owns the wait, because the engine owns the LKB"), and the lock order (design
 * §3.3) forbids a lock-manager path from taking the fork mutex -- which building
 * and stamping a frame on this thread would require, because the envelope
 * counters belong to a CSB the fork thread owns.
 *
 * WHAT IS CARRIED, AND WHY THAT IS NOT LESS HONEST THAN SENDING NOW. The work
 * carries the request's KEY (the proxy LKB's own lock id) and its ROUTING
 * DECISION (which member, resolved through the weight vector) -- the same two
 * values `struct dlm_req` itself holds, and nothing that goes on the wire as
 * content. On the fork thread the post is REBUILT from the lock database
 * through `refill_post`, so the frame that leaves is built from an executive
 * read taken at the moment of transmission, not from a snapshot taken earlier
 * on another thread. A lock released in between makes the refill fail and the
 * transmission is abandoned -- which is correct, because there is no longer a
 * lock to send a frame about.
 *
 * AND THE ONE OPERATION THAT CANNOT WORK THAT WAY IS THE RELEASE (rd vms-49f8).
 * A $DEQ does not leave a lock behind to re-read: it IS the teardown. So a
 * release takes the staged path above instead -- snapshotted here, in the
 * releaser's own context, while the LKB is still real.
 */
static uint32_t dlm_arm_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;
	uint16_t kind;

	if (d == NULL || d->cl == NULL || p == NULL)
		return SS__BADPARAM;
	if (p->req_lkid == VMS_DLM_LKID_UNSET)
		return SS__BADPARAM;   /* never a placeholder handle */
	kind = dlm_arm_op_to_work_kind(p->op);
	if (kind == 0u)
		return SS__BADPARAM;

	if (kind == DLM_ARM_WORK_POST_DEQ)
		return dlm_arm_post_release(d, p);

	return dlm_arm_queue_work(d, kind, p->req_lkid,
				  (uint32_t)p->dst_csid) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__INSFMEM;
}

/* The fork-thread half of an ENQ/CONVERT post: rebuild it from the lock
 * database, then drive the FSM. Every non-OK outcome is the FSM's own counted
 * refusal. A RELEASE never arrives here -- it has no lock left to rebuild from
 * and takes dlm_arm_run_release below. */
static void dlm_arm_run_post(struct vms_dlm_scs *d, uint16_t kind,
			     uint32_t req_lkid, uint32_t dst_csid)
{
	struct vms_dlm_proxy_post p;
	uint32_t op = dlm_arm_work_kind_to_op(kind);

	if (op == 0u || op == VMS_DLM_POST_DEQ)
		return;
	if (dlm_arm_refill_post(d, req_lkid, op, (vms_csid_t)dst_csid, &p) != 0) {
		d->posts_lock_gone++;
		return;
	}
	if (dlm_req_fsm_post(&d->req, &p) != DLM_REQ_OK)
		d->posts_refused++;
}

/*
 * The fork-thread half of a RELEASE: CLAIM the snapshot this work item names
 * and drive the FSM from it. The gates are untouched and all of them are
 * downstream of here -- dlm_req_fsm_post applies the all-OVMX gate and the
 * codec's lock-id refusal, and cnxman_dlm_send applies RULE C per destination --
 * so a staged release toward a system this executive cannot prove runs this
 * implementation is still counted and still not emitted.
 *
 * A claim that names no staged release (a duplicate work item, or a queue reset
 * by a cluster stop) emits NOTHING and is counted: a release is transmitted once
 * or not at all.
 */
static void dlm_arm_run_release(struct vms_dlm_scs *d, uint32_t slot,
				uint32_t seq)
{
	struct vms_dlm_proxy_post p;

	if (dlm_arm_relq_claim(d, slot, seq, &p) != DLM_REQ_OK) {
		d->releases_stale++;
		return;
	}
	if (dlm_req_fsm_post(&d->req, &p) != DLM_REQ_OK)
		d->posts_refused++;
}

/* The engine's directory ops are defined below, after their long rationale;
 * forward-declared here because bind_engine_ops takes their addresses. */
static uint32_t dlm_arm_eng_dir_resolve(void *ctx, uint16_t hash16,
					uint32_t *out_csid);
static uint32_t dlm_arm_eng_dir_generation(void *ctx);
static int dlm_arm_eng_dir_groundable(void *ctx);
static uint32_t dlm_arm_eng_dir_ground(void *ctx, const char *name,
				       uint32_t name_len, uint16_t *out_hash16);

static void dlm_arm_bind_engine_ops(struct vms_dlm_scs *d)
{
	d->eng_ops.post           = dlm_arm_post;
	/* All directory ops are installed unconditionally: the all-OVMX gate is
	 * now DYNAMIC (dir_groundable), checked by the engine at resolve time, not
	 * a one-shot decision at start. In a mixed or single-node configuration the
	 * gate reads 0 and the engine masters locally exactly as before -- see the
	 * long note above dlm_arm_eng_dir_resolve (rung A", vms-3e3). */
	d->eng_ops.dir_resolve    = dlm_arm_eng_dir_resolve;
	d->eng_ops.dir_generation = dlm_arm_eng_dir_generation;
	d->eng_ops.dir_groundable = dlm_arm_eng_dir_groundable;
	d->eng_ops.dir_ground     = dlm_arm_eng_dir_ground;
	d->eng_ops.ctx            = d;
}

/*
 * ===========================================================================
 * THE ENGINE'S DIRECTORY RESOLVER IS NOW INSTALLED, BEHIND THE ALL-OVMX GATE
 * (rung A", design SS3.6; vms-3e3, conductor-ratified). This block records the
 * bootstrap deadlock it resolves and exactly why the resolution is safe.
 * ===========================================================================
 *
 * THE DEADLOCK, AS MEASURED. vms_lock.c's dir_resolve() refuses, BEFORE it
 * ever calls this op, when the resource block carries no WIRE-LEARNED hash:
 *
 *     if (!res->hash_known)
 *             return SS__UNSUPPORTED;       // INV-6: wire-learned or nothing
 *
 * and vms_dlm_proxy.h states the consequence as the design's own rule: "ABSENT
 * (NULL) MEANS 'NO CLUSTER', NOT 'REFUSE' ... It is only when a resolver IS
 * installed that a resource with no wire-learned hash is refused." So
 * installing this op is what turns the refusal on, for EVERY root name this
 * node has never seen on the wire. tests/cluster/host/test_lock_dir.c pins both
 * halves already.
 *
 * THE BOOTSTRAP DEADLOCK. A wire-learned hash reaches a resource block from
 * exactly one place: a cat-0x02 frame somebody ELSE sent (Davis p. 6-50,
 * vms_lock_dlm_learn_dir_hash). With a real VAX in the cluster hashes flow, but
 * RULE C forbids routing DLM traffic to a system not proven to run this
 * implementation. In an OVMX-ONLY cluster RULE C permits it, but no member can
 * originate the FIRST cat-0x02 frame: doing so needs a hash, and computing DEC's
 * is Rule-8-forbidden (the function is unpublished, and a wrong value made a
 * real VAX install OVMX as master of resources it did not master -- the 35/s
 * grant storm). Without a source for the first hash, the whole cross-node path
 * dead-ends UPSTREAM of this file, and installing this op naively would refuse
 * EVERY first $ENQ -- the ACP volume lock, RMS, the XQP -- and stop a two-node
 * OVMX cluster from mounting SYS$DISK.
 *
 * THE RESOLUTION -- rung A", ratified (vms-3e3). An all-proven-OVMX cluster has
 * no real VAX to mis-address and no DEC compatibility to honour on its own
 * private names, so it may originate the first hash with OVMX's OWN directory
 * hash (vms_dlm_ovmx_dir_hash above): deterministic, identical on every OVMX
 * node, documented as OVMX's own. Two gates keep it exactly as narrow as the
 * ruling requires:
 *
 *   - dir_groundable (the all-OVMX gate) is DYNAMIC. When any member cannot be
 *     proven OVMX, the engine masters names locally exactly as before any
 *     resolver existed -- so a mixed OVMX+VAX cluster and a node booting alone
 *     see NO change, and nothing is routed at, or grounded toward, a real VAX.
 *     This is the anti-regression guarantee; it is why installing the resolver
 *     no longer breaks SYS$DISK mount.
 *   - dir_ground grounds ONLY names never seen on the wire, and ONLY when the
 *     gate holds. A name WITH a wire-learned hash still routes by the received
 *     value (dir_resolve), never a computed one.
 *
 * This is exactly parallel to the LDWV all-zero fallback (Option-A): that
 * grounds the VECTOR for an all-OVMX cluster; this grounds the HASH. Both are
 * OVMX bridges for all-OVMX clusters; real-VMS DLM-directory interop stays
 * deferred to FC-P3.2 (oracle-grounded), and neither claims it. INV-6.
 *
 * REMAINING HONESTY DEBT: in an all-OVMX cluster the OVMX hash names a single
 * master per name on every node, which IS cluster-wide mastering; but the value
 * is OVMX's own, so it must never be described as VMS-directory-compatible. In a
 * mixed cluster the old floor stands: each node may master a novel name locally
 * (not cluster-wide) -- unchanged, and named, not hidden.
 */
static uint32_t dlm_arm_eng_dir_resolve(void *ctx, uint16_t hash16,
					uint32_t *out_csid)
{
	vms_csid_t csid = 0;

	if (dlm_arm_dir_resolve(ctx, hash16, &csid) != 0)
		return SS__UNSUPPORTED;
	*out_csid = (uint32_t)csid;
	return SS__NORMAL;
}

static uint32_t dlm_arm_eng_dir_generation(void *ctx)
{
	return dlm_arm_dir_generation(ctx);
}

/*
 * OVMX'S OWN 16-BIT DIRECTORY HASH (rung A", design SS3.6; vms-3e3).
 *
 * This is OVMX's own function, and it is documented as OVMX's own. It is NOT
 * DEC's directory hash -- that function is unpublished and Rule-8-forbidden to
 * reproduce, and it is never needed here because this value NEVER reaches a real
 * VAX (the all-OVMX gate below, plus RULE C on the send side). It reuses the
 * FNV-1a spelling the lock manager already computes over a resource name for its
 * own hash table (vms_lock.c resource_hash_key: offset basis 2166136261,
 * prime 16777619), folded to 16 bits -- a public, well-understood function of
 * the NAME BYTES, chosen precisely because it bears no relationship to DEC's.
 *
 * The only property that matters for correctness is CONSISTENCY (p. 6-32): every
 * OVMX node must map a given name to the same 16-bit value, so all members agree
 * on the master. That holds by construction -- every node runs this one function
 * over the same bytes -- with no dependence on byte order (each byte is folded
 * in on its own).
 */
static uint16_t vms_dlm_ovmx_dir_hash(const char *name, uint32_t len)
{
	uint32_t h = 2166136261u;     /* FNV-1a offset basis */
	uint32_t i;

	if (name == NULL)
		return 0u;
	for (i = 0u; i < len; i++) {
		h ^= (uint32_t)(unsigned char)name[i];
		h *= 16777619u;           /* FNV-1a prime */
	}
	return (uint16_t)((h >> 16) ^ (h & 0xFFFFu));   /* fold 32 -> 16 */
}

/*
 * THE ALL-OVMX GATE, as the engine sees it (vms-3e3). Cross-node resolution and
 * hash grounding are live ONLY while every member is proven-OVMX -- read
 * dynamically from the connection manager's own vector, so a VAX joining turns
 * both off and a VAX leaving turns them back on with no code path to go stale.
 * When this reads 0 the engine masters names locally exactly as an unclustered
 * node does: the anti-regression guarantee for mixed OVMX+VAX clusters.
 */
static int dlm_arm_eng_dir_groundable(void *ctx)
{
	return dlm_arm_all_ovmx((struct vms_dlm_scs *)ctx);
}

/*
 * GROUND a root name's directory hash -- the deliberately-forbidden name->hash
 * op (vms_dlm_proxy.h), permitted ONLY behind the gate. It refuses unless the
 * cluster is all-proven-OVMX, so the name->hash step never runs with a real VAX
 * present -- the 90b3bbbd storm was a real cluster and this cannot touch one.
 * The value is OVMX's own (above), and it makes no claim of real-VMS directory
 * compatibility (that is FC-P3.2, oracle-grounded). INV-6.
 */
static uint32_t dlm_arm_eng_dir_ground(void *ctx, const char *name,
				       uint32_t name_len, uint16_t *out_hash16)
{
	if (out_hash16 == NULL || name == NULL)
		return SS__BADPARAM;
	if (!dlm_arm_eng_dir_groundable(ctx))
		return SS__UNSUPPORTED;   /* not all-OVMX: never ground here */
	*out_hash16 = vms_dlm_ovmx_dir_hash(name, name_len);
	return SS__NORMAL;
}

/* ==========================================================================
 * 5. INBOUND -- one cat-0x02 message on the VMS$VAXcluster connection
 * ========================================================================== */

/*
 * RULE C, the serve half. A system that has not advertised a software-version
 * token byte-identical to our own is, as far as this executive can honestly
 * say, not running this implementation -- and OVMX's cat-0x02 arm is grounded
 * against ITS OWN protocol, not against a real VAX's lock manager. So its
 * request is not served, no lock state is created for it, and nothing is
 * emitted at it. Counted, never silent.
 */
static int dlm_arm_peer_is_ours(struct vms_dlm_scs *d,
				const struct dlm_scs_request *req)
{
	if (req->peer_is_ours)
		return 1;
	d->foreign_refused++;
	dlm_arm_log(d, "%DLM, refusing a lock message from a system that has "
		       "not proved it runs this implementation");
	return 0;
}

/* Fill the master-side request from the PARSED frame and the connection
 * manager's own identification of the sender. `from_csid` is spec §4(a)'s
 * cluster-logical source address -- the one grounded answer to "who is
 * asking"; the DLM body carries no CSID field at all. */
static void dlm_arm_fill_master_req(const struct dlm_scs_request *in,
				    const struct vms_dlm_enq_request *e,
				    uint8_t wireop,
				    struct vms_dlm_master_request *out)
{
	uint32_t n = e->name_len;

	memset(out, 0, sizeof(*out));
	out->op = (wireop == VMS_DLM_WIREOP_CONVERT) ? VMS_DLM_MREQ_CONVERT
						     : VMS_DLM_MREQ_ENQ;
	out->req_csid = (uint32_t)in->from_csid;
	out->req_lkid = e->req_pid_or_lkid;
	out->master_lkid = e->master_lkid;
	out->lkmode = e->mode;
	if (n >= sizeof(out->resnam))
		n = (uint32_t)sizeof(out->resnam) - 1u;
	memcpy(out->resnam, e->name, n);
	out->resnam[n] = '\0';
}

/* Hand the body the codec just built into the CM's reply buffer. The builders
 * write frame-absolute, so the body is the splice at VMS_OFF_SYSAP_BODY -- the
 * only place in this file that names an offset, and it names the codec's own
 * published body origin, not a field. */
static int dlm_arm_stage_reply(struct vms_dlm_scs *d,
			       struct dlm_scs_reply *reply)
{
	if (reply == NULL || reply->body == NULL ||
	    reply->cap < VMS_CM_BODY_LEN)
		return -1;
	memcpy(reply->body, d->txframe + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	reply->len = VMS_CM_BODY_LEN;
	return 0;
}

/* GRANTED: every field of the reply is read off the LKB the engine stamped --
 * the handle it minted, the mode that lock actually holds, and the requester
 * handle it recorded. None of them is echoed from the request (RULE B). */
static int dlm_arm_reply_grant(struct vms_dlm_scs *d,
			       const struct vms_dlm_master_result *r,
			       struct dlm_scs_reply *reply)
{
	uint32_t written = 0;
	vms_codec_status_t rc;

	memset(d->txframe, 0, sizeof(d->txframe));
	/*
	 * THE LVB READ CROSSING (vms-727). When the master resource holds a
	 * value block (result->valblk_present), the grant RETURNS it -- the
	 * grant-with-valblk builder -- so the requester's $ENQ(VALBLK)/$GETLKI
	 * reads the master's LVB back. Otherwise the plain grant, unchanged and
	 * proven cross-node. Every byte of either frame is read off the LKB/RSB
	 * the engine stamped or is a grounded constant; none is composed.
	 */
	if (r->valblk_present)
		rc = vms_dlm_enq_response_build_grant_valblk(r->req_lkid,
					     r->master_lkid, r->granted_mode,
					     r->valblk, d->txframe,
					     (uint32_t)sizeof(d->txframe), &written);
	else
		rc = vms_dlm_enq_response_build_grant(r->req_lkid, r->master_lkid,
					     r->granted_mode, d->txframe,
					     (uint32_t)sizeof(d->txframe), &written);
	if (rc != VMS_CODEC_OK) {
		d->codec_failures++;
		return -1;
	}
	if (dlm_arm_stage_reply(d, reply) != 0)
		return -1;
	d->grants_sent++;
	return 0;
}

/*
 * DENIED (SS$_NOTQUEUED): the spec's deny shape echoes the request's body[20]
 * placeholder and the resource name back verbatim. Those two ARE echoes, by the
 * protocol's own definition of this reply -- they identify the request being
 * refused -- and nothing else is asserted: no lock id of ours, because we hold
 * no lock for it.
 */
static int dlm_arm_reply_deny(struct vms_dlm_scs *d,
			      const struct vms_dlm_enq_request *e,
			      struct dlm_scs_reply *reply)
{
	uint32_t written = 0;

	memset(d->txframe, 0, sizeof(d->txframe));
	if (vms_dlm_enq_response_build_deny(e->req_pid_or_lkid, e->master_lkid,
					    e->name_len, e->name, d->txframe,
					    (uint32_t)sizeof(d->txframe),
					    &written) != VMS_CODEC_OK) {
		d->codec_failures++;
		return -1;
	}
	if (dlm_arm_stage_reply(d, reply) != 0)
		return -1;
	d->denies_sent++;
	return 0;
}

/*
 * THE BLOCKING AST's FRAME (op 0x04, rd vms-d7a3) -- master -> the remote
 * holder whose lock is in the way. GROUNDED by the vms-c03 capture of a real
 * 2-node OpenVMS VAX 7.3 cluster, where the master->holder BLKAST's two lock-id
 * fields correlate byte-for-byte to the holder's own op-0x01 ENQ and to the
 * handle that ENQ's grant assigned.
 *
 * EVERY FIELD IS AN EXECUTIVE READ (RULE B), and all three come off the SAME
 * granted LKB that vms_lock.c found on `res->granted`, under `res->lock`, while
 * it was deciding the conflict:
 *   master_lkid  = `blocking_master_lkid`, that LKB's own lock id as THIS node
 *                  (the master) minted it;
 *   req_lkid     = `blocking_req_lkid`, the handle the HOLDER itself minted and
 *                  which this master stamped on the LKB when it served the
 *                  holder's request -- so the holder finds its ORIGIN record by
 *                  a value the holder's own executive produced;
 *   the DESTINATION = `blocking_csid`, the cluster identity the LKB is held
 *                  for. None of the three is echoed from the request that ran
 *                  into the conflict, and none is a counter.
 *
 * TWO THINGS ARE DELIBERATELY ABSENT FROM THE FRAME:
 *   - the MODE-CONTEXT PAIR at body[30:32]. It is OBSERVED and NOT PINNED (the
 *     codec says so: three samples across two locks is not a one-variable
 *     diff), so `mode_ctx_valid` stays 0, the builder leaves the span
 *     untouched, and this executive asserts nothing it does not hold.
 *   - a RESOURCE NAME. That is the protocol's own shape, not an omission: the
 *     reference frame's readable body[48] belongs to a DIFFERENT lock and is
 *     stale buffer. A BLKAST names its lock by lock-id and by nothing else.
 */
static int dlm_arm_build_blkast(struct vms_dlm_scs *d,
				const struct vms_dlm_master_result *r)
{
	struct vms_dlm_blkast b;
	uint32_t written = 0;

	memset(&b, 0, sizeof(b));
	b.req_lkid       = r->blocking_req_lkid;
	b.master_lkid    = r->blocking_master_lkid;
	b.mode_ctx_valid = 0u;   /* OBSERVED-not-pinned: write NOTHING there */

	memset(d->txframe, 0, sizeof(d->txframe));
	if (vms_dlm_blkast_build(&b, d->txframe, (uint32_t)sizeof(d->txframe),
				 &written) != VMS_CODEC_OK) {
		/* The codec refuses an unset lock id in either field -- the
		 * fc8540ae INVLOCKID lesson on a lock-id-only message. */
		d->codec_failures++;
		return -1;
	}
	return 0;
}

/*
 * A QUEUED request blocks a REMOTE holder, and the master owes that holder a
 * BLOCKING AST. It is SENT (rd vms-d7a3), behind gates that are not this
 * function's to relax:
 *
 *   - THE ALL-OVMX GATE. op 0x04 is a shape OVMX has read off a real cluster's
 *     wire and has never yet been watched to emit AT one, so it goes only where
 *     every member is proven to run this implementation.
 *   - RULE C, per DESTINATION, inside `dlm_arm_send` -> cnxman_dlm_send ->
 *     cnxman_dlm_peer_proven(csb->peer_is_ours). Note this is a DIFFERENT
 *     system from the one RULE C's serve half cleared at the top of
 *     dlm_arm_handle_request: that gate proved the REQUESTER, this one proves
 *     the HOLDER, and the holder is who this frame is addressed to.
 *
 * THIS IS NOT A REPLY, so RULE A is intact (a reply never leaves by itself). It
 * is an ORIGINATION at a THIRD system, addressed by a CSID the lock database
 * named -- exactly what cnxman_dlm_send exists for. The QUEUED outcome stages
 * no reply, so the frame scratch is free for it.
 *
 * Every way it can fail to go out is ONE counter, because they are one fact:
 * this master could not honestly notify that holder.
 */
static void dlm_arm_send_blkast(struct vms_dlm_scs *d,
				const struct vms_dlm_master_result *r)
{
	if (r->blocking_csid == 0u)
		return;   /* nothing blocks it across nodes: nothing is owed */
	if (!dlm_arm_all_ovmx(d) || dlm_arm_build_blkast(d, r) != 0 ||
	    dlm_arm_send(d, (vms_csid_t)r->blocking_csid,
			 d->txframe + VMS_OFF_SYSAP_BODY,
			 VMS_CM_BODY_LEN) != 0) {
		d->blkasts_no_wire_op++;
		return;
	}
	d->blkasts_sent++;
}

/* One inbound ENQ/CONVERT, served as the tree's master. */
static int dlm_arm_serve_enq(struct vms_dlm_scs *d,
			     const struct dlm_scs_request *in,
			     const struct vms_dlm_enq_request *e,
			     uint8_t wireop, struct dlm_scs_reply *reply)
{
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result res;

	if (!vms_lock_dlm_have_delivery_proc()) {
		/* CONDITION 4 (rd vms-c27): no process to own the LKB, so no
		 * lock is created and nothing is answered. */
		d->no_delivery_proc++;
		return -1;
	}
	dlm_arm_fill_master_req(in, e, wireop, &mr);
	if (vms_lock_dlm_master_serve(&mr, &res) != SS__NORMAL)
		return -1;

	d->req_received++;
	switch ((enum vms_dlm_master_outcome)res.outcome) {
	case VMS_DLM_MASTER_GRANTED:
		return dlm_arm_reply_grant(d, &res, reply);
	case VMS_DLM_MASTER_DENIED:
		return dlm_arm_reply_deny(d, e, reply);
	case VMS_DLM_MASTER_QUEUED:
		/* A REAL lock on a REAL waiting queue. The answer is the grant
		 * that follows when the holder releases, so nothing goes back
		 * now -- the honest silence vms_dlm_scs.h's reply->len == 0
		 * names. */
		dlm_arm_send_blkast(d, &res);
		d->queued_no_reply++;
		return 0;
	case VMS_DLM_MASTER_REDIRECT:
		/* Davis p. 6-31 outcome 2. There is no grounded cat-0x02 shape
		 * for "the master is X" (§4(f).1 grounds the grant and deny
		 * shapes and nothing else), so this executive answers with
		 * SILENCE rather than a frame nobody grounded, and counts it.
		 * The requester's own redirect budget bounds the exchange. */
		d->redirects_sent++;
		return 0;
	case VMS_DLM_MASTER_RELEASED:
	case VMS_DLM_MASTER_REFUSED:
	default:
		d->declined++;
		return -1;
	}
}

/*
 * THE RELEASE'S RECEIVE HALF (rd vms-c72), master side.
 *
 * A peer that held a lock THIS node masters has released it. The engine already
 * implements every part of that -- VMS_DLM_MREQ_DEQ, the release itself, and
 * the report of the queued cross-node waiter it flipped to granted
 * (vms_dlm_master.h) -- and what was missing was this file calling it from a
 * RECEIVED FRAME. This is that call.
 *
 * WHAT IS ASSERTED, AND WHERE EACH VALUE COMES FROM (RULE B / INV-6):
 *   req_csid     the CONNECTION MANAGER's identification of the sender (spec
 *                §4(a)). The DLM body carries no CSID field at all, and it is
 *                what the engine checks the lock's own tag against -- a peer
 *                may not release a lock held for another node (proven in
 *                tests/cluster/host/test_lock_host.c).
 *   req_lkid     body[20:24], through the codec: the releaser's OWN handle.
 *   master_lkid  body[24:28], through the codec: OUR handle, which is what
 *                names the LKB to release. A zero in EITHER field is refused by
 *                the codec before this function ever sees the frame (the
 *                fc8540ae INVLOCKID lesson, applied on the receive side too).
 *   lkmode       body[30], carried as the mode being released.
 *
 * NOTHING IS DEFAULTED. No resource name is read (a real $DEQ carries none) and
 * `valblk_present` stays 0, so the master's own value block is LEFT ALONE
 * rather than overwritten with sixteen zeros that would read exactly like data.
 */
static void dlm_arm_fill_deq_req(const struct dlm_scs_request *in,
				 const struct vms_dlm_deq *q,
				 struct vms_dlm_master_request *out)
{
	memset(out, 0, sizeof(*out));
	out->op = VMS_DLM_MREQ_DEQ;
	out->req_csid = (uint32_t)in->from_csid;
	out->req_lkid = q->req_lkid;
	out->master_lkid = q->master_lkid;
	out->lkmode = q->mode;
}

/*
 * THE DEFERRED GRANT THIS RELEASE MAY HAVE EARNED -- OWED, COUNTED, AND NOT
 * ORIGINATED.
 *
 * When the release flips a queued cross-node waiter to granted, the engine says
 * so and the flip is REAL: that waiter's lock is on this master's granted queue
 * from this moment. Telling it, though, would mean ORIGINATING a cat-0x82 grant
 * at a system that did not just ask -- and §4(f).1 grounds the grant shape as
 * the ANSWER TO A REQUEST, correlated by the connection manager's own
 * transaction envelope (RULE A). An uncorrelated reply is exactly what a real
 * VAX rejects, so this executive does not invent one.
 *
 * The waiter is not stranded by that silence: its own request is still
 * outstanding at this master, its retransmit ladder is still running, and the
 * engine's cross-node ENQ is idempotent on (req_csid, req_lkid) -- so the next
 * retransmission finds the lock GRANTED and is answered with a real, correlated
 * grant built from the LKB. If the ladder is spent first, the requester fails
 * its $ENQW with a real status rather than waiting forever. Either way the
 * counter below says how many grants this master owed, which is the measurement
 * the origination rung needs.
 */
static void dlm_arm_count_deferred_grant(struct vms_dlm_scs *d,
					 const struct vms_dlm_master_result *r)
{
	if (r->deferred_grant && r->deferred_csid != 0u)
		d->deferred_grants_no_wire_op++;
}

/* One inbound op-0x03 $DEQ, served as the tree's master. It is a CONSUME, not a
 * RESPOND (the codec's allowlist rows say so: no cat-0x82 answer to a release
 * appears anywhere in the reference capture), so no reply is ever staged and
 * `reply->len` stays 0 -- the honest silence vms_dlm_scs.h names. */
static int dlm_arm_serve_deq(struct vms_dlm_scs *d,
			     const struct dlm_scs_request *in)
{
	struct vms_dlm_deq q;
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result res;

	if (vms_dlm_deq_parse_body(in->body, in->len, &q) != VMS_CODEC_OK) {
		d->unparsed++;
		return -1;
	}
	if (!vms_lock_dlm_have_delivery_proc()) {
		/* CONDITION 4 (rd vms-c27): no process owns the master-side
		 * LKBs, so there is nothing of ours to release. */
		d->no_delivery_proc++;
		return -1;
	}
	dlm_arm_fill_deq_req(in, &q, &mr);
	if (vms_lock_dlm_master_serve(&mr, &res) != SS__NORMAL) {
		d->releases_refused++;
		return -1;
	}

	d->req_received++;
	if ((enum vms_dlm_master_outcome)res.outcome !=
	    VMS_DLM_MASTER_RELEASED) {
		/* REFUSED: the handle named no lock this node holds for THAT
		 * sender. Nothing was released and nothing is answered. */
		d->releases_refused++;
		return -1;
	}
	d->releases_received++;
	dlm_arm_count_deferred_grant(d, &res);
	return 0;
}

/*
 * THE BLOCKING AST's RECEIVE HALF (rd vms-c72), holder side -- the other end of
 * this file's own op-0x04 emit.
 *
 * Everything below the parse already existed and was already proven: the
 * requester FSM's BLKAST event, and `dlm_arm_blkast_deliver` -> the engine's
 * `vms_lock_dlm_proxy_blkast_recv`, which fires the REAL user-mode AST the
 * holder registered. What was missing was the frame reaching it. The FSM's
 * body-taking entry does the codec parse (and inherits its refusal of a zero
 * lock id) and finds the proxy by the handle THIS node minted, so no value from
 * the frame is ever used as anything but a key into our own lock records.
 */
static int dlm_arm_deliver_blkast(struct vms_dlm_scs *d,
				  const struct dlm_scs_request *in)
{
	return dlm_req_fsm_blkast_body(&d->req, in->from_csid, in->body,
				       in->len) == DLM_REQ_OK ? 0 : -1;
}

/*
 * THE VALUE-BLOCK WRITE's RECEIVE HALF (rd vms-727), master side -- the other
 * end of this file's own op-0x06 emit. A remote holder demoted a lock it holds
 * at a write mode and flushed the value block; this replicates that WIRE value
 * into the master resource. Like the $DEQ receive it is a CONSUME (the capture
 * carries no cat-0x82 answer to an op-0x06), so nothing is ever staged. The
 * engine authorizes by cluster identity: the block lands only on a lock this
 * node holds FOR the sender (vms_lock_dlm_master_apply_valblk), so a frame's
 * value is never used as anything but the payload for a lock the sender owns.
 */
static int dlm_arm_serve_valblk(struct vms_dlm_scs *d,
				const struct dlm_scs_request *in)
{
	struct vms_dlm_valblk_convert c;

	if (vms_dlm_valblk_convert_parse_body(in->body, in->len, &c) !=
	    VMS_CODEC_OK) {
		d->unparsed++;
		return -1;
	}
	if (!vms_lock_dlm_have_delivery_proc()) {
		d->no_delivery_proc++;
		return -1;
	}
	if (vms_lock_dlm_master_apply_valblk(in->from_csid, c.master_lkid,
					     c.valblk) != SS__NORMAL) {
		/* The handle named no lock this node holds for THAT sender:
		 * a peer may not write another node's value block. */
		d->declined++;
		return -1;
	}
	d->req_received++;
	d->valblk_writes_received++;
	return 0;
}

/* A cat-0x82 reply to something THIS node asked for. */
static int dlm_arm_handle_reply(struct vms_dlm_scs *d,
				const struct dlm_scs_request *in)
{
	enum dlm_req_status st;

	d->replies_received++;
	/*
	 * `correlated_lkid` 0: the connection manager's §4(j) transaction
	 * correlation is not surfaced to the DLM today, so the FSM matches on
	 * body[20] -- the handle THIS node put on its own request. The FSM
	 * documents both paths and prefers the envelope's when it has one.
	 */
	st = dlm_req_fsm_reply_body(&d->req, in->from_csid, 0u, in->body,
				    in->len);
	return st == DLM_REQ_OK ? 0 : -1;
}

/*
 * THE DOOR. One cat-0x02 message arrived on the VMS$VAXcluster connection.
 * Returns 0 when it was handled (with or without a reply) and non-zero when it
 * was DECLINED -- declines are counted, never hidden.
 */
static int dlm_arm_handle_request(void *ctx, const struct dlm_scs_request *req,
				  struct dlm_scs_reply *reply)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;
	struct vms_dlm_enq_request e;
	uint8_t wireop = 0;

	if (d == NULL || req == NULL || req->body == NULL ||
	    req->len < VMS_CM_BODY_LEN)
		return -1;

	/*
	 * THE HASH, FIRST AND ALWAYS, AND FROM ANY SENDER (Davis p. 6-50,
	 * integration note E49). Every inbound cat-0x02 frame in any role may
	 * carry the sender's own 16-bit value for the root name, and recording
	 * it is the ONLY reason this node can ever address a directory lookup.
	 * It is a pure READ -- nothing is emitted and no lock state is created
	 * -- so it is outside RULE C, which is about what leaves this node.
	 */
	if (req->category == (uint8_t)VMS_DLM_CAT_REQUEST)
		(void)dlm_req_fsm_observe_body(&d->req, req->body, req->len);

	if (req->category == (uint8_t)VMS_DLM_CAT_REQUEST &&
	    req->opcode == (uint8_t)VMS_DLM_WIREOP_REBUILD) {
		/*
		 * THE op-0x0d REBUILD RECORD IS NOT THIS ARM'S TO ANSWER, AND
		 * NOT THIS ARM'S TO REFUSE.
		 *
		 * It has a GROUNDED verbatim-echo response recipe of its own
		 * (spec §4(p): 1367 of 1367 real responses reconstructed
		 * byte-for-byte, zero residuals), the barrier FSM owns it, and
		 * it is how OVMX joined a REAL VAX cluster. Answering it here
		 * from lock state would replace a recipe proven against real
		 * traffic with one that is not -- and DECLINING it would make
		 * the barrier skip the echo and strand a real VAX's rebuild.
		 *
		 * So: hash learned above, reply->len left at 0, and the answer
		 * left to the recipe. This is also why the RULE C gate is
		 * BELOW this branch and not above it.
		 */
		return 0;
	}

	/* RULE C. Everything past this point either creates lock state for the
	 * sender or emits at it, and neither may happen for a system that has
	 * not proved it runs this implementation. */
	if (!dlm_arm_peer_is_ours(d, req))
		return -1;

	if (req->category == (uint8_t)(VMS_DLM_CAT_REQUEST |
				       VMS_WIRE_RESPONSE_BIT))
		return dlm_arm_handle_reply(d, req);
	if (req->category != (uint8_t)VMS_DLM_CAT_REQUEST)
		return -1;

	/*
	 * THE TWO LOCK-ID-ONLY OPS (rd vms-c72), routed on the opcode the
	 * connection manager read off the frame -- the same field the op-0x0d
	 * branch above routes on. Each re-checks the category and the opcode
	 * from the BODY ITSELF inside the codec, so this switch cannot deliver
	 * a body that is not what the envelope claimed.
	 *
	 * BOTH ARE BELOW RULE C, which is the whole reason the gate sits where
	 * it does: an op-0x03 creates (well, destroys) real lock state and an
	 * op-0x04 fires a real user-mode AST, and neither may happen for a
	 * system that has not proved it runs this implementation.
	 */
	if (req->opcode == (uint8_t)VMS_DLM_WIREOP_DEQ)
		return dlm_arm_serve_deq(d, req);
	if (req->opcode == (uint8_t)VMS_DLM_WIREOP_BLKAST)
		return dlm_arm_deliver_blkast(d, req);
	if (req->opcode == (uint8_t)VMS_DLM_WIREOP_CONVERT_VALBLK)
		return dlm_arm_serve_valblk(d, req);

	if (vms_dlm_enq_request_parse_body(req->body, req->len, &wireop, &e) !=
	    VMS_CODEC_OK) {
		d->unparsed++;
		return -1;
	}
	if (wireop == VMS_DLM_WIREOP_ENQ || wireop == VMS_DLM_WIREOP_CONVERT)
		return dlm_arm_serve_enq(d, req, &e, wireop, reply);

	d->declined++;
	return -1;
}

/* ==========================================================================
 * 6. The transition boundaries and a departure
 * ========================================================================== */

/*
 * TELL THE LOCK ENGINE WHO THIS NODE IS (vms_dlm_master.h §1b).
 *
 * The engine's `vms_local_csid` is each substrate's insmod placeholder until
 * something binds it to the cluster's own assignment, and nothing did: measured
 * on the two-node rig, both MEMBERs -- holding CSIDs 0x00010001 and 0x00010002
 * -- reported local_csid=0x00000001 through GET_RESMASTER. An outbound request
 * would have named CSID 1 as the requester from every node.
 *
 * The CLUB is where the assignment lives, and `local_csid_valid` is what says
 * it is real; an unlearned CSID overwrites nothing. Run on the arm's own beat
 * and at every transition boundary, because those are the two moments the CLUB
 * can have learned one -- a genesis that founds generation 1, or an admission
 * that assigns a slot.
 */
static void dlm_arm_sync_local_csid(struct vms_dlm_scs *d)
{
	if (d->cl == NULL || !d->cl->club.local_csid_valid)
		return;
	vms_lock_dlm_set_local_csid((uint32_t)d->cl->club.local_csid);
}

static void dlm_arm_transition_begin(void *ctx,
				     const struct cnxman_transition *tr)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	(void)tr;
	if (d == NULL)
		return;
	/*
	 * Phase 1 (Davis p. 7-40/7-41, p. 6-33): all directory information is
	 * discarded cluster-wide. The vector is invalidated by the connection
	 * manager and the ENGINE re-resolves on the generation change, so there
	 * is nothing for this arm to freeze by hand -- the cache invalidation is
	 * structural, not remembered. Counted so a transcript shows the boundary.
	 */
	d->transitions_begun++;
}

static void dlm_arm_transition_end(void *ctx,
				   const struct cnxman_transition *tr,
				   int completed)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	(void)tr;
	(void)completed;
	if (d == NULL)
		return;
	d->transitions_ended++;
	/* A transition is one of the two moments the CLUB can have learned this
	 * node's CSID (the other is genesis); tell the engine at once rather
	 * than waiting for the next beat. */
	dlm_arm_sync_local_csid(d);
}

static void dlm_arm_member_departed(void *ctx, vms_csid_t csid)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;
	uint32_t found = 0;

	if (d == NULL || csid == 0)
		return;
	d->members_departed++;
	/* Every request outstanding AT the departed node is failed with a real
	 * path-lost status, so no $ENQW waits for an answer that cannot come. */
	(void)dlm_req_fsm_peer_gone(&d->req, csid);
	/* ... and the lock state its departure orphans is swept by the engine.
	 * NOT YET the master-side LKBs this node holds FOR it: that is rd
	 * vms-4d3 (vms-c27 condition 3), a rung of its own. */
	vms_lock_dlm_member_departed((uint32_t)csid, &found);
}

static void dlm_arm_bind_role(struct vms_dlm_scs *d)
{
	d->role.transition_begin = dlm_arm_transition_begin;
	d->role.handle_request   = dlm_arm_handle_request;
	d->role.transition_end   = dlm_arm_transition_end;
	d->role.member_departed  = dlm_arm_member_departed;
	d->role.ctx              = d;
}

/* ==========================================================================
 * 7. The beat -- the requester arm's retransmit ladder
 * ========================================================================== */

static void dlm_arm_arm_beat(struct vms_dlm_scs *d)
{
	(void)cf_timer_arm(d->cl->fork, CF_OWNER_DLM, DLM_ARM_TIMER_BEAT, 0u,
			   DLM_ARM_BEAT_MS);
}

static void dlm_arm_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_dlm_scs *d = (struct vms_dlm_scs *)ctx;

	if (d == NULL || w == NULL)
		return;
	if (w->kind == (uint16_t)CF_WORK_TIMER) {
		if (w->arg0 != DLM_ARM_TIMER_BEAT)
			return;   /* an identity this layer never armed */
		dlm_arm_sync_local_csid(d);
		(void)dlm_req_fsm_tick(&d->req);
		dlm_arm_arm_beat(d);
		return;
	}
	/* A release names a STAGED snapshot (arg0 = slot, arg1 = generation);
	 * every other post names a LOCK to rebuild from (arg0 = lock id, arg1 =
	 * destination). Two work kinds, two sources, neither of them a wire
	 * value carried across the context switch. */
	if (w->kind == (uint16_t)DLM_ARM_WORK_POST_DEQ) {
		dlm_arm_run_release(d, w->arg0, w->arg1);
		return;
	}
	dlm_arm_run_post(d, w->kind, w->arg0, w->arg1);
}

/* ==========================================================================
 * 8. Lifecycle
 * ========================================================================== */

int vms_dlm_scs_start(struct vms_cluster *cl)
{
	struct vms_dlm_scs *d;

	if (cl == NULL)
		return (int)SS__BADPARAM;
	if (cl->dlm != NULL)
		return (int)SS__NORMAL;          /* already up: idempotent */
	if (cl->fork == NULL || cl->pe == NULL || cl->scs == NULL ||
	    cl->cnxman == NULL)
		return (int)SS__NOSUCHDEV;       /* Rule 9: no layer beneath */

	d = (struct vms_dlm_scs *)exec_zalloc(sizeof(*d));
	if (d == NULL)
		return (int)SS__INSFMEM;
	d->cl = cl;

	dlm_arm_bind_req_ops(d);
	dlm_arm_bind_engine_ops(d);
	dlm_arm_bind_role(d);
	dlm_req_fsm_init(&d->req, &d->req_ops);

	/* The release queue, empty, BEFORE the engine's ops are installed below
	 * -- the first $DEQ can arrive the instant they are. */
	exec_lock_init(&d->relq_lock);
	dlm_relq_init(&d->relq);

	(void)cf_set_work_handler(cl->fork, CF_OWNER_DLM, dlm_arm_work_handler,
				  d);
	cl->dlm = d;

	/* Both directions at once, and in this order: the connection manager
	 * may start handing us messages the instant it knows about us, and the
	 * engine may start posting the instant its ops exist. */
	cnxman_set_dlm(cl, &d->role);
	vms_lock_dlm_set_requester_ops(&d->eng_ops);

	dlm_arm_arm_beat(d);
	return (int)SS__NORMAL;
}

void vms_dlm_scs_stop(struct vms_cluster *cl)
{
	struct vms_dlm_scs *d;

	if (cl == NULL || cl->dlm == NULL)
		return;
	d = cl->dlm;

	/*
	 * Detach both directions BEFORE anything is freed, so no message and no
	 * post can reach a context that is going away. With the ops gone the
	 * engine is purely local again -- which is a real VMS configuration and
	 * an honest degradation, not a simulation: a node with no cluster still
	 * locks, and a remote-mastered resource is refused honestly.
	 */
	vms_lock_dlm_set_requester_ops(NULL);
	cnxman_set_dlm(cl, NULL);
	vms_lock_dlm_set_delivery_proc(NULL);

	cf_timer_cancel(cl->fork, CF_OWNER_DLM, DLM_ARM_TIMER_BEAT, 0u);
	(void)cf_set_work_handler(cl->fork, CF_OWNER_DLM, NULL, NULL);

	/* A release still staged when the cluster stops is NOT transmitted:
	 * there is no connection left to transmit it on, and inventing one is
	 * the thing this stack does not do. It dies with the queue, and the
	 * master's view is the departure/rebuild machinery's to reconcile. */
	exec_lock_destroy(&d->relq_lock);

	cl->dlm = NULL;
	exec_free(d);
}

/* ==========================================================================
 * 9. The outbound entry vms_dlm_scs.h §5 publishes
 *
 * Kept because it is this layer's stated interface, and implemented as what it
 * has always been: ask the connection manager to transmit on the connection it
 * holds for that member. `lkid` is the (req_csid, req_lkid) idempotency key's
 * half; it is not placed on the wire here -- the body the FSM built already
 * carries it, read out of the LKB at build time.
 * ========================================================================== */
int vms_dlm_scs_post_request(struct vms_cluster *cl, vms_csid_t dst_csid,
			     uint32_t lkid, uint8_t opcode,
			     const uint8_t *body, uint32_t len)
{
	(void)opcode;
	if (cl == NULL || cl->dlm == NULL || body == NULL)
		return (int)SS__BADPARAM;
	if (lkid == VMS_DLM_LKID_UNSET)
		return (int)SS__BADPARAM;   /* never a placeholder handle */
	return cnxman_dlm_send(cl, dst_csid, body, len) == 0 ?
	       (int)SS__NORMAL : (int)SS__UNSUPPORTED;
}

/* ==========================================================================
 * 10. Readback -- the same values a diagnostic projects (INV-6)
 * ========================================================================== */

/*
 * THE EMIT LEDGER (rd vms-94c). Every field is a counter THIS arm incremented
 * at the moment the thing happened -- `d->` for the master half, `d->req` for
 * the requester FSM -- copied out, never recomputed and never derived from a
 * frame count. Caller holds the fork mutex.
 *
 * Why it is projected at all: a pcap proves a byte reached the segment; only
 * these prove that THIS executive's arm is what put it there.
 */
static void dlm_arm_project_emits(const struct vms_dlm_scs *d,
				  struct vms_dlm_scs_view *out)
{
	out->releases_sent       = d->req.releases_sent;
	out->releases_no_wire_op = d->req.releases_no_wire_op;
	out->blkasts_sent        = d->blkasts_sent;
	out->blkasts_no_wire_op  = d->blkasts_no_wire_op;
	out->blkasts_received    = d->req.blkasts_rx;
	out->blkasts_delivered   = d->req.blkasts_delivered;
	out->blkasts_unparsed    = d->req.blkasts_unparsed;
	out->releases_received   = d->releases_received;
	out->valblk_writes_received = d->valblk_writes_received;
	out->releases_refused    = d->releases_refused;
	out->deferred_grants_owed = d->deferred_grants_no_wire_op;
	out->queued_no_reply     = d->queued_no_reply;
	out->unparsed            = d->unparsed;
	out->foreign_refused     = d->foreign_refused;
	out->posts_queued        = d->posts_queued;
	out->posts_unqueued      = d->posts_unqueued;
	out->posts_lock_gone     = d->posts_lock_gone;
	out->posts_refused       = d->posts_refused;
}

/* The arm's own state and the request/grant tallies it has really seen. */
static void dlm_arm_project_state(const struct vms_cluster *cl,
				  const struct vms_dlm_scs *d,
				  struct vms_dlm_scs_view *out)
{
	out->lockdirwt          = (uint8_t)cl->params.lockdirwt;
	out->rebuild_generation = vms_ldwv_generation(&cl->club.ldwv);
	/* "the VMS$VAXcluster CDT carrying cat-02 is open" -- read as the CLUB's
	 * own member count being more than this node, which is exactly when
	 * there is a peer connection for DLM traffic to ride. */
	out->connected          = (uint8_t)(cl->club.cluster_nodes > 1u);
	out->proxy_lkbs         = dlm_req_fsm_outstanding(&d->req);
	out->req_sent           = d->req.requests_sent + d->req.lookups_sent;
	out->req_received       = d->req_received;
	out->grants_sent        = d->grants_sent;
	out->grants_received    = d->req.grants_rx;
	out->declined           = d->declined + d->foreign_refused +
				  d->no_delivery_proc;
}

int vms_dlm_scs_snapshot(struct vms_cluster *cl, struct vms_dlm_scs_view *out)
{
	struct vms_dlm_scs *d;

	if (cl == NULL || out == NULL)
		return (int)SS__BADPARAM;
	memset(out, 0, sizeof(*out));
	if (cl->dlm == NULL)
		return (int)SS__NOSUCHDEV;
	d = cl->dlm;

	vms_cluster_fork_enter(cl);
	dlm_arm_project_state(cl, d, out);
	dlm_arm_project_emits(d, out);
	/* The SECOND, independent reading of the same traffic, taken one layer
	 * down in the connection manager (vms_cnxman.h). Not derived from the
	 * arm's counters above: a disagreement between the two is information a
	 * cross-node proof is entitled to see. */
	cnxman_project_dlm_leg(cl, out);
	vms_cluster_fork_leave(cl);
	return (int)SS__NORMAL;
}
