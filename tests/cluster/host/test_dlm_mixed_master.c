/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_mixed_master.c - rd vms-025 / vms-db2a / vms-c27(3)'s R1:
 * IN A MIXED CLUSTER, ONE RESOURCE HAS EXACTLY ONE MASTER.
 *
 * ==========================================================================
 * THE HOLE THIS FILE EXISTS TO MAKE UNREPEATABLE
 * ==========================================================================
 * Measured on origin/main, both horns of it:
 *
 *   (a) vms_lock.c's dir_resolve() returns "this node" for EVERY name while
 *       any member is non-OVMX (the all-OVMX gate), and the local $ENQ path
 *       never consulted the directory table THIS NODE HOLDS. So a real VAX
 *       that locked N first -- its lookup answered and RECORDED here, naming
 *       the VAX as N's master (rd vms-8219) -- and a local $ENQ on N gave TWO
 *       masters for one resource.
 *   (b) The other way round: a VAX's lookup for a name this node already
 *       mastered found no directory entry and was answered "you master it",
 *       so the VAX became a second master of a resource this node masters.
 *
 * Two masters for one resource is not a missing feature. It is the lock
 * manager silently agreeing that two systems may both grant EX on the same
 * file, and the first shared write is a corrupted one.
 *
 * ==========================================================================
 * WHAT IS REAL HERE -- everything except the VAX's own lock manager
 * ==========================================================================
 *   the ENGINE     src/kernel-core/vms_lock.c on FC-P4.9's host backend. A
 *                  $ENQ here is the SAME $ENQ the executive runs: the same
 *                  dlm_resolve_master, the same proxy LKB, the same
 *                  master-side dispatch, the same try_grant_waiters.
 *   the CLUB       a real struct vms_club with real CSBs, whose Lock Directory
 *                  Weight Vector is built by the SHIPPING cnxman_ldwv_rebuild
 *                  from LOCKDIRWTs set on those CSBs. "This node is the sole
 *                  directory node" is therefore a REAL READ of a REAL vector
 *                  (vms_ldwv_sole_directory), never a test flag.
 *   the DIRECTORY  src/kernel-core/vms_dlm_dir.c -- the shipping table.
 *   the FSM        src/kernel-core/vms_dlm_scs_fsm.c -- the shipping requester.
 *   the CODEC      src/kernel-core/vms_cluster_codec_dlm.c, both ways. Every
 *                  byte the "VAX" sends is BUILT by the shipping builder and
 *                  read by the shipping parser; no frame is composed by hand.
 *
 * SIMULATED: the VAX's own lock manager (it is scripted), and the LAN (a
 * function call). What is NOT here is the glue TU src/kernel-core/vms_dlm_scs.c
 * -- it names exec_kbackend.h and the fork API and is not host-linkable, as
 * test_dlm_recv_arm.c and test_dlm_deq_reachable.c already document. Its
 * ROUTING (which frame reaches which door, and which gate each sits behind) is
 * pinned by the source scan in test_dlm_scs_arm.c; this file owns what a scan
 * cannot see: WHAT THE LOCK DATABASE DOES.
 *
 * EVERY SECTION CARRIES ITS NEGATIVE CONTROL. Reverting the fix has to make a
 * check here fail, so each scenario also asserts the behaviour with the
 * directory entry absent, or with the configuration not held -- which is the
 * old behaviour, and is still the behaviour outside the interim configuration.
 */
#include "cluster_test.h"
#include "cluster_fixture.h"  /* the REAL captured frame, rd vms-025           */

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_dir.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_scs_fsm.h"
#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"

#include <stdio.h>
#include <string.h>

/* The executive globals vms_lock.c reads, and the AST hook it calls. */
uint32_t vms_local_csid = 0x00010004u;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* The two systems. OVMX is CSV slot 4, the VAX slot 2 -- the lab's shape. */
#define CSID_OVMX  0x00010004u
#define CSID_VAX   0x00010002u
#define CSID_VAX2  0x00010003u

/* The VAX's own handles for its locks. They are ITS values; this executive
 * only ever echoes them back where the protocol says the requester's handle
 * goes, and never mints one on its behalf. */
#define VAX_LKID_1 0x0a0b0001u
#define VAX_LKID_2 0x0a0b0002u

/* ==========================================================================
 * 0. The harness
 * ========================================================================== */

struct mixed {
	struct vms_cluster       cl;
	struct cnxman_ops        cnx_ops;
	struct vms_dlm_dir       dir;
	struct vms_dlm_dir_entry dir_store[64];

	struct dlm_req_fsm       fsm;
	struct dlm_req_ops       fsm_ops;
	struct vms_dlm_requester_ops eng_ops;

	uint32_t now_ms;

	/* What went on the "wire", and to whom. One frame is enough: every
	 * assertion below is about the frame a single action produced. */
	uint32_t n_sent;
	vms_csid_t last_dst;
	uint8_t  last[VMS_CM_BODY_LEN];
};

static struct mixed g;
static struct vms_proc g_delivery;   /* the rd vms-c27 delivery proc */

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

static uint32_t cnx_now(void *ctx) { (void)ctx; return g.now_ms; }
static void cnx_log(void *ctx, const char *msg) { (void)ctx; (void)msg; }

/*
 * Bring the two-system cluster up with the LOCKDIRWTs the caller names, and
 * let the SHIPPING Phase 2 fill build the vector from them. `ovmx_wt`/`vax_wt`
 * are the only inputs: whether this node comes out the SOLE directory node is
 * then the vector's own arithmetic (Davis p. 6-32), which is exactly the
 * property the production gate reads.
 */
static void club_up(uint8_t ovmx_wt, uint8_t vax_wt)
{
	struct vms_csb *csb;

	memset(&g.cl, 0, sizeof(g.cl));
	memset(&g.cnx_ops, 0, sizeof(g.cnx_ops));
	g.cnx_ops.now_ms = cnx_now;
	g.cnx_ops.log = cnx_log;
	g.cnx_ops.ctx = &g;

	memcpy(g.cl.params.scsnode, "OVMXS", 5);
	g.cl.params.scsnode_len = 5;
	g.cl.params.scssystemid = (uint64_t)CSID_OVMX;
	g.cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&g.cl);

	csb = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)CSID_VAX, 1);
	cnxman_csb_set_csid(csb, CSID_VAX);
	cnxman_csb_set_lockdirwt(csb, vax_wt);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	/* The VAX is NOT proven to run this implementation. That is the whole
	 * point of a mixed cluster, and it is what keeps the all-OVMX gate
	 * (and so the engine's hash grounding) shut throughout this file. */
	csb = cnxman_club_local(&g.cl.club);
	cnxman_csb_set_csid(csb, CSID_OVMX);
	cnxman_csb_set_lockdirwt(csb, ovmx_wt);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	g.cl.club.local_csid = CSID_OVMX;
	g.cl.club.local_csid_valid = 1u;

	(void)cnxman_ldwv_rebuild(&g.cl.club, &g.cnx_ops);
}

/* ==========================================================================
 * 0b. The arm's two directory ops, restated.
 *
 * Byte-for-byte what src/kernel-core/vms_dlm_scs.c binds
 * (dlm_arm_eng_dir_local_lookup / dlm_arm_eng_dir_claim_self, pinned line by
 * line by test_dlm_scs_arm.c's source scan) -- the SHIPPING table, the
 * SHIPPING vector read, and the same refusals. The only thing this file
 * supplies is the glue, because the glue TU does not link on the host.
 * ========================================================================== */

static int sole_directory(void *ctx)
{
	(void)ctx;
	return vms_ldwv_sole_directory(&g.cl.club.ldwv);
}

static uint32_t eng_dir_local_lookup(void *ctx, const char *name,
				     uint32_t name_len,
				     struct vms_dlm_dir_local *out)
{
	enum vms_dlm_dir_name_outcome o;
	vms_csid_t master = 0u;
	uint32_t hash = 0u;
	uint8_t hash_known = 0u;

	if (name == NULL || out == NULL)
		return SS__BADPARAM;
	if (!sole_directory(ctx))
		return SS__UNSUPPORTED;
	o = vms_dlm_dir_lookup_name(&g.dir, name, name_len, &master, &hash,
				    &hash_known);
	if (o == VMS_DLM_DIR_NAME_INVAL || o == VMS_DLM_DIR_NAME_AMBIGUOUS)
		return SS__UNSUPPORTED;
	if (o == VMS_DLM_DIR_NAME_NONE) {
		out->master_csid = 0u;
		out->is_self = 0u;
		out->dir_hash_known = 0u;
		return SS__NORMAL;
	}
	out->master_csid = (uint32_t)master;
	out->is_self = (master == (vms_csid_t)CSID_OVMX) ? 1u : 0u;
	out->dir_hash = hash;
	out->dir_hash_known = hash_known;
	return SS__NORMAL;
}

static uint32_t eng_dir_claim_self(void *ctx, const char *name,
				   uint32_t name_len)
{
	if (!sole_directory(ctx))
		return SS__UNSUPPORTED;
	return vms_dlm_dir_claim_self(&g.dir, name, name_len,
				      (vms_csid_t)CSID_OVMX) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__INSFMEM;
}

static uint32_t eng_dir_resolve(void *ctx, uint32_t dir_hash, uint32_t *out)
{
	vms_csid_t csid = 0;

	(void)ctx;
	if (vms_ldwv_resolve(&g.cl.club.ldwv, vms_ldwv_key(dir_hash), &csid) !=
	    VMS_LDWV_OK)
		return SS__UNSUPPORTED;
	*out = (uint32_t)csid;
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return vms_ldwv_generation(&g.cl.club.ldwv);
}

/* The all-OVMX gate, read exactly as production reads it. With a real VAX in
 * the membership it is SHUT for every test in this file. */
static int eng_dir_groundable(void *ctx)
{
	(void)ctx;
	return vms_ldwv_all_ovmx(&g.cl.club.ldwv);
}

/* ==========================================================================
 * 0c. The requester FSM's doors (the arm's, restated the same way)
 * ========================================================================== */

static int fsm_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	g.n_sent++;
	g.last_dst = dst;
	memset(g.last, 0, sizeof(g.last));
	memcpy(g.last, body, len > sizeof(g.last) ? sizeof(g.last) : len);
	return 0;
}

static int fsm_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		      vms_csid_t dst_csid, struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, (uint32_t)dst_csid,
					      out) == SS__NORMAL ? 0 : -1;
}

static int fsm_dir_resolve(void *ctx, uint32_t hash, vms_csid_t *out)
{
	uint32_t c = 0;

	if (eng_dir_resolve(ctx, hash, &c) != SS__NORMAL)
		return -1;
	*out = (vms_csid_t)c;
	return 0;
}

static uint32_t fsm_dir_generation(void *ctx) { return eng_dir_generation(ctx); }
static int fsm_all_ovmx(void *ctx) { return eng_dir_groundable(ctx); }

static int fsm_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			     vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid,
					  (uint32_t)master_csid) == SS__NORMAL ?
	       0 : -1;
}

static int fsm_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) == SS__NORMAL ?
	       0 : -1;
}

static int fsm_grant_recv(void *ctx, const struct vms_dlm_proxy_grant *gr)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(gr) == SS__NORMAL ? 0 : -1;
}

static int fsm_blkast(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) == SS__NORMAL ? 0 : -1;
}

static int fsm_learn(void *ctx, const char *resnam, uint32_t hash)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, hash) == SS__NORMAL ? 0 : -1;
}

static void fsm_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	(void)ctx;
	(void)why;
	(void)vms_lock_dlm_proxy_fail(req_lkid, SS__UNSUPPORTED);
}

static uint32_t fsm_now(void *ctx) { (void)ctx; return g.now_ms; }
static void fsm_log(void *ctx, const char *m) { (void)ctx; (void)m; }

static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p == NULL)
		return SS__BADPARAM;
	return dlm_req_fsm_post(&g.fsm, p) == DLM_REQ_OK ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__UNSUPPORTED;
}

static void mixed_up(uint8_t ovmx_wt, uint8_t vax_wt)
{
	memset(&g.fsm_ops, 0, sizeof(g.fsm_ops));
	memset(&g.eng_ops, 0, sizeof(g.eng_ops));
	g.now_ms = 1000u;
	g.n_sent = 0u;
	g.last_dst = 0u;
	vms_local_csid = CSID_OVMX;

	club_up(ovmx_wt, vax_wt);
	(void)vms_dlm_dir_init(&g.dir, g.dir_store,
			       (uint32_t)(sizeof(g.dir_store) /
					  sizeof(g.dir_store[0])));

	g.fsm_ops.send           = fsm_send;
	g.fsm_ops.refill_post    = fsm_refill;
	g.fsm_ops.dir_resolve    = fsm_dir_resolve;
	g.fsm_ops.dir_generation = fsm_dir_generation;
	g.fsm_ops.all_ovmx       = fsm_all_ovmx;
	g.fsm_ops.mixed_dlm_ok   = sole_directory;
	g.fsm_ops.record_master  = fsm_record_master;
	g.fsm_ops.assume_mastery = fsm_assume;
	g.fsm_ops.grant_recv     = fsm_grant_recv;
	g.fsm_ops.blkast_deliver = fsm_blkast;
	g.fsm_ops.learn_dir_hash = fsm_learn;
	g.fsm_ops.fail           = fsm_fail;
	g.fsm_ops.now_ms         = fsm_now;
	g.fsm_ops.log            = fsm_log;
	g.fsm_ops.ctx            = &g;
	dlm_req_fsm_init(&g.fsm, &g.fsm_ops);

	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	g.eng_ops.post             = eng_post;
	g.eng_ops.dir_resolve      = eng_dir_resolve;
	g.eng_ops.dir_generation   = eng_dir_generation;
	g.eng_ops.dir_groundable   = eng_dir_groundable;
	g.eng_ops.dir_local_lookup = eng_dir_local_lookup;
	g.eng_ops.dir_claim_self   = eng_dir_claim_self;
	g.eng_ops.ctx              = &g;
	vms_lock_dlm_set_requester_ops(&g.eng_ops);

	proc_init(&g_delivery);
	g_delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	vms_lock_dlm_set_local_csid(CSID_OVMX);
}

static void mixed_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ==========================================================================
 * 0d. The scripted VAX, speaking the captured wire format
 * ========================================================================== */

static const uint8_t *body_of(const uint8_t *frame)
{
	return frame + VMS_OFF_SYSAP_BODY;
}

/*
 * ONE op-0x01 ENQ (or op-0x07 CONVERT) as a real VAX puts it on the wire,
 * built by the SHIPPING builder: the requester's own handle at body[20:24],
 * the mode at body[30], the name at body[48..], and the sender's own 32-bit
 * directory hash at body[128:132] -- the value this executive may learn and
 * may never compute (Davis p. 6-50).
 */
static int vax_build_enq(uint8_t *frame, uint8_t opcode, uint32_t vax_lkid,
			 uint8_t mode, const char *name, uint32_t hash)
{
	struct vms_dlm_enq_request r;
	uint32_t written = 0;

	memset(&r, 0, sizeof(r));
	r.mode = mode;
	r.req_pid_or_lkid = vax_lkid;
	r.name_len = (uint8_t)strlen(name);
	memcpy(r.name, name, r.name_len);
	r.dir_hash = hash;
	r.dir_hash_valid = 1u;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_request_build(&r, opcode, frame, VMS_CM_FRAME_LEN,
					 &written) == VMS_CODEC_OK ? 0 : -1;
}

/*
 * THE DIRECTORY ROLE, as the arm performs it for a frame from a system that is
 * not this implementation: learn the hash the frame carried, then ask the table
 * (dlm_arm_handle_request's observe + dlm_arm_dir_lookup, both pinned by the
 * scan). Returns the directory's outcome.
 */
static enum vms_dlm_dir_outcome vax_lookup(const uint8_t *frame,
					   vms_csid_t from,
					   vms_csid_t *out_master)
{
	struct vms_dlm_res_ident id;
	uint32_t hash = 0;
	char nm[VMS_DLM_NAME_MAX + 1];
	uint32_t i;

	if (vms_dlm_res_ident_parse_body(body_of(frame), VMS_CM_BODY_LEN, &id) !=
	    VMS_CODEC_OK)
		return VMS_DLM_DIR_ANSWER_NONE;
	/* The hash learner, from the frame itself. */
	if (vms_dlm_dir_hash_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					&hash) == VMS_CODEC_OK) {
		for (i = 0u; i < id.name_len && id.name[i] != 0u; i++)
			nm[i] = (char)id.name[i];
		nm[i] = '\0';
		(void)vms_lock_dlm_learn_dir_hash(nm, hash);
	}
	return vms_dlm_dir_lookup(&g.dir, &id, from, (vms_csid_t)CSID_OVMX,
				  out_master);
}

/* Serve one of the VAX's op-0x01/op-0x07 frames AS THE MASTER, exactly as
 * dlm_arm_serve_enq_frame does: the shipping parse, then the shipping
 * master-side door. */
static void vax_served_as_master(const uint8_t *frame, vms_csid_t from,
				 uint32_t flags,
				 struct vms_dlm_master_result *out)
{
	struct vms_dlm_enq_request e;
	struct vms_dlm_master_request mr;
	uint8_t wireop = 0;
	uint32_t i;

	memset(out, 0, sizeof(*out));
	out->outcome = (uint8_t)VMS_DLM_MASTER_REFUSED;
	if (vms_dlm_enq_request_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					   &wireop, &e) != VMS_CODEC_OK) {
		ct_check(0, "the shipping parser read the VAX's frame back");
		return;
	}
	memset(&mr, 0, sizeof(mr));
	mr.op = (wireop == VMS_DLM_WIREOP_CONVERT) ? VMS_DLM_MREQ_CONVERT :
						     VMS_DLM_MREQ_ENQ;
	mr.req_csid = (uint32_t)from;
	mr.req_lkid = e.req_pid_or_lkid;
	mr.master_lkid = e.master_lkid;
	mr.lkmode = e.mode;
	mr.flags = flags;
	for (i = 0u; i < e.name_len && i < sizeof(mr.resnam) - 1u; i++)
		mr.resnam[i] = (char)e.name[i];
	mr.resnam[i] = '\0';
	(void)vms_lock_dlm_master_serve(&mr, out);
}

/* The VAX's grant for a request OVMX sent it: the shipping grant builder, fed
 * to the shipping FSM entry the arm calls. */
static int vax_grants(uint32_t ovmx_lkid, uint32_t vax_master_lkid, uint8_t mode)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_response_build_grant(ovmx_lkid, vax_master_lkid, mode,
					     frame, (uint32_t)sizeof(frame),
					     &written) != VMS_CODEC_OK)
		return -1;
	return dlm_req_fsm_reply_body(&g.fsm, (vms_csid_t)CSID_VAX, 0u,
				      body_of(frame), VMS_CM_BODY_LEN) ==
	       DLM_REQ_OK ? 0 : -1;
}

/* ==========================================================================
 * 0e. The engine, as a local process uses it
 * ========================================================================== */

static uint32_t do_enq(struct vms_proc *proc, const char *resnam,
		       uint32_t lkmode, uint32_t flags, uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	a.flags = flags;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	vms_ioctl_enq(proc, (unsigned long)(void *)&a);
	if (lkid_out != NULL)
		*lkid_out = a.lkid;
	return a.status;
}

static uint32_t do_deq(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_deq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_deq(proc, (unsigned long)(void *)&a);
	return a.status;
}

static void read_resmaster(const char *resnam, struct vms_resmaster_args *out)
{
	memset(out, 0, sizeof(*out));
	strscpy(out->resnam, resnam, sizeof(out->resnam));
	vms_ioctl_get_resmaster(NULL, (unsigned long)(void *)out);
}

static uint32_t lki_granted_mode(struct vms_proc *proc, uint32_t lkid)
{
	struct vms_getlki_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_getlki(proc, (unsigned long)(void *)&a);
	return a.status == SS__NORMAL ? a.granted_mode : 0xffffffffu;
}

/* The 32-bit value at body[128:132] of the frame that just left. Read through
 * the codec's own published offset, which is the only place that span is
 * named outside the codec TU. */
static uint32_t sent_dir_hash(void)
{
	const uint8_t *b = g.last;

	return (uint32_t)b[VMS_OFB_DLM_DIR_HASH] |
	       ((uint32_t)b[VMS_OFB_DLM_DIR_HASH + 1] << 8) |
	       ((uint32_t)b[VMS_OFB_DLM_DIR_HASH + 2] << 16) |
	       ((uint32_t)b[VMS_OFB_DLM_DIR_HASH + 3] << 24);
}

/* ==========================================================================
 * 1. THE CONFIGURATION IS A READ, NOT A FLAG
 * ========================================================================== */
static void the_configuration_is_derived(void)
{
	printf("-- the sole-directory configuration is the VECTOR's arithmetic "
	       "--\n");

	mixed_up(1u, 0u);
	ct_check(g.cl.club.ldwv.valid && g.cl.club.ldwv.n == 1u,
		 "LOCKDIRWT 1 here / 0 on the VAX builds a ONE-entry vector "
		 "(p. 6-32: entries per system = its LOCKDIRWT)");
	ct_check(sole_directory(NULL) == 1,
		 "*** and every entry is ours: this node is the SOLE directory "
		 "node ***");
	ct_check(vms_ldwv_all_ovmx(&g.cl.club.ldwv) == 0,
		 "the all-OVMX gate is SHUT throughout (a real VAX is a member)");
	mixed_down();

	mixed_up(1u, 1u);
	ct_check(g.cl.club.ldwv.n == 2u && sole_directory(NULL) == 0,
		 "give the VAX a weight and the configuration is GONE -- "
		 "derived, with no flag to go stale");
	mixed_down();

	mixed_up(0u, 0u);
	ct_check(g.cl.club.ldwv.n == 2u && sole_directory(NULL) == 0,
		 "and the V7.3 default (every member 0) is one entry per "
		 "system, which is NOT this configuration");
	mixed_down();
}

/* ==========================================================================
 * 1b. THE 2026-10-09 LAB BED, AND THE FRAME IT REFUSED (rd vms-025)
 *
 * On a real OpenVMS VAX V7.3 mixed cluster -- VAX1 (1025) + VAX2 (1026) at
 * LOCKDIRWT 0 and the OVMX member OVMXE (1030) meant to be at LOCKDIRWT 1 --
 * an OVMX process took NL on the user-mode resource EVAC$WORKLOAD first, and
 * VAX1 then sent ONE cat-0x02 op-0x01 ENQ(EX) for it. OVMXE answered NOTHING,
 * logged "refusing a lock message from a system that has not proved it runs
 * this implementation", and VAX1's $ENQW hung forever.
 *
 * THE CAUSE WAS NOT IN THE FRAME. `SYSBOOT> SET LOCKDIRWT 1` had been answered
 * %SYSGEN-E-NOSUCHP -- the shipped OVMXVMSSYS.PAR carried no LOCKDIRWT row at
 * all -- so OVMXE joined at weight 0, THREE members at weight 0 gave one vector
 * entry per system, vms_ldwv_sole_directory() read FALSE, and every arm of the
 * mixed-cluster DLM was gated off.
 *
 * So this section is two things a lab cannot be asked to infer again:
 *   (a) the EXACT lab member set, built by the shipping Phase 2 fill, with its
 *       verdict either way -- three members at 0 is NOT the configuration, and
 *       the intended weights ARE;
 *   (b) the EXACT captured frame (fixtures/dlm-evac-workload-enq-from-vax1.spec,
 *       record 3907 of the lab pcap) driven through the shipping parser and the
 *       shipping master-side door: unserved on the lab bed, GRANTED at EX on the
 *       intended one -- and granted with values read off the LKB this executive
 *       minted, never echoed from the frame.
 * ========================================================================== */

#define EVAC_RESNAM   "EVAC$WORKLOAD"
/* VAX1's own values, read OFF the captured frame -- not chosen here. */
#define EVAC_VAX_PID  0x2020021eu   /* body[20:24] */
#define EVAC_VAX_LKID 0x1a00021du   /* body[24:28] */
#define EVAC_VAX_HASH 0x00027e10u   /* body[128:132], the VAX's OWN hash */

/* Bring up the lab's THREE-member bed: this node plus TWO real VAXes, neither
 * proven to run this implementation, at the LOCKDIRWTs the caller names. The
 * vector is built by the SHIPPING cnxman_ldwv_rebuild from those weights. */
static void labbed_up(uint8_t ovmx_wt, uint8_t vax1_wt, uint8_t vax2_wt)
{
	struct vms_csb *csb;

	mixed_up(ovmx_wt, vax1_wt);          /* this node + VAX1 */
	csb = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)CSID_VAX2, 1);
	ct_check(csb != NULL, "the lab's SECOND real VAX gets a CSB");
	if (csb == NULL)
		return;
	cnxman_csb_set_csid(csb, CSID_VAX2);
	cnxman_csb_set_lockdirwt(csb, vax2_wt);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	(void)cnxman_ldwv_rebuild(&g.cl.club, &g.cnx_ops);
}

/* The captured frame, loaded and validated by the clean-room specimen loader
 * (sha256 over the assembled bytes, capture listed in the chain of custody).
 * Returns the 204-byte frame, or NULL with the test already reddened. */
static const uint8_t *evac_captured_frame(void)
{
	static struct vms_fixture fx;
	static int loaded;
	char path[600];
	char err[VMS_FIXTURE_ERRLEN] = "";

	if (loaded)
		return fx.wire_len == VMS_CM_FRAME_LEN ? fx.bytes : NULL;
	loaded = 1;
	snprintf(path, sizeof(path), "%s/%s", OVMX_FIXTURE_DIR,
		 "dlm-evac-workload-enq-from-vax1.spec");
	if (vms_fixture_load(path, OVMX_CLEANROOM_MANIFEST, &fx, err,
			     sizeof(err)) != 0) {
		printf("       reason: %s\n", err);
		ct_check(0, "the captured EVAC$WORKLOAD ENQ specimen loads");
		return NULL;
	}
	ct_check(fx.origin == VMS_FIXTURE_ORIGIN_CAPTURE &&
		 fx.wire_len == VMS_CM_FRAME_LEN,
		 "the specimen is a real CAPTURE of the 204-byte frame class");
	return fx.wire_len == VMS_CM_FRAME_LEN ? fx.bytes : NULL;
}

/* What the captured frame SAYS, read by the shipping parsers only. */
static void the_captured_frame_is_what_the_lab_saw(void)
{
	const uint8_t *frame = evac_captured_frame();
	struct vms_dlm_enq_request e;
	struct vms_dlm_res_ident id;
	uint32_t hash = 0;
	uint8_t wireop = 0;

	printf("-- the REAL captured frame, read by the shipping parsers --\n");
	if (frame == NULL)
		return;

	ct_check(vms_dlm_enq_request_parse_body(body_of(frame),
						VMS_CM_BODY_LEN, &wireop, &e) ==
		 VMS_CODEC_OK, "the shipping ENQ parser reads VAX1's frame");
	ct_check_eq_u32(wireop, (uint32_t)VMS_DLM_WIREOP_ENQ,
			"  it is an op-0x01 ENQ, a new lock request");
	ct_check_eq_u32(e.mode, LCK_K_EXMODE,
			"  at EX -- which is COMPATIBLE with the NL this node "
			"held, so the faithful answer was a grant");
	ct_check_eq_u32(e.req_pid_or_lkid, EVAC_VAX_PID,
			"  carrying VAX1's own requesting PID");
	ct_check(e.name_len == (uint8_t)strlen(EVAC_RESNAM) &&
		 memcmp(e.name, EVAC_RESNAM, e.name_len) == 0,
		 "  for the resource EVAC$WORKLOAD");

	ct_check(vms_dlm_res_ident_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					      &id) == VMS_CODEC_OK,
		 "the shipping resource-identity parser reads the same frame");
	ct_check_eq_u32(id.group, 1u, "  UIC group 1, as the lab bed was");
	ct_check_eq_u32(id.mode, 3u,
			"  access mode 3 (USER) -- a user-mode resource, which "
			"is what the evacuation workload locks");
	ct_check(vms_dlm_dir_hash_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					     &hash) == VMS_CODEC_OK &&
		 hash == EVAC_VAX_HASH,
		 "*** and the directory hash is the one VAX1 ITSELF put on the "
		 "wire (Davis p. 6-50) -- the only value OVMX may assert for "
		 "this name ***");
}

/*
 * THE REFUSAL, REPRODUCED. The lab bed exactly: three members, every LOCKDIRWT
 * 0 (because the operator's SET was refused), this node holding NL on
 * EVAC$WORKLOAD and mastering it. The captured frame arrives and the master
 * role is NOT in force -- which is the whole refusal, and it is the PREDICATE
 * that is false, not anything about the frame.
 */
static void on_the_lab_bed_the_configuration_is_not_in_force(void)
{
	struct vms_proc app;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0;

	printf("-- the 2026-10-09 lab bed: three members, every LOCKDIRWT 0 "
	       "--\n");
	labbed_up(0u, 0u, 0u);
	proc_init(&app);

	ct_check(g.cl.club.ldwv.valid && g.cl.club.ldwv.n == 3u,
		 "the vector is one entry per system (p. 6-32's all-zero rule)");
	ct_check_eq_u32((uint32_t)sole_directory(NULL), 0u,
			"*** so this node is NOT the sole lock directory node: "
			"the interim mixed-cluster arm is GATED OFF ***");

	/* The OVMX standby's NL, exactly as the lab's EVACWL took it. */
	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "the OVMX standby holds EVAC$WORKLOAD at NL");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_OVMX,
			"  and this node MASTERS it (no other system had it)");

	/* The engine's own two reads the directory role asks first. BOTH were
	 * true on the lab node, which is why the console's "does not master"
	 * line was false and why it was replaced. */
	ct_check_eq_u32((uint32_t)vms_lock_dlm_name_mastered_here(EVAC_RESNAM),
			1u,
			"*** the engine says this node MASTERS the name -- the "
			"console line that said 'does not master' was wrong ***");
	ct_check_eq_u32((uint32_t)vms_lock_dlm_name_in_use(EVAC_RESNAM), 1u,
			"  and that it holds locks on it");
	mixed_down();
}

/*
 * ...AND THE SAME FRAME IS SERVED once the configuration really holds. Nothing
 * about the frame changed: only this node's LOCKDIRWT, which is what the
 * operator was trying to set at SYSBOOT and could not.
 */
static void with_lockdirwt_set_the_captured_frame_is_granted(void)
{
	const uint8_t *frame = evac_captured_frame();
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0;

	printf("-- the intended bed (this node LOCKDIRWT 1): the SAME captured "
	       "frame is GRANTED --\n");
	if (frame == NULL)
		return;
	labbed_up(1u, 0u, 0u);
	proc_init(&app);

	ct_check(g.cl.club.ldwv.valid && g.cl.club.ldwv.n == 1u,
		 "LOCKDIRWT 1 here and 0 on both VAXes is a ONE-entry vector");
	ct_check_eq_u32((uint32_t)sole_directory(NULL), 1u,
			"*** and this node IS the sole lock directory node ***");

	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "the OVMX standby holds EVAC$WORKLOAD at NL, as before");
	ct_check_eq_u32((uint32_t)vms_lock_dlm_name_mastered_here(EVAC_RESNAM),
			1u, "  and masters it");

	vax_served_as_master(frame, (vms_csid_t)CSID_VAX, 0u, &res);
	ct_check_eq_u32(res.outcome, (uint32_t)VMS_DLM_MASTER_GRANTED,
			"*** VAX1's captured ENQ(EX) is GRANTED by this node as "
			"the resource's master (p. 6-31 outcome (a)) ***");
	ct_check_eq_u32(res.granted_mode, LCK_K_EXMODE,
			"  at EX, the mode the frame asked for and the engine "
			"recorded on the LKB");
	ct_check_eq_u32(res.req_lkid, EVAC_VAX_PID,
			"  and the requester handle is VAX1's own, echoed where "
			"the protocol says the requester's handle goes");
	ct_check(res.master_lkid != 0u && res.master_lkid != EVAC_VAX_LKID,
		 "*** while the MASTER handle is one THIS executive minted -- "
		 "not the frame's body[24:28] plumbed through (RULE B) ***");

	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_OVMX,
			"  and the resource still has exactly ONE master: this "
			"node");
	ct_check_eq_u32(rm.remote_holder_csid, CSID_VAX,
			"  with VAX1 recorded as the remote holder");
	mixed_down();
}

/* ==========================================================================
 * 2. rd vms-025 -- THE VAX LOCKED IT FIRST
 * ========================================================================== */
static void vax_first_then_ovmx_routes_to_the_vax(void)
{
	const char *N = "EVAC$WORKLOAD";
	const uint32_t VAX_HASH = 0x5a3c0117u;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, st;

	printf("-- rd vms-025: the VAX locked it first; an OVMX $ENQ goes to "
	       "the VAX --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	/* The VAX's lookup arrives and this node, its directory, answers "you
	 * master it" -- and records it. This is the shipped behaviour of
	 * rd vms-8219 and the first horn of the hole. */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, VAX_HASH) == 0,
		 "the shipping builder produced the VAX's op-0x01 for " "EVAC$WORKLOAD");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "this node's directory answers 'you master it' and RECORDS "
		 "the VAX as the master");

	/* *** THE $ENQ *** -- a local process locks the same name. */
	st = do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u,
		 "a local $ENQ on that name is accepted (a proxy LKB exists)");

	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"*** and the resource's MASTER is the VAX -- this node "
			"did NOT master it a second time ***");
	ct_check_eq_u32(rm.is_local_master, 0u,
			"  the engine does not claim mastery");

	/* ... and the request really left, for the VAX, carrying the VAX's own
	 * hash. Nothing here is a status-only assertion: the frame is the
	 * evidence (the strawman's defect was a frame, not a status). */
	ct_check_eq_u32(g.n_sent, 1u, "exactly one frame left this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"*** addressed to the VAX the directory named ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_OP],
			(uint32_t)VMS_DLM_WIREOP_ENQ, "  as an op-0x01 request");
	ct_check_eq_u32(sent_dir_hash(), VAX_HASH,
			"*** carrying the hash the VAX ITSELF put on the wire "
			"for that name -- never a computed one ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_REQ_LKID] |
			((uint32_t)g.last[VMS_OFB_DLM_REQ_LKID + 1] << 8) |
			((uint32_t)g.last[VMS_OFB_DLM_REQ_LKID + 2] << 16) |
			((uint32_t)g.last[VMS_OFB_DLM_REQ_LKID + 3] << 24),
			lkid,
			"  and OUR OWN handle, the one this executive minted");

	/* Nothing is granted until the MASTER says so. This check is what makes
	 * the one after it mean something: a build that mastered the name
	 * locally would hold EX here already, and the "lock crossed" assertion
	 * alone would pass for entirely the wrong reason. */
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"*** and NOTHING is granted yet: the requester waits "
			"for the VAX's master, it does not grant itself ***");

	/* The VAX grants it. The lock is now really held here, through the
	 * master's own handle. */
	ct_check(vax_grants(lkid, 0x7fff0001u, (uint8_t)LCK_K_EXMODE) == 0,
		 "the VAX's grant reply is accepted by the shipping FSM");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_EXMODE,
			"*** the local process holds EX on a resource the VAX "
			"masters: the lock crossed ***");

	/* And the release goes back to the VAX -- a lock taken at a remote
	 * master has to be returnable, or the evacuation can never let go. */
	g.n_sent = 0u;
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the local $DEQ succeeds");
	(void)dlm_req_fsm_tick(&g.fsm);
	mixed_down();
}

/* NEGATIVE CONTROL for §2: with NOTHING in the directory, the same $ENQ
 * masters LOCALLY -- which is the old behaviour, and is still correct when the
 * cluster has told this node nothing about the name. */
static void with_no_entry_the_enq_masters_locally(void)
{
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0;

	printf("-- negative: an empty directory still masters locally --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	ct_check(do_enq(&app, "OVMXFIRST1", LCK_K_EXMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u, "the $ENQ is granted");
	read_resmaster("OVMXFIRST1", &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"this node masters it (nobody else has claimed it)");
	ct_check_eq_u32(g.n_sent, 0u,
			"and NOTHING went on the wire -- no lookup with a hash "
			"this node does not hold");
	mixed_down();
}

/* NEGATIVE CONTROL for §2: outside the configuration the directory entry is
 * NOT acted on, and the previous behaviour stands exactly. This is what makes
 * the change safe to land: a default-weight cluster sees no difference. */
static void outside_the_configuration_nothing_changes(void)
{
	const char *N = "EVAC$WORKLOAD";
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0;

	printf("-- negative: with default weights the interim is INERT --\n");
	mixed_up(0u, 0u);                 /* the V7.3 default on both */
	proc_init(&app);

	ct_check(sole_directory(NULL) == 0, "not the sole directory node");
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, 0x5a3c0117u) == 0,
		 "the VAX's frame is built");
	(void)vax_lookup(frame, CSID_VAX, &master);

	ct_check(do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL,
		 "the local $ENQ is granted");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"*** and masters LOCALLY, exactly as before this item: "
			"the interim configuration is not in force ***");
	ct_check_eq_u32(g.n_sent, 0u, "nothing was routed anywhere");
	mixed_down();
}

/* A directory entry whose hash this node never learned is a REFUSAL, not a
 * frame with a zero in it (the grant storm) and not a second local master. */
static void an_entry_without_a_hash_is_refused(void)
{
	struct vms_dlm_res_ident id;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0, st;
	const char *N = "REGONLY1";

	printf("-- an entry from a REGISTRATION (no learnable hash): refused "
	       "--\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	/* An op-0x0d registration records the master. The hash learner does
	 * not read an op-0x0d (the value is only grounded on op-0x01), so this
	 * executive holds NO hash for the name. */
	memset(&id, 0, sizeof(id));
	id.hash = 0u;                 /* the entry carries none */
	id.group = 0u;
	id.mode = 3u;
	id.name_len = (uint8_t)strlen(N);
	memcpy(id.name, N, id.name_len);
	ct_check(vms_dlm_dir_register(&g.dir, &id, CSID_VAX) == 0,
		 "the VAX registers itself as the master of " "REGONLY1");

	st = do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__UNSUPPORTED && lkid == 0u,
		 "*** the $ENQ is REFUSED honestly: the VAX masters it and "
		 "this node has no hash to address it with ***");
	ct_check_eq_u32(g.n_sent, 0u,
			"  and NOTHING left this node -- never a frame with a "
			"zero where the hash goes");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.is_local_master, 0u,
			"  and it did NOT become a second master instead");
	mixed_down();
}

/*
 * A FULL directory table must not stop this node from locking. The claim is
 * belt-and-braces -- the directory role asks the ENGINE first -- so a table
 * that cannot take another entry costs a counter, not a $ENQ.
 */
static void a_full_directory_table_still_locks(void)
{
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t i, lkid = 0, failures = 0;
	char nm[24];

	printf("-- a FULL directory table still masters locally (no wedge) --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	/* 64 slots, one eighth kept free: well past the bound after 80 names. */
	for (i = 0u; i < 80u; i++) {
		lkid = 0;
		snprintf(nm, sizeof(nm), "FULLTAB%u", i);
		if (do_enq(&app, nm, LCK_K_EXMODE, 0u, &lkid) != SS__NORMAL ||
		    lkid == 0u)
			failures++;
	}
	ct_check_eq_u32(failures, 0u,
			"*** 80 local $ENQs on a 64-slot directory table ALL "
			"succeed: a bounded table never refuses a lock ***");
	ct_check(g.dir.full_refusals > 0u,
		 "  and the refusals to RECORD are counted, not hidden");
	read_resmaster("FULLTAB79", &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"  the last name is mastered here, as it always was");
	ct_check_eq_u32(g.n_sent, 0u, "  and nothing went on the wire");
	mixed_down();
}

/* ==========================================================================
 * 3. rd vms-db2a -- OVMX LOCKED IT FIRST
 * ========================================================================== */
static void ovmx_first_then_the_vax_is_served_as_master(void)
{
	const char *N = "EVAC$WORKLOAD";
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0;

	printf("-- rd vms-db2a: OVMX masters it; the VAX is SERVED, not told "
	       "to master it --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	ct_check(do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "a local $ENQ takes EX and masters the name here");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u, "  this node is the master");
	ct_check(vms_lock_dlm_name_mastered_here(N) == 1,
		 "  and says so through the read the directory role uses");
	ct_check(vms_dlm_dir_lookup_name(&g.dir, N, (uint32_t)strlen(N),
					 &master, NULL, NULL) ==
		 VMS_DLM_DIR_NAME_MASTER && master == CSID_OVMX,
		 "*** and the mastery is RECORDED in this node's own directory "
		 "-- which is what stops the next asker being told to master "
		 "it ***");

	/* *** THE VAX ASKS *** with its own group, its own access mode and its
	 * own hash, none of which this node holds for the name. */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, 0x5a3c0117u) == 0,
		 "the VAX's op-0x01 is built");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_SELF && master == CSID_OVMX,
		 "*** the directory answers THIS NODE MASTERS IT (p. 6-51), "
		 "not 'you master it' ***");
	ct_check_eq_u32(g.dir.answered_you, 0u,
			"  nothing was answered 'you master it'");

	/* ... and the master role resolves it: EX against a local EX holder
	 * QUEUES, genuinely, on a real waiting queue. */
	vax_served_as_master(frame, CSID_VAX, 0u, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED &&
		 res.master_lkid != 0u,
		 "*** the VAX's EX request is QUEUED by this executive, behind "
		 "the local EX holder ***");
	ct_check_eq_u32(res.req_lkid, VAX_LKID_1,
			"  and the queued LKB carries the VAX's OWN handle, "
			"read back off the LKB the engine stamped");

	/* The local holder releases: the VAX's request flips to GRANTED. The
	 * lock database moved -- which is the whole evacuation in one step,
	 * in the direction OVMX -> VAX. */
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the local holder $DEQs");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.n_granted, 1u,
			"*** one lock is granted on the resource now ***");
	ct_check_eq_u32(rm.remote_holder_csid, CSID_VAX,
			"*** and it is held FOR THE VAX's cluster identity: a "
			"real grant to a real VMS system ***");
	ct_check_eq_u32(rm.is_local_master, 1u,
			"  with this node still the one master");
	mixed_down();
}

/* The NOQUEUE case: SS$_NOTQUEUED is a real refusal of a real conflict. */
static void a_vax_noqueue_request_is_denied(void)
{
	const char *N = "EVAC$NOQ";
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;
	struct vms_proc app;
	uint32_t lkid = 0;

	printf("-- the VAX's NOQUEUE request on a held resource is DENIED --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	ct_check(do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL,
		 "this node holds EX and masters it");
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_2,
			       (uint8_t)LCK_K_EXMODE, N, 0x11223344u) == 0,
		 "the VAX's op-0x01 is built");
	vax_served_as_master(frame, CSID_VAX, LCK_M_NOQUEUE, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_DENIED,
		 "*** DENIED -- the deny shape the real wire grounds, from a "
		 "real incompatibility ***");
	ct_check_eq_u32(res.master_lkid, 0u,
			"  and no lock id is asserted, because no lock exists");
	mixed_down();
}

/* A CONVERT from the VAX, and the BLOCKING AST this master owes a remote
 * holder -- named from executive state, and honestly NOT emitted. */
static void a_convert_names_the_blocking_holder(void)
{
	const char *N = "EVAC$CVT";
	uint8_t f1[VMS_CM_FRAME_LEN], f2[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result held, queued;
	struct vms_proc app;
	uint32_t lkid = 0;

	printf("-- a VAX holder, an NL->EX convert behind it, and the BLKAST "
	       "this master OWES --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	/* This node masters the name (it touched it first, at NL). */
	ct_check(do_enq(&app, N, LCK_K_NLMODE, 0u, &lkid) == SS__NORMAL,
		 "this node masters the name at NL");

	/* The VAX takes EX across the wire -- compatible with NL, so granted. */
	ct_check(vax_build_enq(f1, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, 0x22334455u) == 0,
		 "the VAX's EX request is built");
	vax_served_as_master(f1, CSID_VAX, 0u, &held);
	ct_check(held.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 held.granted_mode == LCK_K_EXMODE,
		 "the VAX is GRANTED EX by this master (NL is compatible)");

	/* A SECOND VMS system now converts its NL to EX: incompatible with the
	 * VAX's EX, so it queues -- and the master owes the VAX a blocking AST,
	 * named from the blocking LKB. */
	ct_check(vax_build_enq(f2, VMS_DLM_WIREOP_CONVERT, VAX_LKID_2,
			       (uint8_t)LCK_K_EXMODE, N, 0x22334455u) == 0,
		 "the second system's op-0x07 convert is built");
	vax_served_as_master(f2, CSID_VAX2, 0u, &queued);
	ct_check(queued.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED,
		 "*** the convert QUEUES behind the VAX's EX ***");
	ct_check_eq_u32(queued.blocking_csid, CSID_VAX,
			"*** and the engine NAMES the blocking holder's CSID, "
			"read off that holder's own LKB ***");
	ct_check_eq_u32(queued.blocking_master_lkid, held.master_lkid,
			"  with OUR handle for it");
	ct_check_eq_u32(queued.blocking_req_lkid, VAX_LKID_1,
			"  and the holder's own handle, so the holder could "
			"find its lock by a value its executive produced");
	/*
	 * HONEST GAP, AND WHERE IT IS PINNED. The engine's decision above is
	 * real; the op-0x05 frame that would TELL the VAX is NOT emitted at a
	 * system which has not proved it runs this implementation -- its
	 * mode-context pair at body[30:32] is observed-and-not-pinned, so OVMX
	 * would have to write a zero where every real frame carries data. That
	 * is an ARM fact (dlm_arm_send_blkast's all-OVMX gate, and the
	 * blkasts_no_wire_op counter under it), so it is asserted where arm
	 * facts are asserted: test_dlm_scs_arm.c's source scan. It is not
	 * restated here as a check this file cannot actually see.
	 */
	mixed_down();
}

/* ==========================================================================
 * 4. rd vms-c27 condition 3 / rd vms-4d3 -- THE VAX LEAVES
 * ========================================================================== */
static void the_vax_departing_releases_its_locks(void)
{
	const char *N = "EVAC$DEPART";
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result held;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0, n = 0;

	printf("-- rd vms-4d3: the VAX departs, its locks go, the waiter is "
	       "GRANTED --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	/* This node masters the name at NL; the VAX takes EX across the wire. */
	ct_check(do_enq(&app, N, LCK_K_NLMODE, 0u, &lkid) == SS__NORMAL,
		 "this node masters the name");
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "and lets its NL go");
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, 0x33445566u) == 0,
		 "the VAX's EX request is built");
	vax_served_as_master(frame, CSID_VAX, 0u, &held);
	ct_check(held.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "the VAX holds EX here");

	/* A local process queues behind it -- the standby, waiting to take the
	 * workload over. Asynchronous, so the test is not the one blocking. */
	lkid = 0;
	ct_check(do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "a local $ENQ EX is accepted");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"*** and is genuinely QUEUED (granted mode still NL) "
			"behind the VAX ***");

	/* *** THE DEPARTURE *** */
	ct_check(vms_lock_dlm_release_csid_locks(CSID_VAX, &n) == SS__NORMAL,
		 "the per-CSID sweep runs");
	ct_check_eq_u32(n, 1u,
			"*** exactly ONE lock was released -- the VAX's ***");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_EXMODE,
			"*** and the local waiter is now GRANTED EX: the "
			"workload can take over ***");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.remote_holder_csid, 0u,
			"  no remote-held grant is left on the resource");

	/* NEGATIVE CONTROL: another CSID's departure releases NOTHING of the
	 * locks this node holds for anybody else -- condition 2's tag is what
	 * selects, and it is read off the LKB. */
	n = 0u;
	(void)vms_lock_dlm_release_csid_locks(CSID_VAX2, &n);
	ct_check_eq_u32(n, 0u,
			"a different CSID's departure releases nothing");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_EXMODE,
			"  and the local lock is untouched");
	ct_check(vms_lock_dlm_release_csid_locks(0u, &n) != SS__NORMAL,
		 "and CSID 0 is refused: 'no system' is not a system");
	mixed_down();
}

/* The local locks of THIS node are never swept by a peer's departure: a local
 * lock carries req_csid 0, which is not a cluster identity. */
static void a_departure_never_touches_a_local_lock(void)
{
	struct vms_proc app;
	uint32_t lkid = 0, n = 0;

	printf("-- negative: a departure never releases a LOCAL lock --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	ct_check(do_enq(&app, "EVAC$LOCALONLY", LCK_K_EXMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u, "a local lock is held at EX");
	(void)vms_lock_dlm_release_csid_locks(CSID_VAX, &n);
	ct_check_eq_u32(n, 0u, "the VAX's departure releases nothing");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_EXMODE,
			"and the local lock still holds EX");
	mixed_down();
}

int main(void)
{
	printf("=== test_dlm_mixed_master (rd vms-025 / vms-db2a / vms-c27: "
	       "ONE master per resource in a MIXED cluster, R1) ===\n");

	the_configuration_is_derived();

	the_captured_frame_is_what_the_lab_saw();
	on_the_lab_bed_the_configuration_is_not_in_force();
	with_lockdirwt_set_the_captured_frame_is_granted();

	vax_first_then_ovmx_routes_to_the_vax();
	with_no_entry_the_enq_masters_locally();
	outside_the_configuration_nothing_changes();
	an_entry_without_a_hash_is_refused();
	a_full_directory_table_still_locks();

	ovmx_first_then_the_vax_is_served_as_master();
	a_vax_noqueue_request_is_denied();
	a_convert_names_the_blocking_holder();

	the_vax_departing_releases_its_locks();
	a_departure_never_touches_a_local_lock();

	return ct_summary("test_dlm_mixed_master");
}
