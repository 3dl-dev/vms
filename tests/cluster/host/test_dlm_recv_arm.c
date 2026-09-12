/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_recv_arm.c - rd vms-c72's R1: A PEER'S op-0x03 AND op-0x04 REALLY
 * CHANGE THIS EXECUTIVE'S LOCK DATABASE.
 *
 * ==========================================================================
 * THE GAP THIS FILE EXISTS TO MAKE UNREPEATABLE
 * ==========================================================================
 * Both emits fire on the live 2-node rig (#1174: releases_sent>0; #1180:
 * blkasts_sent>0) and the PEER threw both away: blkasts_received=0,
 * blkasts_delivered=0, and the inbound op-0x03 raised `unparsed`. The receiving
 * arm routed only op-0x01/op-0x07 to the engine; everything else fell to a
 * counted decline. The codec could already parse both (#1160) -- the gap was the
 * DISPATCH and the ENGINE-DELIVERY call.
 *
 * A test that only proved "the frame parses" would have been green through that
 * entire gap. So this file asserts the thing a parse cannot: THE LOCK DATABASE
 * MOVED. A released master copy is gone from the engine, and a delivered
 * blocking AST is a real entry on a real holder's AST queue.
 *
 * ==========================================================================
 * WHAT IS REAL HERE
 * ==========================================================================
 *   the ENGINE   src/kernel-core/vms_lock.c on FC-P4.9's host backend -- the
 *                REAL master-side door (vms_lock_dlm_master_serve) and the REAL
 *                holder-side blocking-AST delivery.
 *   the FSM      src/kernel-core/vms_dlm_scs_fsm.c -- the SHIPPING
 *                dlm_req_fsm_blkast_body(), the op-0x04 entry the arm calls.
 *   the CODEC    src/kernel-core/vms_cluster_codec_dlm.c -- every byte fed in
 *                is BUILT by the shipping builder and read by the shipping
 *                parser. This file performs no byte arithmetic except where it
 *                deliberately forges a malformed body, and it says so there.
 *
 * What is NOT here is the glue TU (src/kernel-core/vms_dlm_scs.c): it names
 * exec_kbackend.h and the fork API and is not host-linkable, exactly as
 * test_cnxman_glue.c and test_dlm_deq_reachable.c already document. Its routing
 * -- that op-0x03 and op-0x04 reach these two doors, and that both sit BELOW the
 * RULE C gate -- is pinned by source-scan in test_dlm_scs_arm.c. This file owns
 * the half a scan cannot see: what the doors DO to real lock state.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_master.h"   /* the MASTER-side seam the op-0x03 lands on     */
#include "vms_dlm_proxy.h"    /* the REQUESTER seam the op-0x04 lands on       */
#include "vms_dlm_scs_fsm.h"  /* the requester FSM + its op-0x04 body entry    */
#include "vms_cluster_codec_cm.h"   /* VMS_CM_FRAME_LEN / VMS_CM_BODY_LEN */
#include "vms_cluster_codec_dlm.h"

#include <stdio.h>
#include <string.h>

/* The executive globals vms_lock.c reads, and the AST hook it calls. */
uint32_t vms_local_csid = 1;

static uint32_t ast_arrivals;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
	ast_arrivals++;
}

#define CSID_LOCAL   1u    /* this node: the master in §2, the holder in §1 */
#define CSID_PEER_A  2u    /* the peer that holds, then releases            */
#define CSID_PEER_B  3u    /* a second peer -- the waiter, and the impostor */
#define CSID_MASTER  4u    /* in §1, the node that masters our proxy's tree */

/* ==========================================================================
 * 0. Shared scaffolding
 * ========================================================================== */

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

/* How many user-mode ASTs are queued on a process right now. This is the
 * observable a delivered BLKAST produces, and nothing else in these tests
 * queues one. */
static uint32_t user_asts_pending(const struct vms_proc *p)
{
	return (uint32_t)p->ast[PSL_C_USER].count;
}

/* The BODY of a frame the codec built. The builders write FRAME-absolute, so
 * the body is the splice at VMS_OFF_SYSAP_BODY -- the same one line of offset
 * arithmetic the shipping arm allows itself, and the only one here. */
static const uint8_t *body_of(const uint8_t *frame)
{
	return frame + VMS_OFF_SYSAP_BODY;
}

/* ==========================================================================
 * 1. THE HOLDER SIDE: an inbound op-0x04 fires a REAL user-mode blocking AST
 *
 * This node holds a cross-node lock. The master runs into a conflict and sends
 * the blocking AST this arm now consumes. The proof is not that the body parsed
 * -- it is that the holder's process came out of it with an AST queued.
 * ========================================================================== */

struct holder_arm {
	struct dlm_req_fsm fsm;
	struct dlm_req_ops ops;
	uint32_t           blkast_calls;    /* ops->blkast_deliver invocations */
	uint32_t           last_blkast_lkid;
	uint32_t           now_ms;
	uint8_t            sent[DLM_REQ_BODY_LEN];
	uint32_t           n_sent;
};

static struct holder_arm h;

static int h_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	(void)dst;
	h.n_sent++;
	memcpy(h.sent, body, len > sizeof(h.sent) ? sizeof(h.sent) : len);
	return 0;
}

static int h_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		    vms_csid_t dst_csid, struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, (uint32_t)dst_csid,
					      out) == SS__NORMAL ? 0 : -1;
}

static int h_dir_resolve(void *ctx, uint16_t hash16, vms_csid_t *out_csid)
{
	(void)ctx;
	(void)hash16;
	*out_csid = (vms_csid_t)CSID_MASTER;
	return 0;
}

static uint32_t h_dir_generation(void *ctx) { (void)ctx; return 1u; }
static int h_all_ovmx(void *ctx) { (void)ctx; return 1; }

static int h_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			   vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid,
					  (uint32_t)master_csid) == SS__NORMAL ?
	       0 : -1;
}

static int h_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) == SS__NORMAL ?
	       0 : -1;
}

static int h_grant_recv(void *ctx, const struct vms_dlm_proxy_grant *g)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(g) == SS__NORMAL ? 0 : -1;
}

/*
 * THE DOOR UNDER TEST on this side -- byte-for-byte what the shipping arm binds
 * (`dlm_arm_blkast_deliver`): the REAL engine call, wrapped only to count the
 * invocations so the test can say the dispatch reached it.
 */
static int h_blkast_deliver(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	h.blkast_calls++;
	h.last_blkast_lkid = req_lkid;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) == SS__NORMAL ? 0 : -1;
}

static int h_learn(void *ctx, const char *resnam, uint16_t hash16)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, hash16) == SS__NORMAL ?
	       0 : -1;
}

static void h_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	(void)ctx;
	(void)why;
	(void)vms_lock_dlm_proxy_fail(req_lkid, SS__UNSUPPORTED);
}

static uint32_t h_now(void *ctx) { (void)ctx; return h.now_ms; }
static void h_log(void *ctx, const char *msg) { (void)ctx; (void)msg; }

static uint32_t h_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p == NULL)
		return SS__BADPARAM;
	return dlm_req_fsm_post(&h.fsm, p) == DLM_REQ_OK ? (uint32_t)SS__NORMAL
							 : (uint32_t)SS__UNSUPPORTED;
}

static uint32_t h_eng_dir_resolve(void *ctx, uint16_t hash16, uint32_t *out)
{
	(void)ctx;
	(void)hash16;
	*out = CSID_MASTER;
	return SS__NORMAL;
}

static uint32_t h_eng_dir_generation(void *ctx) { (void)ctx; return 1u; }

static struct vms_dlm_requester_ops h_eng_ops;

static void holder_up(void)
{
	memset(&h, 0, sizeof(h));
	h.now_ms = 1000u;
	ast_arrivals = 0;
	vms_local_csid = CSID_LOCAL;

	memset(&h.ops, 0, sizeof(h.ops));
	h.ops.send            = h_send;
	h.ops.refill_post     = h_refill;
	h.ops.dir_resolve     = h_dir_resolve;
	h.ops.dir_generation  = h_dir_generation;
	h.ops.all_ovmx        = h_all_ovmx;
	h.ops.record_master   = h_record_master;
	h.ops.assume_mastery  = h_assume;
	h.ops.grant_recv      = h_grant_recv;
	h.ops.blkast_deliver  = h_blkast_deliver;
	h.ops.learn_dir_hash  = h_learn;
	h.ops.fail            = h_fail;
	h.ops.now_ms          = h_now;
	h.ops.log             = h_log;
	h.ops.ctx             = &h;
	dlm_req_fsm_init(&h.fsm, &h.ops);

	if (vms_lock_init() != 0) {
		ct_check(0, "holder: vms_lock_init");
		return;
	}
	memset(&h_eng_ops, 0, sizeof(h_eng_ops));
	h_eng_ops.post           = h_post;
	h_eng_ops.dir_resolve    = h_eng_dir_resolve;
	h_eng_ops.dir_generation = h_eng_dir_generation;
	h_eng_ops.ctx            = &h;
	vms_lock_dlm_set_requester_ops(&h_eng_ops);
}

static void holder_down(void)
{
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* Play the wire: the cluster's own 16-bit value for the name (FC-P4.3 -- the
 * executive never computes one). */
static void wire_learn(const char *resnam)
{
	uint16_t hash = 0x4000u;
	const char *c;

	for (c = resnam; *c != '\0'; c++)
		hash = (uint16_t)((hash << 1) ^ (uint8_t)*c);
	(void)vms_lock_dlm_learn_dir_hash(resnam, hash);
}

/*
 * One granted cross-node proxy owned by `proc`, WITH a blocking AST registered
 * -- the only thing that makes a delivered BLKAST observable, and something the
 * engine refuses to fake when it is absent.
 *
 * THE GRANT ARRIVES THE WAY PRODUCTION DELIVERS IT: a cat-0x82 reply body, built
 * by the shipping builder and handed to the SHIPPING FSM entry the arm calls
 * (dlm_req_fsm_reply_body). That matters for this test and not just for realism
 * -- it is what moves the arm's request record to ST_GRANTED, which is the state
 * a BLKAST is accepted in. A test that shortcut the grant straight into the
 * engine would leave the arm in ST_ENQ and prove nothing about the real path.
 */
static uint32_t granted_proxy_with_blkast(struct vms_proc *proc,
					  const char *resnam, uint32_t mode,
					  uint32_t master_lkid)
{
	struct vms_enq_args e;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.lkmode = mode;
	e.blkastadr = 0x4000u;      /* the holder's own routine address */
	strscpy(e.resnam, resnam, sizeof(e.resnam));
	wire_learn(resnam);
	vms_ioctl_enq(proc, (unsigned long)(void *)&e);
	if (e.lkid == 0u)
		return 0u;

	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_response_build_grant(e.lkid, master_lkid, (uint8_t)mode,
					     frame, (uint32_t)sizeof(frame),
					     &written) != VMS_CODEC_OK)
		return 0u;
	if (dlm_req_fsm_reply_body(&h.fsm, (vms_csid_t)CSID_MASTER, 0u,
				   body_of(frame), VMS_CM_BODY_LEN) !=
	    DLM_REQ_OK)
		return 0u;
	return e.lkid;
}

/* Build a REAL op-0x04 through the shipping builder. Returns 0 on success. */
static int build_blkast(uint8_t *frame, uint32_t req_lkid, uint32_t master_lkid)
{
	struct vms_dlm_blkast b;
	uint32_t written = 0;

	memset(&b, 0, sizeof(b));
	b.req_lkid = req_lkid;
	b.master_lkid = master_lkid;
	b.mode_ctx_valid = 0u;      /* OBSERVED-not-pinned: assert nothing */
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_blkast_build(&b, frame, VMS_CM_FRAME_LEN, &written) ==
	       VMS_CODEC_OK ? 0 : -1;
}

static void inbound_blkast_fires_a_real_ast(void)
{
	const uint32_t MASTER_LKID = 0x00ABCDEFu;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_proc holder;
	uint32_t lkid;

	printf("-- an inbound op-0x04 DELIVERS a real blocking AST (rd vms-c72) "
	       "--\n");
	holder_up();
	proc_init(&holder);

	lkid = granted_proxy_with_blkast(&holder, "RECV_BLK1", LCK_K_PRMODE,
					 MASTER_LKID);
	ct_check(lkid != 0, "a real proxy LKB is held here, granted by a real "
			    "master, with a blocking AST registered");
	ct_check_eq_u32(user_asts_pending(&holder), 0u,
			"  and no AST is queued on the holder yet");

	if (build_blkast(frame, lkid, MASTER_LKID) != 0) {
		ct_check(0, "the shipping builder produced an op-0x04");
		holder_down();
		return;
	}
	ct_check(1, "the shipping builder produced an op-0x04 naming that lock");

	/* *** THE DELIVERY *** -- the entry point the arm's dispatch calls. */
	ct_check(dlm_req_fsm_blkast_body(&h.fsm, (vms_csid_t)CSID_MASTER,
					 body_of(frame), VMS_CM_BODY_LEN) ==
		 DLM_REQ_OK,
		 "*** the inbound op-0x04 BODY is ACCEPTED, not declined ***");
	ct_check_eq_u32(h.blkast_calls, 1u,
			"  the engine's holder-side delivery door was invoked");
	ct_check_eq_u32(h.last_blkast_lkid, lkid,
			"  with the handle THIS executive minted (body[20:24])");
	ct_check_eq_u32(h.fsm.blkasts_rx, 1u,
			"  blkasts_received rose (the snapshot's own field)");
	ct_check_eq_u32(h.fsm.blkasts_delivered, 1u,
			"  blkasts_delivered rose");

	/* *** THE TEETH *** -- the lock database moved: a REAL user-mode AST is
	 * queued on the REAL holder, which no parse could have produced. */
	ct_check_eq_u32(user_asts_pending(&holder), 1u,
			"*** a REAL user-mode blocking AST is queued on the "
			"holder's process ***");
	ct_check(ast_arrivals >= 1u,
		 "  and the executive's AST arrival hook really ran");

	holder_down();
}

/* NEGATIVE CONTROL 1: a body naming a lock this node does not hold delivers
 * NOTHING. An AST fired at a lock we cannot name would be a fabrication. */
static void blkast_for_an_unknown_lock_delivers_nothing(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_proc holder;
	uint32_t lkid;

	printf("-- negative: an op-0x04 naming no lock of ours --\n");
	holder_up();
	proc_init(&holder);
	lkid = granted_proxy_with_blkast(&holder, "RECV_BLK2", LCK_K_PRMODE,
					 0x11112222u);
	ct_check(lkid != 0, "a real proxy exists (so the refusal is about the "
			    "HANDLE, not an empty arm)");

	if (build_blkast(frame, lkid ^ 0x5A5A5A5Au, 0x11112222u) != 0) {
		ct_check(0, "builder: a well-formed op-0x04 for another handle");
		holder_down();
		return;
	}
	ct_check(dlm_req_fsm_blkast_body(&h.fsm, (vms_csid_t)CSID_MASTER,
					 body_of(frame), VMS_CM_BODY_LEN) !=
		 DLM_REQ_OK,
		 "the BLKAST is REFUSED: it names no request of ours");
	ct_check_eq_u32(h.blkast_calls, 0u,
			"  the engine door was never called");
	ct_check_eq_u32(user_asts_pending(&holder), 0u,
			"  and NO AST was queued (never fired at a lock we "
			"cannot name)");
	ct_check_eq_u32(h.fsm.replies_unmatched, 1u,
			"  counted as unmatched, never silent");

	holder_down();
}

/*
 * NEGATIVE CONTROL 2: a ZERO lock id and an unparseable body.
 *
 * The zero case is forged BY HAND on purpose -- the shipping builder REFUSES to
 * write one (the fc8540ae INVLOCKID lesson), so the only way to prove the
 * receive side refuses it too is to forge the bytes a hostile/broken peer would
 * send. This is the one place in this file that touches raw offsets, and it
 * reaches them through the codec's own published constants.
 */
static void a_zero_lock_id_and_a_garbage_body_are_refused(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t forged[VMS_CM_BODY_LEN];
	struct vms_proc holder;
	uint32_t lkid;

	printf("-- negative: a zero lock id, and a body that is not an op-0x04 "
	       "--\n");
	holder_up();
	proc_init(&holder);
	lkid = granted_proxy_with_blkast(&holder, "RECV_BLK3", LCK_K_PRMODE,
					 0x33334444u);
	ct_check(lkid != 0, "a real proxy exists");

	/* (a) master_lkid == 0, everything else well-formed. */
	memset(forged, 0, sizeof(forged));
	forged[VMS_OFB_DLM_CAT] = (uint8_t)VMS_DLM_CAT_REQUEST;
	forged[VMS_OFB_DLM_OP]  = (uint8_t)VMS_DLM_WIREOP_BLKAST;
	forged[VMS_OFB_DLM_REQ_LKID + 0] = (uint8_t)(lkid & 0xffu);
	forged[VMS_OFB_DLM_REQ_LKID + 1] = (uint8_t)((lkid >> 8) & 0xffu);
	forged[VMS_OFB_DLM_REQ_LKID + 2] = (uint8_t)((lkid >> 16) & 0xffu);
	forged[VMS_OFB_DLM_REQ_LKID + 3] = (uint8_t)((lkid >> 24) & 0xffu);
	/* body[24:28] left at zero -- the fabricated referent. */

	ct_check(dlm_req_fsm_blkast_body(&h.fsm, (vms_csid_t)CSID_MASTER,
					 forged, (uint32_t)sizeof(forged)) !=
		 DLM_REQ_OK,
		 "*** a BLKAST whose master_lkid is 0 is REFUSED, not acted on "
		 "(fc8540ae) ***");
	ct_check_eq_u32(h.blkast_calls, 0u, "  nothing reached the engine");
	ct_check_eq_u32(user_asts_pending(&holder), 0u, "  no AST was queued");
	ct_check_eq_u32(h.fsm.blkasts_unparsed, 1u,
			"  counted as unparsed, never silent");

	/* (b) a body that is a well-formed op-0x03, not an op-0x04: the codec's
	 * opcode gate refuses it, so the two lock-id-only shapes can never be
	 * confused for one another. */
	if (build_blkast(frame, lkid, 0x33334444u) == 0) {
		uint8_t wrong[VMS_CM_BODY_LEN];

		memcpy(wrong, body_of(frame), sizeof(wrong));
		wrong[VMS_OFB_DLM_OP] = (uint8_t)VMS_DLM_WIREOP_DEQ;
		ct_check(dlm_req_fsm_blkast_body(&h.fsm,
						 (vms_csid_t)CSID_MASTER, wrong,
						 (uint32_t)sizeof(wrong)) !=
			 DLM_REQ_OK,
			 "a body carrying a DIFFERENT opcode is refused by the "
			 "codec's own gate");
		ct_check_eq_u32(h.blkast_calls, 0u,
				"  and still nothing reached the engine");
		ct_check_eq_u32(h.fsm.blkasts_unparsed, 2u,
				"  counted");
	}

	holder_down();
}

/* ==========================================================================
 * 2. THE MASTER SIDE: an inbound op-0x03 really RELEASES the master copy
 *
 * This node masters the tree. A peer that held a lock here releases it, and the
 * frame's two lock ids are what name the LKB to destroy. The proof is that the
 * lock is GONE from the engine afterwards -- read back through the engine's own
 * door, not from a counter this file kept.
 * ========================================================================== */

/* Build a REAL op-0x03 through the shipping builder. */
static int build_deq(uint8_t *frame, uint32_t req_lkid, uint32_t master_lkid,
		     uint8_t mode)
{
	struct vms_dlm_deq d;
	uint32_t written = 0;

	memset(&d, 0, sizeof(d));
	d.req_lkid = req_lkid;
	d.master_lkid = master_lkid;
	d.mode = mode;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_deq_build(&d, frame, VMS_CM_FRAME_LEN, &written) ==
	       VMS_CODEC_OK ? 0 : -1;
}

/*
 * THE TRANSLATION THE ARM PERFORMS, and the ONLY thing this file restates from
 * it (`dlm_arm_fill_deq_req`, pinned line-by-line by test_dlm_scs_arm.c's
 * source scan): the PARSED frame plus the connection manager's identification
 * of the sender, and nothing else. No resource name -- a $DEQ carries none --
 * and no value block, so the master's own is left alone.
 */
static void deq_body_to_master_request(const uint8_t *body, uint32_t from_csid,
				       struct vms_dlm_master_request *out,
				       int *parsed)
{
	struct vms_dlm_deq q;

	memset(out, 0, sizeof(*out));
	*parsed = 0;
	if (vms_dlm_deq_parse_body(body, VMS_CM_BODY_LEN, &q) != VMS_CODEC_OK)
		return;
	*parsed = 1;
	out->op = VMS_DLM_MREQ_DEQ;
	out->req_csid = from_csid;
	out->req_lkid = q.req_lkid;
	out->master_lkid = q.master_lkid;
	out->lkmode = q.mode;
}

/* Serve one inbound ENQ as the master, exactly as the arm does. */
static void master_enq(uint32_t csid, uint32_t lkid, uint32_t mode,
		       uint32_t flags, const char *resnam,
		       struct vms_dlm_master_result *out)
{
	struct vms_dlm_master_request r;

	memset(&r, 0, sizeof(r));
	r.op = VMS_DLM_MREQ_ENQ;
	r.req_csid = csid;
	r.req_lkid = lkid;
	r.lkmode = mode;
	r.flags = flags;
	strscpy(r.resnam, resnam, sizeof(r.resnam));
	(void)vms_lock_dlm_master_serve(&r, out);
}

/* Is the lock still there? Asked the way the engine answers it: a second
 * release of the same handle, by its real owner, can only succeed once. */
static int master_lock_still_there(uint32_t csid, uint32_t req_lkid,
				   uint32_t master_lkid)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result res;

	memset(&r, 0, sizeof(r));
	r.op = VMS_DLM_MREQ_DEQ;
	r.req_csid = csid;
	r.req_lkid = req_lkid;
	r.master_lkid = master_lkid;
	(void)vms_lock_dlm_master_serve(&r, &res);
	return res.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED;
}

static void inbound_deq_releases_the_master_copy(void)
{
	const uint32_t PEER_A_LKID = 0x0A0A0001u;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result granted, released;
	struct vms_proc delivery;
	int parsed = 0;

	printf("-- an inbound op-0x03 RELEASES the master copy (rd vms-c72) "
	       "--\n");
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0) {
		ct_check(0, "master: vms_lock_init");
		return;
	}
	proc_init(&delivery);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	master_enq(CSID_PEER_A, PEER_A_LKID, LCK_K_EXMODE, 0u, "RECV_DEQ1",
		   &granted);
	ct_check(granted.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 granted.master_lkid != 0u,
		 "a peer's $ENQ made a REAL master-side LKB here");

	if (build_deq(frame, PEER_A_LKID, granted.master_lkid,
		      (uint8_t)LCK_K_EXMODE) != 0) {
		ct_check(0, "the shipping builder produced an op-0x03");
		goto out;
	}
	ct_check(1, "the shipping builder produced an op-0x03 naming BOTH real "
		    "handles");

	deq_body_to_master_request(body_of(frame), CSID_PEER_A, &mr, &parsed);
	ct_check(parsed, "the shipping parser read it back as a grounded "
			 "op-0x03 body");
	ct_check_eq_u32(mr.master_lkid, granted.master_lkid,
			"  body[24:28] is OUR handle -- the LKB to release");
	ct_check_eq_u32(mr.req_lkid, PEER_A_LKID,
			"  body[20:24] is the RELEASER's own handle");

	/* *** THE DELIVERY *** */
	ct_check(vms_lock_dlm_master_serve(&mr, &released) == SS__NORMAL &&
		 released.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "*** the inbound op-0x03 is SERVED, not declined: the engine "
		 "RELEASED it ***");

	/* *** THE TEETH *** -- the master copy is really gone. */
	ct_check(!master_lock_still_there(CSID_PEER_A, PEER_A_LKID,
					  granted.master_lkid),
		 "*** the master-side LKB is GONE from the lock database: a "
		 "second release of the same handle finds nothing ***");

out:
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/*
 * THE DEFERRED GRANT the release earns. The flip is REAL (the engine says which
 * waiter, with which handles), and the arm counts it as OWED rather than
 * originating an uncorrelated cat-0x82 grant. This asserts the fact the counter
 * is a projection of -- if the engine stopped reporting the flip, the counter
 * would be a number with nothing behind it.
 */
static void a_release_reports_the_waiter_it_flipped(void)
{
	const uint32_t A_LKID = 0x0B0B0001u;
	const uint32_t B_LKID = 0x0B0B0002u;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result granted, queued, released;
	struct vms_proc delivery;
	int parsed = 0;

	printf("-- an inbound op-0x03 that flips a queued cross-node waiter "
	       "--\n");
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0) {
		ct_check(0, "master: vms_lock_init");
		return;
	}
	proc_init(&delivery);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	master_enq(CSID_PEER_A, A_LKID, LCK_K_EXMODE, 0u, "RECV_DEQ2",
		   &granted);
	master_enq(CSID_PEER_B, B_LKID, LCK_K_EXMODE, 0u, "RECV_DEQ2", &queued);
	ct_check(granted.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 queued.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED,
		 "peer A holds it EX and peer B is really queued behind him");

	if (build_deq(frame, A_LKID, granted.master_lkid,
		      (uint8_t)LCK_K_EXMODE) != 0) {
		ct_check(0, "builder: the op-0x03");
		goto out;
	}
	deq_body_to_master_request(body_of(frame), CSID_PEER_A, &mr, &parsed);
	ct_check(parsed, "the op-0x03 parses");

	ct_check(vms_lock_dlm_master_serve(&mr, &released) == SS__NORMAL &&
		 released.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "the release is served");
	ct_check(released.deferred_grant == 1u &&
		 released.deferred_csid == CSID_PEER_B &&
		 released.deferred_req_lkid == B_LKID &&
		 released.deferred_master_lkid == queued.master_lkid,
		 "*** the release really FLIPPED the queued waiter, and the "
		 "engine names it -- what deferred_grants_owed counts ***");

out:
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/*
 * NEGATIVE CONTROL 3: cross-node authorization is by CLUSTER IDENTITY. The
 * sender's CSID comes from the connection manager, never from the body -- so a
 * peer that did not hold the lock cannot release it by naming its handles.
 */
static void a_peer_may_not_release_another_nodes_lock(void)
{
	const uint32_t A_LKID = 0x0C0C0001u;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result granted, refused;
	struct vms_proc delivery;
	int parsed = 0;

	printf("-- negative: an op-0x03 from a peer that does not hold it --\n");
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0) {
		ct_check(0, "master: vms_lock_init");
		return;
	}
	proc_init(&delivery);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	master_enq(CSID_PEER_A, A_LKID, LCK_K_EXMODE, 0u, "RECV_DEQ3",
		   &granted);
	ct_check(granted.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "peer A holds a real lock here");

	if (build_deq(frame, A_LKID, granted.master_lkid,
		      (uint8_t)LCK_K_EXMODE) != 0) {
		ct_check(0, "builder: the op-0x03");
		goto out;
	}
	/* The SAME bytes -- arriving from a DIFFERENT system. */
	deq_body_to_master_request(body_of(frame), CSID_PEER_B, &mr, &parsed);
	ct_check(parsed, "the frame parses identically");

	ct_check(vms_lock_dlm_master_serve(&mr, &refused) == SS__NORMAL &&
		 refused.outcome != (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "*** the release is REFUSED: the sender's CSID is not the one "
		 "the LKB is held for ***");
	ct_check(master_lock_still_there(CSID_PEER_A, A_LKID,
					 granted.master_lkid),
		 "  and peer A's lock is STILL THERE afterwards");

out:
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/*
 * NEGATIVE CONTROL 4: a zero master_lkid, and no delivery proc.
 *
 * The zero is forged by hand for the same reason as in §1 -- the builder
 * refuses to write one. The delivery-proc case is vms-c27 condition 4 applied
 * to the receive side: with nobody owning the master-side LKBs there is nothing
 * of ours to release, and the arm refuses before it asks the engine.
 */
static void a_zero_master_lkid_is_refused(void)
{
	uint8_t forged[VMS_CM_BODY_LEN];
	struct vms_dlm_master_request mr;
	int parsed = 1;

	printf("-- negative: an op-0x03 naming lock 0 --\n");

	memset(forged, 0, sizeof(forged));
	forged[VMS_OFB_DLM_CAT] = (uint8_t)VMS_DLM_CAT_REQUEST;
	forged[VMS_OFB_DLM_OP]  = (uint8_t)VMS_DLM_WIREOP_DEQ;
	forged[VMS_OFB_DLM_REQ_LKID] = 0x11u;   /* a real-looking requester id */
	/* body[24:28] left at zero -- the fabricated referent. */

	deq_body_to_master_request(forged, CSID_PEER_A, &mr, &parsed);
	ct_check(!parsed,
		 "*** an op-0x03 whose master_lkid is 0 never becomes a "
		 "request: the codec refuses it (fc8540ae) ***");
	ct_check_eq_u32(mr.master_lkid, 0u,
			"  and no engine call was composed from it");
}

/* ==========================================================================
 * 3. THE VALUE BLOCK's RECEIVE HALF: an inbound op-0x06 MOVES the master's
 *    value block (rd vms-727)
 *
 * A peer holds a lock here at a write mode and flushes its value block on a
 * demote (an op-0x06 CONVERT-with-VALBLK). The proof is the same caliber as the
 * $DEQ's: not a counter, but the LOCK DATABASE MOVED -- the master resource's
 * value block, read back through the engine's own grant path, now HOLDS the
 * peer's bytes where it held zeros before.
 * ========================================================================== */

/* Build a REAL op-0x06 through the shipping builder. */
static int build_valblk_convert(uint8_t *frame, uint32_t req_lkid,
				uint32_t master_lkid, uint8_t mode,
				const uint8_t *valblk)
{
	struct vms_dlm_valblk_convert c;
	uint32_t written = 0;

	memset(&c, 0, sizeof(c));
	c.req_lkid = req_lkid;
	c.master_lkid = master_lkid;
	c.mode = mode;
	c.serial = 0u;
	memcpy(c.valblk, valblk, VMS_DLM_VALBLK_WIRE_LEN);
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_valblk_convert_build(&c, frame, VMS_CM_FRAME_LEN,
					    &written) == VMS_CODEC_OK ? 0 : -1;
}

/* Read the master resource's value block back the way the engine hands it out:
 * a LOCAL NL grant with LCK_M_VALBLK (NL is compatible with any held mode, so
 * it is granted at once and copies res->valblk into the caller's LKSB). */
static void master_read_valblk(struct vms_proc *proc, const char *resnam,
			       uint8_t out[VMS_DLM_VALBLK_WIRE_LEN])
{
	struct vms_enq_args e;

	memset(&e, 0, sizeof(e));
	e.lkmode = LCK_K_NLMODE;
	e.flags = LCK_M_VALBLK;
	strscpy(e.resnam, resnam, sizeof(e.resnam));
	vms_ioctl_enq(proc, (unsigned long)(void *)&e);
	memcpy(out, e.valblk, VMS_DLM_VALBLK_WIRE_LEN);
}

static void inbound_valblk_convert_writes_the_master_block(void)
{
	const uint32_t PEER_A_LKID = 0x0C0C0001u;
	static const uint8_t PATTERN[VMS_DLM_VALBLK_WIRE_LEN] =
		"WROTEBYPEERAXXXX";
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t before[VMS_DLM_VALBLK_WIRE_LEN];
	uint8_t after[VMS_DLM_VALBLK_WIRE_LEN];
	struct vms_dlm_valblk_convert q;
	struct vms_dlm_master_result granted;
	struct vms_proc delivery;

	printf("-- an inbound op-0x06 WRITES the master value block (rd vms-727) "
	       "--\n");
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0) {
		ct_check(0, "master: vms_lock_init");
		return;
	}
	proc_init(&delivery);
	delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&delivery);

	master_enq(CSID_PEER_A, PEER_A_LKID, LCK_K_EXMODE, 0u, "RECV_LVB1",
		   &granted);
	ct_check(granted.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 granted.master_lkid != 0u,
		 "a peer's $ENQ made a REAL master-side LKB here, held at EX");

	master_read_valblk(&delivery, "RECV_LVB1", before);
	ct_check(before[0] == 0u,
		 "the master's value block starts empty (no write yet)");

	if (build_valblk_convert(frame, PEER_A_LKID, granted.master_lkid,
				 (uint8_t)LCK_K_NLMODE, PATTERN) != 0) {
		ct_check(0, "the shipping builder produced an op-0x06");
		goto out;
	}
	ct_check(1, "the shipping builder produced an op-0x06 naming BOTH real "
		    "handles and carrying the block");
	ct_check(vms_dlm_valblk_convert_parse_body(body_of(frame),
						   VMS_CM_BODY_LEN, &q) ==
		 VMS_CODEC_OK,
		 "the shipping parser read it back as a grounded op-0x06 body");
	ct_check_eq_u32(q.master_lkid, granted.master_lkid,
			"  body[24:28] is OUR handle -- the LKB whose block moves");

	/* *** THE DELIVERY *** authorized by cluster identity (PEER_A holds it). */
	ct_check(vms_lock_dlm_master_apply_valblk(CSID_PEER_A, q.master_lkid,
						  q.valblk) == SS__NORMAL,
		 "*** the inbound op-0x06 is SERVED, not declined: the engine "
		 "APPLIED the value block ***");

	/* *** THE TEETH *** -- the master's block really moved. */
	master_read_valblk(&delivery, "RECV_LVB1", after);
	ct_check(memcmp(after, PATTERN, VMS_DLM_VALBLK_WIRE_LEN) == 0,
		 "*** the master resource's value block now HOLDS the peer's "
		 "bytes: the lock database MOVED, not a counter ***");

	/* NEGATIVE: a different peer may not write this lock's block. */
	{
		static const uint8_t IMPOSTOR[VMS_DLM_VALBLK_WIRE_LEN] =
			"IMPOSTORXXXXXXXX";
		uint8_t still[VMS_DLM_VALBLK_WIRE_LEN];

		ct_check(vms_lock_dlm_master_apply_valblk(CSID_PEER_B,
							  q.master_lkid,
							  IMPOSTOR) != SS__NORMAL,
			 "a peer that does NOT hold the lock is REFUSED "
			 "(SS$_IVLOCKID): no writing another node's block");
		master_read_valblk(&delivery, "RECV_LVB1", still);
		ct_check(memcmp(still, PATTERN, VMS_DLM_VALBLK_WIRE_LEN) == 0,
			 "*** and the block is UNCHANGED by the refused write ***");
	}

out:
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

int main(void)
{
	printf("=== test_dlm_recv_arm (rd vms-c72: the DLM arm's RECEIVE half, "
	       "R1 host unit) ===\n");

	inbound_blkast_fires_a_real_ast();
	blkast_for_an_unknown_lock_delivers_nothing();
	a_zero_lock_id_and_a_garbage_body_are_refused();

	inbound_deq_releases_the_master_copy();
	a_release_reports_the_waiter_it_flipped();
	a_peer_may_not_release_another_nodes_lock();
	a_zero_master_lkid_is_refused();

	inbound_valblk_convert_writes_the_master_block();

	return ct_summary("test_dlm_recv_arm");
}
