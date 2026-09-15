/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_deq_reachable.c - rd vms-49f8's R1: A CROSS-NODE $DEQ REALLY BUILDS
 * AN op-0x03 FRAME.
 *
 * ==========================================================================
 * THE BUG THIS FILE EXISTS TO MAKE UNREPEATABLE
 * ==========================================================================
 * rd vms-d7a3 (#1165) gave the requester arm a real op-0x03 $DEQ emit, and
 * tests/cluster/host/test_dlm_requester.c proved the emit itself: hand the FSM
 * a release post and a grounded frame comes out. What no test could see was
 * that NOTHING IN PRODUCTION EVER HANDED IT ONE. Measured on the live 2-node
 * rig (PR #1174): `releases_sent=0`, `releases_no_wire_op=0`,
 * `posts_lock_gone=2` -- not a gate refusing, a caller that never arrived.
 *
 * The root cause is a lifetime, not a gate. The glue used to queue a post to
 * the fork thread carrying ONLY the lock id and rebuild it there through
 * `refill_post` -- correct for an ENQ, a CONVERT and every retransmit, and
 * impossible for a release, because the $DEQ *is* the destruction of the proxy
 * LKB. By the time the fork thread ran there was no lock left to read.
 * dlm_release_batch_post (image rundown) died identically.
 *
 * ==========================================================================
 * WHAT IS REAL IN THIS FILE, AND WHY THAT IS THE WHOLE POINT
 * ==========================================================================
 * Four shipping objects, in one host process, on the path a real $DEQ takes:
 *
 *   the ENGINE        src/kernel-core/vms_lock.c, the REAL lock manager on
 *                     FC-P4.9's host backend. A real proxy LKB is created by a
 *                     real $ENQ, granted by a real inbound grant, and destroyed
 *                     by a real $DEQ. The post the arm receives is the one
 *                     dlm_proxy_fill_post() read out of that LKB.
 *   the RELEASE QUEUE src/kernel-core/vms_dlm_scs_fsm.c's `struct dlm_relq` --
 *                     the object under test, staged from the engine's post
 *                     before the LKB dies and claimed after it is gone.
 *   the REQUESTER FSM src/kernel-core/vms_dlm_scs_fsm.c, driven exactly as the
 *                     glue drives it.
 *   the CODEC         src/kernel-core/vms_cluster_codec_dlm.c -- every byte
 *                     asserted here is built AND parsed by the shipping codec,
 *                     never by byte arithmetic.
 *
 * What is NOT real is the substrate: the fork thread is a list this file
 * drains, and the executive lock around the queue is not needed in one thread.
 * The GLUE that binds those two (src/kernel-core/vms_dlm_scs.c) is not
 * host-linkable -- it names exec_kbackend.h and the FC-P0.5 fork API -- so its
 * wiring and ITS ORDER are pinned by source-scan in test_dlm_scs_arm.c, the
 * two-proof shape test_cnxman_glue.c established. This file proves the half a
 * scan cannot: that a real release, of a real lock, produces a real frame whose
 * every field traces to the lock database.
 *
 * ==========================================================================
 * THE ASSERTION WITH TEETH
 * ==========================================================================
 * `check_deq_traces_to_lkb` compares the emitted frame against a reading of the
 * LKB taken through the engine's OWN refill door immediately before the $DEQ.
 * So the claim is not "the fields look plausible": the frame is the image of an
 * executive read. And the root cause is pinned as its own assertion -- after
 * the $DEQ that same door answers SS$_IVLOCKID, which is exactly why a frame
 * built from a refill could never have existed.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_proxy.h"    /* the requester seam the engine posts through   */
#include "vms_dlm_scs_fsm.h"  /* the requester FSM + the release queue         */
#include "vms_frame_compose.h"

#include <stdio.h>
#include <string.h>

/* The executive globals vms_lock.c reads, and the AST hook it calls. */
uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_LOCAL     1u   /* this node                                   */
#define CSID_DIRECTORY 2u   /* the member the weight vector names          */
#define CSID_MASTER    3u   /* the node that masters the tree              */

/* ==========================================================================
 * 1. The arm, wired the way src/kernel-core/vms_dlm_scs.c wires it
 *
 * One difference and it is the substrate only: `fork_post` appends to an array
 * instead of cf_post()ing to the fork thread, and `fork_drain` is what the fork
 * thread's work handler does. Everything between the engine and the wire is the
 * shipping object.
 * ========================================================================== */

#define ARM_WORK_MAX 40u
#define ARM_SENT_MAX 40u

struct arm_work {
	uint16_t kind;    /* VMS_DLM_POST_ENQ / _CONVERT / _DEQ              */
	uint32_t arg0;    /* a lock id, or a release queue SLOT              */
	uint32_t arg1;    /* a destination, or a release queue GENERATION    */
};

struct arm_frame {
	uint32_t dst;
	uint8_t  body[DLM_REQ_BODY_LEN];
	uint32_t len;
};

struct arm {
	struct dlm_relq     relq;
	struct dlm_req_fsm  fsm;
	struct dlm_req_ops  ops;

	struct arm_work     work[ARM_WORK_MAX];
	uint32_t            n_work;

	struct arm_frame    sent[ARM_SENT_MAX];
	uint32_t            n_sent;

	/* the glue's own counters, mirrored so this file can assert them */
	uint32_t releases_staged;
	uint32_t releases_no_slot;
	uint32_t releases_stale;
	uint32_t posts_lock_gone;
	uint32_t posts_refused;
	uint32_t posts_unqueued;

	int      all_ovmx;      /* vms_ldwv_all_ovmx, as the CM answers it    */
	int      send_fails;    /* RULE C: the CM refused the destination     */
	uint32_t now_ms;
	uint32_t dir_csid;
	uint32_t dir_gen;
};

static struct arm a;

/* ---- the FSM's doors: every engine door is the REAL engine entry point --- */

static int arm_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	struct arm_frame *f;

	(void)ctx;
	if (a.send_fails || a.n_sent >= ARM_SENT_MAX)
		return -1;
	f = &a.sent[a.n_sent++];
	memset(f, 0, sizeof(*f));
	f->dst = (uint32_t)dst;
	f->len = len > DLM_REQ_BODY_LEN ? DLM_REQ_BODY_LEN : len;
	memcpy(f->body, body, f->len);
	return 0;
}

static int arm_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		      vms_csid_t dst_csid, struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, (uint32_t)dst_csid,
					      out) == SS__NORMAL ? 0 : -1;
}

static int arm_dir_resolve(void *ctx, uint16_t hash16, vms_csid_t *out_csid)
{
	(void)ctx;
	(void)hash16;
	*out_csid = (vms_csid_t)a.dir_csid;
	return 0;
}

static uint32_t arm_dir_generation(void *ctx)
{
	(void)ctx;
	return a.dir_gen;
}

static int arm_all_ovmx(void *ctx)
{
	(void)ctx;
	return a.all_ovmx;
}

static int arm_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			     vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid,
					  (uint32_t)master_csid) == SS__NORMAL ?
	       0 : -1;
}

static int arm_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) == SS__NORMAL ?
	       0 : -1;
}

static int arm_grant_recv(void *ctx, const struct vms_dlm_proxy_grant *g)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(g) == SS__NORMAL ? 0 : -1;
}

static int arm_blkast(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) == SS__NORMAL ? 0 : -1;
}

static int arm_learn(void *ctx, const char *resnam, uint16_t hash16)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, hash16) == SS__NORMAL ? 0 : -1;
}

static void arm_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	(void)ctx;
	(void)why;
	(void)vms_lock_dlm_proxy_fail(req_lkid, SS__UNSUPPORTED);
}

static uint32_t arm_now(void *ctx)
{
	(void)ctx;
	return a.now_ms;
}

static void arm_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

/* ---- the ENGINE's door into the arm: vms_dlm_proxy.h's `post` ----------- */

/* cf_post's stand-in. Nonzero means the fork queue would not take it. */
static int arm_fork_post(uint16_t kind, uint32_t arg0, uint32_t arg1)
{
	if (a.n_work >= ARM_WORK_MAX) {
		a.posts_unqueued++;
		return -1;
	}
	a.work[a.n_work].kind = kind;
	a.work[a.n_work].arg0 = arg0;
	a.work[a.n_work].arg1 = arg1;
	a.n_work++;
	return 0;
}

/*
 * A RELEASE IS STAGED, NOT REBUILT -- dlm_arm_post_release(), with cf_post
 * replaced. `p` is the post the engine filled from the LIVE LKB, a few
 * instructions before vms_deq_core tears that LKB down.
 */
static uint32_t arm_post_release(const struct vms_dlm_proxy_post *p)
{
	uint32_t slot = 0u, seq = 0u;
	enum dlm_req_status st;

	st = dlm_relq_stage(&a.relq, p, &slot, &seq);
	if (st == DLM_REQ_E_NOSLOT) {
		a.releases_no_slot++;
		return SS__INSFMEM;
	}
	if (st != DLM_REQ_OK)
		return SS__BADPARAM;
	a.releases_staged++;

	if (arm_fork_post((uint16_t)VMS_DLM_POST_DEQ, slot, seq) != 0) {
		dlm_relq_abandon(&a.relq, slot, seq);
		return SS__INSFMEM;
	}
	return SS__NORMAL;
}

static uint32_t arm_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p == NULL || p->req_lkid == VMS_DLM_LKID_UNSET)
		return SS__BADPARAM;
	if (p->op == VMS_DLM_POST_DEQ)
		return arm_post_release(p);
	return arm_fork_post((uint16_t)p->op, p->req_lkid, p->dst_csid) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__INSFMEM;
}

/* ---- the fork thread's work handler ------------------------------------ */

static void arm_run_release(uint32_t slot, uint32_t seq)
{
	struct vms_dlm_proxy_post p;

	if (dlm_relq_claim(&a.relq, slot, seq, &p) != DLM_REQ_OK) {
		a.releases_stale++;
		return;
	}
	if (dlm_req_fsm_post(&a.fsm, &p) != DLM_REQ_OK)
		a.posts_refused++;
}

static void arm_run_post(uint16_t kind, uint32_t req_lkid, uint32_t dst_csid)
{
	struct vms_dlm_proxy_post p;

	if (arm_refill(NULL, req_lkid, kind, (vms_csid_t)dst_csid, &p) != 0) {
		a.posts_lock_gone++;
		return;
	}
	if (dlm_req_fsm_post(&a.fsm, &p) != DLM_REQ_OK)
		a.posts_refused++;
}

static void fork_drain(void)
{
	uint32_t i, n = a.n_work;

	for (i = 0; i < n; i++) {
		if (a.work[i].kind == (uint16_t)VMS_DLM_POST_DEQ)
			arm_run_release(a.work[i].arg0, a.work[i].arg1);
		else
			arm_run_post(a.work[i].kind, a.work[i].arg0,
				     a.work[i].arg1);
	}
	a.n_work = 0;
}

/* ==========================================================================
 * 2. Bring the whole path up: the real engine, the real ops, an empty arm
 * ========================================================================== */

static struct vms_dlm_requester_ops eng_ops;

static void arm_reset(void)
{
	memset(&a, 0, sizeof(a));
	a.all_ovmx = 1;            /* an OVMX-only cluster, the cleared case  */
	a.now_ms = 1000u;
	a.dir_csid = CSID_DIRECTORY;
	a.dir_gen = 1u;

	dlm_relq_init(&a.relq);

	memset(&a.ops, 0, sizeof(a.ops));
	a.ops.send            = arm_send;
	a.ops.refill_post     = arm_refill;
	a.ops.dir_resolve     = arm_dir_resolve;
	a.ops.dir_generation  = arm_dir_generation;
	a.ops.all_ovmx        = arm_all_ovmx;
	a.ops.record_master   = arm_record_master;
	a.ops.assume_mastery  = arm_assume;
	a.ops.grant_recv      = arm_grant_recv;
	a.ops.blkast_deliver  = arm_blkast;
	a.ops.learn_dir_hash  = arm_learn;
	a.ops.fail            = arm_fail;
	a.ops.now_ms          = arm_now;
	a.ops.log             = arm_log;
	a.ops.ctx             = &a;
	dlm_req_fsm_init(&a.fsm, &a.ops);
}

static uint32_t eng_dir_resolve(void *ctx, uint16_t hash16, uint32_t *out_csid)
{
	(void)ctx;
	(void)hash16;
	*out_csid = a.dir_csid;
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return a.dir_gen;
}

static void engine_up(void)
{
	vms_local_csid = CSID_LOCAL;
	arm_reset();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	memset(&eng_ops, 0, sizeof(eng_ops));
	eng_ops.post           = arm_post;
	eng_ops.dir_resolve    = eng_dir_resolve;
	eng_ops.dir_generation = eng_dir_generation;
	eng_ops.ctx            = &a;
	vms_lock_dlm_set_requester_ops(&eng_ops);
}

static void engine_down(void)
{
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

static void proc_init(struct vms_proc *p)
{
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

/*
 * Play the wire: give the executive the cluster's hash for `resnam`. Produced
 * HERE (this file stands in for the sending system), never by the executive --
 * the FC-P4.3 rule the whole directory path rests on.
 */
static void wire_learn(const char *resnam)
{
	uint16_t h = 0x4000u;
	const char *c;

	for (c = resnam; *c != '\0'; c++)
		h = (uint16_t)((h << 1) ^ (uint8_t)*c);
	(void)vms_lock_dlm_learn_dir_hash(resnam, h);
}

static uint32_t do_enq(struct vms_proc *proc, const char *resnam,
		       uint32_t lkmode, uint32_t *lkid_out)
{
	struct vms_enq_args e;

	memset(&e, 0, sizeof(e));
	e.lkmode = lkmode;
	strscpy(e.resnam, resnam, sizeof(e.resnam));
	wire_learn(resnam);
	vms_ioctl_enq(proc, (unsigned long)(void *)&e);
	if (lkid_out)
		*lkid_out = e.lkid;
	return e.status;
}

static uint32_t do_deq(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_deq_args d;

	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&d);
	return d.status;
}

/* The master's own grant, delivered through the engine's real inbound path --
 * which is what puts the master's handle and the granted mode ON THE LKB. */
static uint32_t deliver_grant(struct vms_proc *proc, uint32_t req_lkid,
			      uint32_t mode, uint32_t master_lkid,
			      const char *resnam)
{
	struct vms_dlm_xnode_args x;

	memset(&x, 0, sizeof(x));
	x.op = VMS_DLM_OP_GRANT;
	x.lkmode = mode;
	x.req_lkid = req_lkid;
	x.master_lkid = master_lkid;
	x.req_csid = CSID_LOCAL;
	x.master_csid = CSID_MASTER;
	strscpy(x.resnam, resnam, sizeof(x.resnam));
	return vms_lock_dlm_xnode_dispatch(proc, &x);
}

/* One granted cross-node lock, held at `mode` with the master's handle on it.
 * Returns the proxy's lock id. */
static uint32_t granted_proxy(struct vms_proc *proc, const char *resnam,
			      uint32_t mode, uint32_t master_lkid)
{
	uint32_t lkid = 0;

	(void)do_enq(proc, resnam, mode, &lkid);
	fork_drain();                       /* the ENQ leaves as a real frame */
	(void)deliver_grant(proc, lkid, mode, master_lkid, resnam);
	return lkid;
}

/* ==========================================================================
 * 3. Frame inspection -- through the SHIPPING codec, never byte arithmetic
 * ========================================================================== */

static uint32_t splice(const struct arm_frame *s, uint8_t *frame)
{
	struct vms_cm_link link;
	uint32_t written = 0;

	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	memcpy(frame + VMS_OFF_SYSAP_BODY, s->body, s->len);
	return VMS_CM_FRAME_LEN;
}

static int parse_deq(const struct arm_frame *s, struct vms_dlm_deq *out)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_frame_info fi;
	uint32_t len = splice(s, frame);

	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_dlm_deq_parse(frame, len, &fi, out) != VMS_CODEC_OK)
		return -1;
	return 0;
}

static uint8_t sent_opcode(const struct arm_frame *s)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	vms_wire_view_t v;
	uint32_t len = splice(s, frame);

	vms_wire_view_init(&v, frame, len);
	return vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
}

/*
 * *** THE ASSERTION WITH TEETH. *** The emitted release is compared to a
 * reading of the LKB taken through the ENGINE'S OWN refill door immediately
 * before the $DEQ. Three fields, three executive reads: a frame that carried a
 * templated mode, a remembered handle or anything but this node's own lock id
 * reddens here.
 */
static void check_deq_traces_to_lkb(const struct arm_frame *s,
				    const struct vms_dlm_proxy_post *lkb,
				    const char *label)
{
	struct vms_dlm_deq d;
	char what[200];

	snprintf(what, sizeof(what), "%s: the frame parses as a grounded op-0x03",
		 label);
	if (parse_deq(s, &d) != 0) {
		ct_check(0, what);
		return;
	}
	ct_check(1, what);

	snprintf(what, sizeof(what),
		 "%s: body[20:24] is the proxy LKB's OWN handle (0x%08x), as the "
		 "executive minted it", label, (unsigned)lkb->req_lkid);
	ct_check_eq_u32(d.req_lkid, lkb->req_lkid, what);

	snprintf(what, sizeof(what),
		 "%s: body[24:28] is the MASTER's handle (0x%08x), as the "
		 "master's own grant recorded it on the LKB", label,
		 (unsigned)lkb->master_lkid);
	ct_check_eq_u32(d.master_lkid, lkb->master_lkid, what);

	snprintf(what, sizeof(what),
		 "%s: body[30] is the mode the LKB really held (%u), not a "
		 "template", label, (unsigned)lkb->lkmode);
	ct_check_eq_u32(d.mode, lkb->lkmode, what);
}

/* Read the LKB the way the arm's refill door does. Returns 0 when the lock is
 * gone -- which, after a $DEQ, is the whole root cause in one call. */
static int read_lkb(uint32_t lkid, uint32_t dst, struct vms_dlm_proxy_post *out)
{
	memset(out, 0, sizeof(*out));
	return vms_lock_dlm_proxy_refill_post(lkid, VMS_DLM_POST_DEQ, dst, out)
	       == SS__NORMAL;
}

/* ==========================================================================
 * 4. THE REACHABILITY PROOF
 * ========================================================================== */
static void test_deq_reaches_the_wire(void)
{
	const uint32_t MASTER_LKID = 0x00C0FFEEu;
	struct vms_dlm_proxy_post before;
	struct vms_proc proc;
	uint32_t lkid, st;

	printf("-- a cross-node $DEQ on a real proxy LKB BUILDS a real op-0x03 "
	       "(rd vms-49f8)\n");
	engine_up();
	proc_init(&proc);

	lkid = granted_proxy(&proc, "DEQREACH1", LCK_K_PRMODE, MASTER_LKID);
	ct_check(lkid != 0, "a real proxy LKB exists, granted by a real master");

	/* What the lock database holds, one instant before the release. */
	ct_check(read_lkb(lkid, CSID_MASTER, &before),
		 "the engine reads the LKB: this is the release's ONLY source");
	ct_check_eq_u32(before.master_lkid, MASTER_LKID,
			"  it carries the master's handle from the GRANT");
	ct_check_eq_u32(before.lkmode, LCK_K_PRMODE,
			"  and the mode the lock is really held at (PR)");

	/* ---- the release ---- */
	st = do_deq(&proc, lkid);
	ct_check_eq_u32(st, SS__NORMAL, "$DEQ on the proxy is accepted");

	/*
	 * *** THE ROOT CAUSE, PINNED AS AN ASSERTION. *** The old fork-thread
	 * path rebuilt the post from the lock database at transmit time. This is
	 * what that rebuild would find now, and it is why the op-0x03 emit had
	 * no reachable caller: the release destroyed its own source.
	 */
	{
		struct vms_dlm_proxy_post gone;

		ct_check(!read_lkb(lkid, CSID_MASTER, &gone),
			 "*** the proxy LKB is GONE: a refill at transmit time "
			 "answers SS$_IVLOCKID -- the root cause of vms-49f8 ***");
	}

	ct_check_eq_u32(dlm_relq_pending(&a.relq), 1u,
			"*** the release was SNAPSHOTTED before the teardown: "
			"one staged, waiting for the fork thread ***");
	ct_check_eq_u32(a.releases_staged, 1u, "  counted as staged");

	/* ---- the fork thread ---- */
	fork_drain();

	ct_check_eq_u32(a.n_sent, 2u,
			"two frames total: the ENQ, and now the RELEASE");
	ct_check_eq_u32(sent_opcode(&a.sent[1]), VMS_DLM_WIREOP_DEQ,
			"*** the second frame is a grounded op-0x03 $DEQ ***");
	ct_check_eq_u32(a.sent[1].dst, CSID_MASTER,
			"  addressed to the MASTER, never a directory node");
	check_deq_traces_to_lkb(&a.sent[1], &before, "the release");

	ct_check_eq_u32(a.fsm.releases_sent, 1u,
			"*** releases_sent rose: the emit has a REACHABLE "
			"caller (it read 0 on the live rig) ***");
	ct_check_eq_u32(a.fsm.releases_no_wire_op, 0u,
			"  and nothing was counted as a gap");
	ct_check_eq_u32(a.posts_lock_gone, 0u,
			"*** posts_lock_gone did NOT rise: the release never "
			"went looking for a lock that no longer exists ***");
	ct_check_eq_u32(a.relq.claimed, 1u, "the snapshot was claimed once");
	ct_check_eq_u32(dlm_relq_pending(&a.relq), 0u,
			"  and the slot was given back");
	ct_check_eq_u32(a.releases_stale, 0u, "no stale claim");

	engine_down();
}

/* ==========================================================================
 * 5. IMAGE RUNDOWN reaches the emit too (the second dead path)
 *
 * dlm_release_batch_post() collects a release from the LIVE LKB under
 * res->lock, drops proc->lock_list_lock and posts it -- and then died exactly
 * as the interactive $DEQ did, because it reached the same glue. One fix, both
 * paths, and this is the half a $DEQ test would not have caught.
 * ========================================================================== */
static void test_rundown_release_reaches_the_wire(void)
{
	const uint32_t MASTER_LKID = 0x0BADF00Du;
	struct vms_dlm_proxy_post before;
	struct vms_proc proc;
	uint32_t lkid;

	printf("-- IMAGE RUNDOWN's release reaches the same emit "
	       "(dlm_release_batch_post)\n");
	engine_up();
	proc_init(&proc);

	lkid = granted_proxy(&proc, "DEQRUNDOWN", LCK_K_EXMODE, MASTER_LKID);
	ct_check(read_lkb(lkid, CSID_MASTER, &before),
		 "one granted proxy LKB, held by the process");

	vms_proc_rundown_locks(&proc, 0u);   /* the REAL image-rundown sweep */

	ct_check_eq_u32(a.releases_staged, 1u,
			"rundown STAGED the release from the live LKB");
	{
		struct vms_dlm_proxy_post gone;

		ct_check(!read_lkb(lkid, CSID_MASTER, &gone),
			 "  and then tore the LKB down, as rundown must");
	}

	fork_drain();
	ct_check_eq_u32(a.n_sent, 2u, "the ENQ, then the rundown RELEASE");
	ct_check_eq_u32(sent_opcode(&a.sent[1]), VMS_DLM_WIREOP_DEQ,
			"*** rundown emitted a real op-0x03 $DEQ ***");
	check_deq_traces_to_lkb(&a.sent[1], &before, "the rundown release");
	ct_check_eq_u32(a.fsm.releases_sent, 1u, "counted as sent");
	ct_check_eq_u32(a.posts_lock_gone, 0u,
			"and NOT as a lock that vanished under the arm");

	engine_down();
}

/* ==========================================================================
 * 6. THE NEGATIVE CONTROLS -- every way a staged release must NOT reach a wire
 *
 * The staging changes WHERE the release's fields come from. It changes NOTHING
 * about who may be sent one, and each gate is checked by the pair (nothing
 * sent, the refusal counted): a gate that only stops the frame, without saying
 * so, is indistinguishable from the bug this item fixed. Mutating any gate
 * reddens this function.
 * ========================================================================== */

/* Drive one real granted proxy to the point of release, then $DEQ it: the
 * release is staged. Returns the lock id. */
static uint32_t stage_one_release(struct vms_proc *proc, const char *resnam,
				  uint32_t master_lkid)
{
	uint32_t lkid = granted_proxy(proc, resnam, LCK_K_PRMODE, master_lkid);

	(void)do_deq(proc, lkid);
	return lkid;
}

static void expect_nothing_sent(uint32_t n_before, const char *label)
{
	char what[200];

	snprintf(what, sizeof(what), "%s: *** NOTHING went on the wire ***",
		 label);
	ct_check_eq_u32(a.n_sent, n_before, what);

	snprintf(what, sizeof(what), "%s: the refusal is COUNTED", label);
	ct_check_eq_u32(a.fsm.releases_no_wire_op, 1u, what);

	snprintf(what, sizeof(what), "%s: and nothing is counted as sent",
		 label);
	ct_check_eq_u32(a.fsm.releases_sent, 0u, what);

	snprintf(what, sizeof(what),
		 "%s: the snapshot was claimed and dropped, never left staged",
		 label);
	ct_check_eq_u32(dlm_relq_pending(&a.relq), 0u, what);
}

static void test_gate_all_ovmx(void)
{
	struct vms_proc proc;
	uint32_t n;

	printf("-- NEGATIVE: a release toward a cluster that is not all-OVMX\n");
	engine_up();
	proc_init(&proc);

	(void)stage_one_release(&proc, "DEQGATE1", 0x0777u);
	n = a.n_sent;
	a.all_ovmx = 0;                  /* a member cannot be proven ours */
	fork_drain();
	expect_nothing_sent(n, "the all-OVMX gate (a mixed cluster)");

	engine_down();
}

static void test_gate_rule_c(void)
{
	struct vms_proc proc;
	uint32_t n;

	printf("-- NEGATIVE: RULE C -- the connection manager refuses the "
	       "destination\n");
	engine_up();
	proc_init(&proc);

	(void)stage_one_release(&proc, "DEQGATE2", 0x0778u);
	n = a.n_sent;
	a.send_fails = 1;                /* what an unproven peer looks like */
	fork_drain();
	expect_nothing_sent(n, "RULE C (the peer is not proven ours)");
	ct_check_eq_u32(a.fsm.send_failures, 1u,
			"  the connection manager's own refusal is counted too");

	engine_down();
}

static void test_gate_zero_master_lkid(void)
{
	struct vms_proc proc;
	uint32_t lkid = 0, n;

	printf("-- NEGATIVE: a release naming lock 0 -- the fc8540ae refusal\n");
	engine_up();
	proc_init(&proc);

	/*
	 * The directory answered "the master is X" and the master has not
	 * granted yet, so the LKB carries a real master CSID and NO master
	 * handle. That is the real configuration in which master_lkid is 0 --
	 * not a value anyone chose.
	 */
	(void)do_enq(&proc, "DEQGATE3", LCK_K_PRMODE, &lkid);
	fork_drain();
	ct_check_eq_u32(vms_lock_dlm_record_master("DEQGATE3", lkid,
						   CSID_MASTER),
			(uint32_t)SS__NORMAL,
			"the directory named the master; the master has not "
			"granted, so master_lkid is still 0");
	{
		struct vms_dlm_proxy_post p;

		ct_check(read_lkb(lkid, CSID_MASTER, &p) &&
			 p.master_lkid == VMS_DLM_LKID_UNSET,
			 "  confirmed out of the lock database, not assumed");
	}

	n = a.n_sent;
	(void)do_deq(&proc, lkid);
	ct_check_eq_u32(a.releases_staged, 1u,
			"the release is staged: the refusal is the CODEC's, "
			"not a silent drop upstream of it");
	fork_drain();
	expect_nothing_sent(n, "a release naming lock 0 (fc8540ae)");
	ct_check_eq_u32(a.fsm.codec_failures, 1u,
			"  the codec REFUSED to build it -- the placeholder "
			"that bugchecked a real VAX cannot reach a wire");

	engine_down();
}

/* ==========================================================================
 * 7. THE QUEUE's OWN REFUSALS -- full, and claimed twice
 *
 * Both are places this object declines rather than invents: a full queue stages
 * NOTHING (an evicted release is a lock the master still believes we hold), and
 * a second claim of one staging emits NOTHING (a release is transmitted once or
 * not at all).
 * ========================================================================== */
static void test_queue_full_is_an_honest_refusal(void)
{
	struct vms_proc proc;
	char name[32];
	uint32_t lkid[DLM_RELQ_SLOTS + 1u];
	uint32_t i, st;

	printf("-- the release queue FULL: an honest counted refusal, never an "
	       "evicted release\n");
	engine_up();
	proc_init(&proc);

	for (i = 0; i < DLM_RELQ_SLOTS + 1u; i++) {
		snprintf(name, sizeof(name), "DEQFULL%02u", (unsigned)i);
		lkid[i] = granted_proxy(&proc, name, LCK_K_PRMODE,
					0x1000u + i);
	}
	a.n_sent = 0;   /* the ENQs are not what this scenario is about */

	{
		uint32_t accepted = 0;

		for (i = 0; i < DLM_RELQ_SLOTS; i++) {
			st = do_deq(&proc, lkid[i]);
			if (st == SS__NORMAL)
				accepted++;
		}
		ct_check_eq_u32(accepted, DLM_RELQ_SLOTS,
				"every release stages while there is room");
	}
	ct_check_eq_u32(dlm_relq_pending(&a.relq), DLM_RELQ_SLOTS,
			"the queue is full: every slot holds a real release");

	st = do_deq(&proc, lkid[DLM_RELQ_SLOTS]);
	ct_check_eq_u32(st, (uint32_t)SS__INSFMEM,
			"*** the release past capacity is REFUSED, and the "
			"releaser is told so ***");
	ct_check_eq_u32(a.releases_no_slot, 1u, "  counted");
	ct_check_eq_u32(a.relq.full_refused, 1u, "  by the queue itself");
	ct_check_eq_u32(dlm_relq_pending(&a.relq), DLM_RELQ_SLOTS,
			"  and NOTHING already staged was evicted for it");

	fork_drain();
	ct_check_eq_u32(a.n_sent, DLM_RELQ_SLOTS,
			"every staged release really went out");
	ct_check_eq_u32(a.fsm.releases_sent, DLM_RELQ_SLOTS, "  and is counted");

	engine_down();
}

static void test_a_claim_happens_once(void)
{
	struct vms_proc proc;
	uint32_t slot, seq, n;

	printf("-- a staging is claimed ONCE: a replayed work item emits "
	       "nothing\n");
	engine_up();
	proc_init(&proc);

	(void)stage_one_release(&proc, "DEQONCE1", 0x0779u);
	ct_check_eq_u32(a.n_work, 1u, "one work item names the staging");
	slot = a.work[0].arg0;
	seq  = a.work[0].arg1;

	fork_drain();
	n = a.n_sent;
	ct_check_eq_u32(a.fsm.releases_sent, 1u, "the release went out once");

	/* Replay the SAME work item -- a duplicate the fork queue could hand
	 * back, or a stale one left by a cluster stop. */
	arm_run_release(slot, seq);
	ct_check_eq_u32(a.n_sent, n,
			"*** the replay emitted NOTHING: the slot is gone ***");
	ct_check_eq_u32(a.releases_stale, 1u, "  and the stale claim is counted");
	ct_check_eq_u32(a.relq.stale_refused, 1u, "  by the queue itself");

	/* A generation that never existed names nothing either. */
	arm_run_release(slot, seq + 0x1000u);
	ct_check_eq_u32(a.n_sent, n, "an invented generation names nothing");
	ct_check_eq_u32(a.releases_stale, 2u, "  counted again");

	engine_down();
}

int main(void)
{
	printf("=== test_dlm_deq_reachable (rd vms-49f8: the op-0x03 $DEQ has a "
	       "REACHABLE caller) ===\n");
	test_deq_reaches_the_wire();
	test_rundown_release_reaches_the_wire();
	test_gate_all_ovmx();
	test_gate_rule_c();
	test_gate_zero_master_lkid();
	test_queue_full_is_an_honest_refusal();
	test_a_claim_happens_once();
	return ct_summary("test_dlm_deq_reachable");
}
