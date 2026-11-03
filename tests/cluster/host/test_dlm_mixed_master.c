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
 *   (a) vms_lock.c's dir_resolve() returned "this node" for EVERY name while
 *       any member was non-OVMX (the all-OVMX gate, retired by rd vms-b5b0),
 *       and the local $ENQ path never consulted the directory table THIS NODE
 *       HOLDS. So a real VAX that locked N first -- its lookup answered and
 *       RECORDED here, naming the VAX as N's master (rd vms-8219) -- and a
 *       local $ENQ on N gave TWO masters for one resource.
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
 *                  from LOCKDIRWTs set on those CSBs. "Which node directs this
 *                  resource" is therefore the REAL vector's own arithmetic over
 *                  the REAL hash (vms_ldwv_resolve o vms_dlm_name_hash_proven),
 *                  never a test flag.
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
 * WHAT rd vms-b5b0 CHANGED IN THIS FILE. The scenarios used to run inside the
 * INTERIM CONFIGURATION -- every real VMS member at LOCKDIRWT 0 and this node
 * above 0, so that the vector directed every root name here and a directory
 * decision needed no hash. Section 1 now runs at the V7.3 DEFAULT (every member
 * 0, so about half of all names are directed at the VAX) and asserts the thing
 * the interim could not do: an OVMX-first $ENQ on a name the vector directs AT
 * THE VAX leaves as one op-0x01, addressed there, carrying the value the proven
 * resource-name hash computes. The sections after it keep weights 1/0 -- not as
 * a gate (there is none any more) but because that vector directs every name
 * here, which is the configuration those scenarios are about.
 *
 * EVERY SECTION CARRIES ITS NEGATIVE CONTROL. Reverting the fix has to make a
 * check here fail, so each scenario also asserts the behaviour with the
 * directory entry absent -- and section 1's control is the one that keeps a
 * real VAX safe: an identity outside the PROVEN coverage puts NO frame on the
 * wire at all.
 */
#include "cluster_test.h"
#include "cluster_fixture.h"  /* the REAL captured frame, rd vms-025 */

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_f11b_ser.h"
#include "vms_dlm_dir.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_scs_fsm.h"
#include "vms_dlm_rm_fsm.h"   /* rd vms-33a: the holder's half, end to end */
#include "vms_dlm_adopt.h"
#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_master_wire.h" /* rd vms-1e58: the ARM's own translation */
#include "vms_dlm_hash.h"     /* the PROVEN resource-name hash the engine routes
			       * by for a resource no frame has named (rd
			       * vms-b5b0) */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/*
 * THE RESOURCE IDENTITY EVERY LOCK IN THIS FILE NAMES (rd vms-b5b0). The local
 * processes carry uic 0 and run at access mode 0, so the resources they create
 * are (group 0, mode 0); the scripted VAX's frames carry the SAME identity,
 * because these scenarios are about ONE resource with two claimants -- which is
 * only one resource if both sides name the same domain.
 */
#define OVMX_RES_GROUP 0u
#define OVMX_RES_MODE  0u

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
	/* rd vms-d63: the last few frames, in order, for a sequence check. */
	uint8_t  hist[8][VMS_CM_BODY_LEN];
	/* rd vms-d63: a release is STAGED and CLAIMED through the same release
	 * queue the wire arm uses (vms_dlm_scs.c dlm_arm_post_release ->
	 * dlm_arm_run_release), so what the queue keeps is what goes out. */
	struct dlm_relq relq;
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
	/* Every PCB the executive makes carries a process id, and the lock
	 * manager puts it at body[20:24] of a cross-node request that has no
	 * master handle yet (rd vms-c55). A PCB with none is one the executive
	 * never made, so the harness gives each one its own. */
	{
		static uint32_t next_vms_pid = 0x10000001u;

		p->vms_pid = next_vms_pid++;
	}
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

/* The engine's ask, as a wire identity -- dlm_arm_ask_to_ident. */
static int ask_to_ident(const struct vms_dlm_dir_ask *q,
			struct vms_dlm_res_ident *id)
{
	uint32_t i;

	if (q->name == NULL || q->name_len == 0u ||
	    q->name_len > VMS_DLM_NAME_MAX)
		return -1;
	memset(id, 0, sizeof(*id));
	id->hash = q->hash;
	id->group = q->group;
	id->mode = q->mode;
	id->name_len = (uint8_t)q->name_len;
	for (i = 0u; i < q->name_len; i++)
		id->name[i] = (uint8_t)q->name[i];
	return 0;
}

static uint32_t eng_dir_local_lookup(void *ctx, const struct vms_dlm_dir_ask *q,
				     struct vms_dlm_dir_local *out)
{
	struct vms_dlm_res_ident id;
	enum vms_dlm_dir_ident_outcome o;
	vms_csid_t master = 0u;
	uint32_t hash = 0u;
	uint8_t hash_known = 0u;

	(void)ctx;
	if (q == NULL || out == NULL || ask_to_ident(q, &id) != 0)
		return SS__BADPARAM;
	o = vms_dlm_dir_lookup_ident(&g.dir, &id, &master, &hash, &hash_known);
	if (o == VMS_DLM_DIR_IDENT_INVAL)
		return SS__UNSUPPORTED;
	if (o == VMS_DLM_DIR_IDENT_NONE) {
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

static uint32_t eng_dir_claim_self(void *ctx, const struct vms_dlm_dir_ask *q)
{
	struct vms_dlm_res_ident id;

	(void)ctx;
	if (q == NULL || ask_to_ident(q, &id) != 0)
		return SS__BADPARAM;
	return vms_dlm_dir_claim_self(&g.dir, &id, (vms_csid_t)CSID_OVMX) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__INSFMEM;
}

/* rd vms-3ea: the engine's "this node no longer masters it" -- the SAME table
 * call the shipping arm makes (dlm_arm_eng_dir_release_self). */
static uint32_t eng_dir_release_self(void *ctx, const struct vms_dlm_dir_ask *q)
{
	struct vms_dlm_res_ident id;

	(void)ctx;
	if (q == NULL || ask_to_ident(q, &id) != 0)
		return SS__BADPARAM;
	return vms_dlm_dir_remove(&g.dir, &id, (vms_csid_t)CSID_OVMX) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__IVLOCKID;
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

/* The one question the vector answers WITHOUT a value (rd vms-b5b0), read
 * exactly as the arm reads it: dlm_arm_eng_dir_all_ours. */
static int eng_dir_all_ours(void *ctx)
{
	(void)ctx;
	return vms_ldwv_directs_everything_here(&g.cl.club.ldwv);
}

/*
 * WHICH NODE THE VECTOR NAMES FOR A RESOURCE, computed the way the engine
 * computes it: the PROVEN resource-name hash over the resource's identity, then
 * the published index rule (rd vms-b5b0). The tests use it to pick names whose
 * directory is this node and names whose directory is the VAX -- so a routing
 * assertion is about the real vector's real arithmetic, not a chosen constant.
 */
static vms_csid_t dir_node_for(const char *name, uint16_t group, uint8_t mode)
{
	vms_csid_t csid = 0;
	uint32_t h = 0;

	if (vms_dlm_name_hash_proven(group, mode, (const uint8_t *)name,
				     (uint32_t)strlen(name), &h) !=
	    VMS_DLM_HASH_OK)
		return 0;
	if (vms_ldwv_resolve(&g.cl.club.ldwv, vms_ldwv_key(h), &csid) !=
	    VMS_LDWV_OK)
		return 0;
	return csid == 0u ? (vms_csid_t)CSID_OVMX : csid;
}

/* The first of `names` whose directory node is `want`, or NULL. */
static const char *a_name_directed_at(vms_csid_t want, const char *const *names,
				      uint32_t n)
{
	uint32_t i;

	for (i = 0u; i < n; i++) {
		if (dir_node_for(names[i], OVMX_RES_GROUP, OVMX_RES_MODE) ==
		    want)
			return names[i];
	}
	return NULL;
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
	memmove(g.hist[1], g.hist[0], sizeof(g.hist) - sizeof(g.hist[0]));
	memcpy(g.hist[0], g.last, sizeof(g.last));
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

static int fsm_record_master(void *ctx, const char *resnam, uint32_t resnam_len, uint32_t req_lkid,
			     vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, resnam_len, req_lkid,
					  (uint32_t)master_csid) == SS__NORMAL ?
	       0 : -1;
}

static int fsm_assume(void *ctx, const char *resnam, uint32_t resnam_len, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, resnam_len, req_lkid) == SS__NORMAL ?
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

static int fsm_learn(void *ctx, const char *resnam, uint32_t resnam_len, uint16_t group,
		     uint8_t mode, uint32_t hash)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, resnam_len, group, mode, hash) ==
	       SS__NORMAL ? 0 : -1;
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
	if (p->op == VMS_DLM_POST_DEQ) {
		struct vms_dlm_proxy_post c;
		uint32_t slot = 0u, seq = 0u;

		if (dlm_relq_stage(&g.relq, p, &slot, &seq) != DLM_REQ_OK ||
		    dlm_relq_claim(&g.relq, slot, seq, &c) != DLM_REQ_OK)
			return SS__INSFMEM;
		return dlm_req_fsm_post(&g.fsm, &c) == DLM_REQ_OK ?
		       (uint32_t)SS__NORMAL : (uint32_t)SS__UNSUPPORTED;
	}
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
	dlm_relq_init(&g.relq);
	/* No frame of an EARLIER scenario may stand in for one this scenario
	 * never sent (rd vms-7649: a check of g.last after a send that did not
	 * happen read the previous scenario's frame). */
	memset(g.last, 0, sizeof(g.last));
	vms_local_csid = CSID_OVMX;

	club_up(ovmx_wt, vax_wt);
	(void)vms_dlm_dir_init(&g.dir, g.dir_store,
			       (uint32_t)(sizeof(g.dir_store) /
					  sizeof(g.dir_store[0])));

	g.fsm_ops.send           = fsm_send;
	g.fsm_ops.refill_post    = fsm_refill;
	g.fsm_ops.dir_resolve    = fsm_dir_resolve;
	g.fsm_ops.dir_generation = fsm_dir_generation;
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
	g.eng_ops.dir_all_ours     = eng_dir_all_ours;
	g.eng_ops.dir_local_lookup = eng_dir_local_lookup;
	g.eng_ops.dir_claim_self   = eng_dir_claim_self;
	g.eng_ops.dir_release_self = eng_dir_release_self;
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
static int vax_build_enq_flags(uint8_t *frame, uint8_t opcode,
			       uint32_t vax_lkid, uint8_t mode,
			       const char *name, uint32_t hash,
			       uint16_t wire_flags)
{
	struct vms_dlm_enq_request r;
	uint32_t written = 0;

	memset(&r, 0, sizeof(r));
	r.mode = mode;
	r.req_flags = wire_flags;       /* body[28:30], LCK$M_ public encoding */
	r.req_lkid = vax_lkid;
	r.name_len = (uint8_t)strlen(name);
	memcpy(r.name, name, r.name_len);
	r.dir_hash = hash;
	r.dir_hash_valid = 1u;
	/* body[44:46] + body[46]: the identity that qualifies the name. The
	 * builder refuses a frame that does not state it (rd vms-b5b0). */
	r.res_group = OVMX_RES_GROUP;
	r.res_acmode = OVMX_RES_MODE;
	r.res_ident_valid = 1u;
	/* body[20:24]: a real member's REQUESTING PID until the master names a
	 * handle (rd vms-c55; 20,368 of 21,499 real op-0x01 requests) */
	r.req_pid = 0x20200204u;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_request_build(&r, opcode, frame, VMS_CM_FRAME_LEN,
					 &written) == VMS_CODEC_OK ? 0 : -1;
}

static int vax_build_enq(uint8_t *frame, uint8_t opcode, uint32_t vax_lkid,
			 uint8_t mode, const char *name, uint32_t hash)
{
	return vax_build_enq_flags(frame, opcode, vax_lkid, mode, name, hash, 0u);
}

/*
 * ONE op-0x07 CONVERT as a real VAX puts it on the wire: it names its lock by
 * the two handles and carries a STALE identity span -- which is exactly the
 * measured shape (docs/design-dlm-name-hash.md §4a: 2 of 8 real op-0x07 frames
 * carried a half-overwritten name whose hash belonged to another resource). So
 * the builder is told `res_ident_valid = 0` and writes nothing there, and the
 * parser will not vouch for it either: the engine takes the resource from the
 * LOCK the convert names.
 */
static int vax_build_convert(uint8_t *frame, uint32_t vax_lkid,
			     uint32_t master_lkid, uint8_t mode)
{
	struct vms_dlm_enq_request r;
	uint32_t written = 0;

	memset(&r, 0, sizeof(r));
	r.mode = mode;
	r.req_lkid = vax_lkid;
	r.master_lkid = master_lkid;
	r.name_len = 0u;
	r.res_ident_valid = 1u;   /* the BUILDER needs a value to write; the
				   * PARSER refuses to vouch for an op-0x07's,
				   * which is the half that matters */
	r.res_group = OVMX_RES_GROUP;
	r.res_acmode = OVMX_RES_MODE;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_request_build(&r, VMS_DLM_WIREOP_CONVERT, frame,
					 VMS_CM_FRAME_LEN, &written) ==
	       VMS_CODEC_OK ? 0 : -1;
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
		/* The COUNT this harness copied, like the real arm (rd vms-326). */
		(void)vms_lock_dlm_learn_dir_hash(nm, i, id.group, id.mode, hash);
	}
	return vms_dlm_dir_lookup(&g.dir, &id, from, (vms_csid_t)CSID_OVMX,
				  out_master);
}

/* Serve one of the VAX's op-0x01/op-0x07 frames AS THE MASTER, exactly as
 * dlm_arm_serve_enq_frame does: the shipping parse, the shipping translation
 * (vms_dlm_master_wire.h -- the arm's own), then the shipping master-side door.
 *
 * THE REQUESTER'S OPTIONS COME OFF THE FRAME (rd vms-1e58 C4). This harness
 * used to take them as a parameter, and so it handed the master an
 * LCK_M_NOQUEUE the real arm never read off any frame: the NOQUEUE test here
 * was green while a real VAX's EX/NOQUEUE was QUEUED in the lab. */
static void vax_served_as_master(const uint8_t *frame, vms_csid_t from,
				 struct vms_dlm_master_result *out)
{
	struct vms_dlm_enq_request e;
	struct vms_dlm_master_request mr;
	uint8_t wireop = 0;

	memset(out, 0, sizeof(*out));
	out->outcome = (uint8_t)VMS_DLM_MASTER_REFUSED;
	if (vms_dlm_enq_request_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					   &wireop, &e) != VMS_CODEC_OK) {
		ct_check(0, "the shipping parser read the VAX's frame back");
		return;
	}
	vms_dlm_master_req_from_wire(&e, wireop, (uint32_t)from, &mr);
	(void)vms_lock_dlm_master_serve(&mr, out);
}

/*
 * The VAX's grant for a request OVMX sent it: the shipping grant builder, fed
 * to the shipping FSM entry the arm calls.
 *
 * IT ECHOES THE REQUEST OVMX ACTUALLY SENT (rd vms-b5b0) -- g.last, the bytes
 * the FSM put on the wire a moment ago -- because that is what a real master
 * does, and because it makes the handle correlation REAL here: the grant can
 * only carry OVMX's own handle back if OVMX put it in the slot a master reads.
 * `ovmx_lkid` and `mode` are now only cross-checks on that frame.
 */
static int vax_grants(uint32_t ovmx_lkid, uint32_t vax_master_lkid, uint8_t mode)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	(void)ovmx_lkid;
	(void)mode;
	if (g.n_sent == 0u)
		return -1;              /* nothing to answer */
	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_response_build_grant(g.last, VMS_CM_BODY_LEN,
					     vax_master_lkid, NULL, 0u, frame,
					     (uint32_t)sizeof(frame),
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
 * 1. THE VECTOR ROUTES, AT THE V7.3 DEFAULT LOCKDIRWT (rd vms-b5b0)
 *
 * THE HEADLINE, and the thing the interim could not do. With every member at
 * LOCKDIRWT 0 -- the default, and what a real cluster looks like unless somebody
 * sets it -- p. 6-32 gives ONE vector entry per system, so about half of all
 * root names are directed at the VAX. The interim configuration (every VAX at 0
 * and this node above 0) existed precisely because that case needs the
 * resource-name hash; it is now computed and proven, so this test asserts the
 * general case directly:
 *
 *   - the vector is the SHIPPING Phase 2 fill over real CSBs;
 *   - the directory node for a name is the SHIPPING index rule over the
 *     SHIPPING hash;
 *   - and an OVMX-first $ENQ on a name the vector directs AT THE VAX puts one
 *     frame on the wire, addressed to the VAX, carrying that computed value at
 *     body[128:132] and the resource's own identity at body[44:48].
 * ========================================================================== */
static void the_vector_routes_at_the_default_lockdirwt(void)
{
	/* Candidates only: WHICH of them is directed at the VAX is the
	 * vector's arithmetic, and the test asks it rather than assuming. */
	static const char *const cands[] = {
		"EVAC$W", "EVAC$WK", "EVAC$WKL", "EVAC$WKLD", "EVAC$WORK",
		"EVAC$WORKL", "EVAC$WORKLD", "EVAC$JOB", "EVAC$Q",
		"EVAC$QUEUE", "EVAC$VOL", "EVAC$FILE", "EVAC$DB",
		"EVAC$IDX", "EVAC$LOG"
	};
	const char *n_vax, *n_self;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0, expect = 0, st;

	printf("-- rd vms-b5b0: at the DEFAULT LOCKDIRWT the vector routes to "
	       "the VAX --\n");
	mixed_up(0u, 0u);                 /* the V7.3 default on both */
	proc_init(&app);

	ct_check(g.cl.club.ldwv.valid && g.cl.club.ldwv.n == 2u,
		 "every member at LOCKDIRWT 0 gives ONE entry per system "
		 "(p. 6-32)");
	ct_check(vms_ldwv_all_ovmx(&g.cl.club.ldwv) == 0,
		 "and the VAX is NOT proven to run this implementation");

	n_vax = a_name_directed_at((vms_csid_t)CSID_VAX, cands,
				   (uint32_t)(sizeof(cands) / sizeof(cands[0])));
	n_self = a_name_directed_at((vms_csid_t)CSID_OVMX, cands,
				    (uint32_t)(sizeof(cands) / sizeof(cands[0])));
	ct_check(n_vax != NULL && n_self != NULL,
		 "the vector directs some root names here and some at the VAX "
		 "-- which is what a default-weight cluster IS");
	if (n_vax == NULL || n_self == NULL) {
		mixed_down();
		return;
	}

	/* *** THE $ENQ this item exists for: a name OVMX touches FIRST, whose
	 * directory is the VAX. *** */
	ct_check(vms_dlm_name_hash(OVMX_RES_GROUP, OVMX_RES_MODE,
				   (const uint8_t *)n_vax,
				   (uint32_t)strlen(n_vax), &expect) ==
		 VMS_DLM_HASH_OK, "the function answers for it");
	st = do_enq(&app, n_vax, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u,
		 "the $ENQ is accepted (a proxy LKB exists)");
	ct_check_eq_u32(g.n_sent, 1u, "exactly one frame left this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"*** addressed to the VAX -- the directory node the "
			"vector names for that value ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_OP],
			(uint32_t)VMS_DLM_WIREOP_ENQ, "  as an op-0x01 lookup");
	ct_check_eq_u32(sent_dir_hash(), expect,
			"*** carrying the value the PROVEN function computes "
			"for the resource's identity ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_MODE], OVMX_RES_MODE,
			"  and the access mode that value is OF (body[46])");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP] |
			((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP + 1] << 8),
			OVMX_RES_GROUP,
			"  and the UIC group (body[44:46])");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"*** and NOTHING is granted yet: the requester waits "
			"for the directory's answer ***");
	read_resmaster(n_vax, &rm);
	ct_check_eq_u32(rm.dir_csid, CSID_VAX,
			"  the readback names the VAX as the directory");
	ct_check_eq_u32(rm.is_local_master, 0u,
			"*** and this node did NOT master it locally -- the "
			"interim's behaviour, retired ***");

	/* A name the SAME vector directs HERE still masters locally on first
	 * use: p. 6-31's "the directory node simply assumes mastery". */
	g.n_sent = 0u;
	lkid = 0;
	ct_check(do_enq(&app, n_self, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "a name the vector directs HERE is granted");
	read_resmaster(n_self, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"*** mastered HERE, because this node IS its directory "
			"and nobody else claims it (p. 6-31) ***");
	ct_check_eq_u32(g.n_sent, 0u, "  and nothing went on the wire for it");
	mixed_down();
}

/* ==========================================================================
 * 1b. THE 2026-10-09 REAL-VAX LAB BED, AND THE FRAME IT REFUSED
 *
 * (rd vms-025. Capture: tests/lab/captures/vms-025-mixed-dlm-refusal-20261009/.)
 *
 * The bed, exactly: VAX1 (1025) + VAX2 (1026), both real OpenVMS VAX V7.3, and
 * one OVMX member -- THREE systems, EVERY LOCKDIRWT 0 (the V7.3 default, and
 * what the lab really had: the operator's `SYSBOOT> SET LOCKDIRWT 1` was
 * answered %SYSGEN-E-NOSUCHP). Group 1, user mode, resource EVAC$WORKLOAD.
 *
 * TWO ARMS WERE RUN, AND BOTH FAILED on the code that shipped at the time:
 *
 *   ARM A  An OVMX process took NL on EVAC$WORKLOAD first. VAX1 then sent ONE
 *          cat-0x02 op-0x01 ENQ(EX). OVMX answered NOTHING and logged
 *          "refusing a lock message from a system that has not proved it runs
 *          this implementation"; VAX1's $ENQW hung forever, though EX is
 *          compatible with NL and OVMX WAS the resource's master.
 *   ARM B  VAX1 took EX first and OVMX answered its lookup "you master it";
 *          then an OVMX process was granted EX LOCALLY while VAX1 still held
 *          EX -- two holders of one resource.
 *
 * This section is both arms, on the lab's own bed, driven by the REAL CAPTURED
 * FRAME (fixtures/dlm-evac-workload-enq-from-vax1.spec, record 3907 of the lab
 * pcap, origin: capture). The frame is not rebuilt from a template: its bytes
 * are the VAX's, including the value VAX1 itself put at body[128:132].
 *
 * AND THAT VALUE IS THE PROOF THE HASH IS RIGHT. EVAC$WORKLOAD appears in NO
 * corpus file in this tree -- not the derivation set, not the held-out set, not
 * the predicted set, not the prestudy names -- so the function reproducing
 * VAX1's own wire value for it is an independent, held-out confirmation off a
 * real VMS node's wire.
 * ========================================================================== */

#define EVAC_RESNAM    "EVAC$WORKLOAD"
#define EVAC_GROUP     1u            /* body[44:46] of the captured frame   */
#define EVAC_MODE      3u            /* body[46], PSL_C_USER                */
#define EVAC_VAX_PID   0x2020021eu   /* body[20:24], VAX1's requesting PID  */
#define EVAC_VAX_LKID  0x1a00021du   /* body[24:28], VAX1's own handle      */
#define EVAC_WIRE_HASH 0x00027e10u   /* body[128:132], VAX1's OWN value     */

/*
 * The lab's THREE-system bed. `mixed_up` brings this node and VAX1 up; VAX2 is
 * the third real VMS member, so the vector is three entries long exactly as it
 * was on the lab node. CSV-index order over the file's own CSIDs (VAX 2, VAX2
 * 3, OVMX 4) puts this node's entry LAST -- the same shape the lab's CSIDs
 * (1025 -> slot 1, 1026 -> slot 2, OVMXE -> slot 3) produced.
 */
static void labbed_up(uint8_t ovmx_wt, uint8_t vax1_wt, uint8_t vax2_wt)
{
	struct vms_csb *csb;

	mixed_up(ovmx_wt, vax1_wt);
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

/* A local process as the lab's own was: UIC group 1, running at user mode. */
static void lab_proc_init(struct vms_proc *p)
{
	proc_init(p);
	p->uic = (uint32_t)EVAC_GROUP << 16;
	p->current_mode = PSL_C_USER;
}

/* The captured frame, loaded and validated by the clean-room specimen loader
 * (sha256 over the assembled bytes; the capture is listed in the chain of
 * custody). Returns the 204-byte frame, or NULL with the test reddened. */
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

/* What the captured frame says, read by the SHIPPING parsers -- and the
 * held-out confirmation of the proven hash against it. */
static void the_captured_frame_and_the_held_out_value(void)
{
	const uint8_t *frame = evac_captured_frame();
	struct vms_dlm_enq_request e;
	struct vms_dlm_res_ident id;
	uint32_t hash = 0, computed = 0;
	uint8_t wireop = 0;

	printf("-- rd vms-025: the REAL captured frame, and the value the "
	       "proven function computes for it --\n");
	if (frame == NULL)
		return;

	ct_check(vms_dlm_enq_request_parse_body(body_of(frame),
						VMS_CM_BODY_LEN, &wireop, &e) ==
		 VMS_CODEC_OK, "the shipping ENQ parser reads VAX1's frame");
	ct_check_eq_u32(wireop, (uint32_t)VMS_DLM_WIREOP_ENQ,
			"  it is an op-0x01 ENQ, a new lock request");
	ct_check_eq_u32(e.mode, LCK_K_EXMODE,
			"  at EX -- COMPATIBLE with the NL this node held, so "
			"the faithful answer was a grant");
	ct_check_eq_u32(e.master_lkid, EVAC_VAX_PID,
			"  carrying VAX1's own requesting PID in the body[20:24] slot the master overwrites");
	ct_check(e.name_len == (uint8_t)strlen(EVAC_RESNAM) &&
		 memcmp(e.name, EVAC_RESNAM, e.name_len) == 0,
		 "  for the resource EVAC$WORKLOAD");

	ct_check(vms_dlm_res_ident_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					      &id) == VMS_CODEC_OK,
		 "the shipping identity parser reads the same frame");
	ct_check_eq_u32(id.group, EVAC_GROUP, "  UIC group 1 (body[44:46])");
	ct_check_eq_u32(id.mode, EVAC_MODE,
			"  access mode 3 = USER (body[46]) -- NOT the 0x03 "
			"'name marker' constant the codec used to write");
	ct_check(vms_dlm_dir_hash_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					     &hash) == VMS_CODEC_OK &&
		 hash == EVAC_WIRE_HASH,
		 "  and the value VAX1 ITSELF put at body[128:132]");

	/* *** THE HELD-OUT CONFIRMATION. *** */
	ct_check(vms_dlm_name_hash_proven(id.group, id.mode, id.name,
					  id.name_len, &computed) ==
		 VMS_DLM_HASH_OK, "the proven function answers for that "
		 "identity (inside its coverage)");
	ct_check_eq_u32(computed, EVAC_WIRE_HASH,
			"*** and computes EXACTLY the value the real VAX put on "
			"the wire, for a name held out of every corpus in this "
			"tree ***");
}

/*
 * ARM A. OVMX takes NL first and masters the resource; VAX1's CAPTURED op-0x01
 * ENQ(EX) arrives. The lab saw silence. This asserts the grant.
 */
static void lab_arm_a_the_captured_enq_is_served_as_master(void)
{
	const uint8_t *frame = evac_captured_frame();
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t dirnode = 0;
	uint32_t lkid = 0;

	printf("-- rd vms-025 ARM A: three members at LOCKDIRWT 0, OVMX "
	       "first, VAX1's CAPTURED frame --\n");
	if (frame == NULL)
		return;
	labbed_up(0u, 0u, 0u);
	lab_proc_init(&app);

	ct_check(g.cl.club.ldwv.valid && g.cl.club.ldwv.n == 3u,
		 "the lab's bed: ONE vector entry per system (p. 6-32's "
		 "all-zero rule), three systems");
	dirnode = dir_node_for(EVAC_RESNAM, EVAC_GROUP, EVAC_MODE);
	ct_check_eq_u32((uint32_t)dirnode, CSID_OVMX,
			"*** and the vector directs EVAC$WORKLOAD at THIS node "
			"-- which is where the real VAX really addressed its "
			"lookup, so OVMX's vector and the VAX's agree ***");

	/* The OVMX standby's NL, exactly as the lab's EVACWL took it. */
	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "the OVMX standby holds EVAC$WORKLOAD at NL");
	ct_check_eq_u32(g.n_sent, 0u,
			"  and nothing went on the wire: this node is the "
			"resource's directory and nobody else claimed it");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"  so this node MASTERS it (p. 6-31 'assumes mastery')");
	ct_check_eq_u32((uint32_t)vms_lock_dlm_name_mastered_here(
				EVAC_RESNAM, (uint32_t)strlen(EVAC_RESNAM),
				EVAC_GROUP, EVAC_MODE), 1u,
			"  and says so through the read the directory role "
			"uses -- the read that was TRUE on the lab node while "
			"the console said 'does not master'");

	/* *** VAX1'S CAPTURED FRAME *** */
	vax_served_as_master(frame, CSID_VAX, &res);
	ct_check_eq_u32(res.outcome, (uint32_t)VMS_DLM_MASTER_GRANTED,
			"*** VAX1's CAPTURED ENQ(EX) is GRANTED by this node as "
			"the resource's master (p. 6-31 outcome (a)) -- the lab "
			"got silence ***");
	ct_check_eq_u32(res.granted_mode, LCK_K_EXMODE,
			"  at EX, read off the LKB the engine stamped");
	ct_check_eq_u32(res.req_lkid, EVAC_VAX_LKID,
			"  with VAX1's own handle where the requester's handle "
			"goes");
	ct_check(res.master_lkid != 0u && res.master_lkid != EVAC_VAX_LKID,
		 "*** and a MASTER handle this executive minted -- not the "
		 "frame's body[24:28] plumbed through (RULE B) ***");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"  the resource still has exactly ONE master: this node");
	ct_check_eq_u32(rm.remote_holder_csid, CSID_VAX,
			"  with VAX1 recorded as the remote holder");
	mixed_down();
}

/*
 * ARM B. VAX1's CAPTURED frame arrives FIRST, so VAX1 masters the resource;
 * then an OVMX process locks the same identity. The lab saw a second holder.
 * This asserts the request is ROUTED at VAX1 and nothing is granted here.
 */
static void lab_arm_b_the_ovmx_enq_routes_at_the_vax(void)
{
	const uint8_t *frame = evac_captured_frame();
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0;

	printf("-- rd vms-025 ARM B: VAX1's CAPTURED frame first, then an OVMX "
	       "$ENQ on the same identity --\n");
	if (frame == NULL)
		return;
	labbed_up(0u, 0u, 0u);
	lab_proc_init(&app);

	/* VAX1's lookup arrives at this node, its directory: nothing masters
	 * the resource, so "you master it" -- and it is RECORDED. */
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "this node's directory answers VAX1 'you master it' and "
		 "RECORDS it as the master of that exact identity");

	/* *** THE $ENQ THAT MADE A SECOND HOLDER IN THE LAB. *** */
	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "the OVMX $ENQ is accepted (a proxy LKB exists)");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"*** and the resource's MASTER is VAX1 -- this node did "
			"NOT master it a second time ***");
	ct_check_eq_u32(rm.is_local_master, 0u,
			"  the engine claims no mastery");
	ct_check_eq_u32(g.n_sent, 1u, "exactly one frame left this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"*** addressed to VAX1, the master its own frame told "
			"us about ***");
	ct_check_eq_u32(sent_dir_hash(), EVAC_WIRE_HASH,
			"  carrying the value VAX1 ITSELF put on the wire for "
			"that name -- the LEARNED one, never a recomputed one");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP] |
			((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP + 1] << 8),
			EVAC_GROUP, "  and the identity that value is OF: "
			"group 1 (body[44:46])");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_MODE], EVAC_MODE,
			"  user mode (body[46])");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"*** and NOTHING above NL is granted here: the lab's "
			"second holder cannot happen ***");
	mixed_down();
}

/*
 * rd vms-f29f: THE MASTER GRANTS A REQUEST IT QUEUED, AND THE $ENQW WAKES.
 *
 * Lab run vms-5fa (2026-11-02, tests/lab/captures/vms-5fa-shared-volume-
 * 20261102): VAX1 masters EVAC$WORKLOAD; OVMXE's $ENQW EX reaches it (frame
 * 5781), is answered 0xfb QUEUED, and at 18:30:44.18 VAX1 grants it -- its SDA
 * shows "Master copy of lock 0100040F on system 00010003" granted EX, its
 * master handle 100006A4. The grant reached OVMXE as a cat-0x02 op-0x02. The
 * OVMX process never woke: it never printed, never wrote EVAC.DAT, and its EX
 * blocked every later requester.
 *
 * Driven here end to end through the shipping engine and FSM: a REAL $ENQW on
 * its own thread, the master's QUEUED answer built by the shipping builder from
 * the request OVMX really sent, and the deferred grant as a REAL VAX op-0x02
 * (fixtures/dlm-grant-notify-b, a real master's EX grant of a lock it had
 * queued) carrying the two handles of THIS exchange: VAX1's master handle from
 * the lab, and the requester handle OVMX put on its own request.
 */
#define F29F_VAX1_MASTER_LKID 0x100006a4u  /* VAX1's handle, vms-5fa capture */

struct f29f_waiter {
	struct vms_proc *proc;
	uint32_t status;
	uint32_t lkid;
	uint32_t lk_status;
	volatile int done;
};

static void *f29f_enqw_thread(void *arg)
{
	struct f29f_waiter *w = (struct f29f_waiter *)arg;
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = LCK_K_EXMODE;
	a.flags = LCK_M_SYNC;
	strscpy(a.resnam, EVAC_RESNAM, sizeof(a.resnam));
	vms_ioctl_enq(w->proc, (unsigned long)(void *)&a);
	w->status = a.status;
	w->lkid = a.lkid;
	w->lk_status = a.lk_status;
	w->done = 1;
	return NULL;
}

static int f29f_wait(volatile int *flag, volatile uint32_t *count,
		     uint32_t want, int ticks)
{
	int i;

	for (i = 0; i < ticks; i++) {
		if (flag != NULL && *flag)
			return 1;
		if (count != NULL && *count >= want)
			return 1;
		usleep(5000);
	}
	return 0;
}

static uint32_t f29f_rd32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void f29f_wr32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

/* The real VAX op-0x02, its two handle cells set to this exchange's. */
static int f29f_real_grant_notify(uint8_t *body, uint32_t master_lkid,
				  uint32_t req_lkid)
{
	static struct vms_fixture fx;
	char path[600];
	char err[VMS_FIXTURE_ERRLEN] = "";

	snprintf(path, sizeof(path), "%s/%s", OVMX_FIXTURE_DIR,
		 "dlm-grant-notify-b.spec");
	if (vms_fixture_load(path, OVMX_CLEANROOM_MANIFEST, &fx, err,
			     sizeof(err)) != 0 ||
	    fx.origin != VMS_FIXTURE_ORIGIN_CAPTURE ||
	    fx.wire_len != VMS_CM_FRAME_LEN) {
		printf("       reason: %s\n", err);
		return -1;
	}
	memcpy(body, fx.bytes + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	f29f_wr32(body + VMS_OFB_DLM_MASTER_LKID, master_lkid);
	f29f_wr32(body + VMS_OFB_DLM_REQ_LKID, req_lkid);
	return 0;
}

static void f29f_a_queued_enqw_wakes_on_the_masters_grant(void)
{
	const uint8_t *frame = evac_captured_frame();
	uint8_t ans[VMS_CM_FRAME_LEN];
	uint8_t gn[VMS_CM_BODY_LEN];
	struct vms_resmaster_args rm;
	struct f29f_waiter w;
	struct vms_proc app;
	pthread_t th;
	uint32_t written = 0, ovmx_lkid;
	int woke;

	printf("-- rd vms-f29f: VAX1 masters EVAC$WORKLOAD, QUEUES an OVMX "
	       "$ENQW EX, then GRANTS it with an op-0x02 --\n");
	if (frame == NULL)
		return;
	labbed_up(0u, 1u, 1u);   /* the lab's LOCKDIRWTs: VAXes 1, OVMX 0 */
	lab_proc_init(&app);

	ct_check_eq_u32((uint32_t)dir_node_for(EVAC_RESNAM, EVAC_GROUP,
					       EVAC_MODE), CSID_VAX,
			"the vector directs EVAC$WORKLOAD at VAX1, where the "
			"lab's frame 5781 went");

	memset(&w, 0, sizeof(w));
	w.proc = &app;
	ct_check(pthread_create(&th, NULL, f29f_enqw_thread, &w) == 0,
		 "the OVMX process's $ENQW EX runs on its own thread");
	ct_check(f29f_wait(NULL, &g.n_sent, 1u, 400),
		 "its request leaves this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"  addressed to VAX1, the master");
	ovmx_lkid = f29f_rd32(g.last + VMS_OFB_DLM_REQ_LKID);
	ct_check(ovmx_lkid != 0u,
		 "  carrying the requester handle this executive minted");

	/* VAX1's 0xfb: queued behind its own EX holder. */
	memset(ans, 0, sizeof(ans));
	ct_check(vms_dlm_enq_response_build_queued(g.last, VMS_CM_BODY_LEN,
						   F29F_VAX1_MASTER_LKID,
						   ovmx_lkid, ans,
						   (uint32_t)sizeof(ans),
						   &written) == VMS_CODEC_OK,
		 "VAX1's QUEUED answer to that request builds");
	ct_check(dlm_req_fsm_reply_body(&g.fsm, (vms_csid_t)CSID_VAX, 0u,
					body_of(ans), VMS_CM_BODY_LEN) ==
		 DLM_REQ_OK, "  and the FSM takes it");
	ct_check(!f29f_wait(&w.done, NULL, 0u, 120),
		 "QUEUED: the $ENQW is still waiting, past the deadlock "
		 "re-scan interval -- not granted, and not SS$_DEADLOCK");

	/* *** VAX1 GRANTS IT: the real op-0x02. *** */
	ct_check(f29f_real_grant_notify(gn, F29F_VAX1_MASTER_LKID,
					ovmx_lkid) == 0,
		 "the real VAX op-0x02 specimen loads (an EX deferred grant)");
	/* negctl: dlm-f29f-deferred-grant-not-read */
	ct_check(dlm_req_fsm_grant_notify_body(&g.fsm, (vms_csid_t)CSID_VAX,
					       gn, VMS_CM_BODY_LEN) ==
		 DLM_REQ_OK,
		 "*** the master's op-0x02 is TAKEN as the grant of a request "
		 "of ours ***");
	woke = f29f_wait(&w.done, NULL, 0u, 400);
	if (!woke)
		(void)vms_lock_dlm_proxy_fail(ovmx_lkid, SS__UNSUPPORTED);
	ct_check(woke && w.status == SS__NORMAL,
		 "*** and the $ENQW WAKES with SS$_NORMAL -- the lab's process "
		 "slept for ever here ***");
	pthread_join(th, NULL);
	ct_check_eq_u32(w.lkid, ovmx_lkid, "  with the handle it asked under");
	ct_check_eq_u32(lki_granted_mode(&app, ovmx_lkid), LCK_K_EXMODE,
			"  $GETLKI: held at EX, the mode VAX1 granted");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check(rm.master_csid == CSID_VAX && rm.is_local_master == 0u,
		 "  and the resource still has ONE master: VAX1");
	ct_check_eq_u32(g.fsm.grant_notifies_rx, 1u,
			"  one deferred grant read, counted");

	/* The process goes on and RELEASES -- at VAX1, by VAX1's handle. */
	g.n_sent = 0u;
	ct_check(do_deq(&app, ovmx_lkid) == SS__NORMAL,
		 "the process releases its EX");
	ct_check(g.n_sent == 1u && g.last_dst == CSID_VAX &&
		 g.last[VMS_OFB_DLM_OP] == (uint8_t)VMS_DLM_WIREOP_DEQ &&
		 f29f_rd32(g.last + VMS_OFB_DLM_MASTER_LKID) ==
		 F29F_VAX1_MASTER_LKID &&
		 f29f_rd32(g.last + VMS_OFB_DLM_REQ_LKID) == ovmx_lkid,
		 "*** the op-0x03 goes to VAX1 naming VAX1's OWN handle from "
		 "the op-0x02 -- the release every later requester waited "
		 "for ***");
	mixed_down();
}

/* Its refusals: nothing is granted that the frame does not really grant. */
static void f29f_an_op02_grants_nothing_it_does_not_name(void)
{
	const uint8_t *frame = evac_captured_frame();
	uint8_t gn[VMS_CM_BODY_LEN];
	struct vms_proc app;
	uint32_t lkid = 0, ovmx_lkid;

	printf("-- rd vms-f29f: an op-0x02 for no request of ours, or one the "
	       "codec refuses, grants nothing --\n");
	if (frame == NULL)
		return;
	labbed_up(0u, 1u, 1u);   /* the lab's LOCKDIRWTs: VAXes 1, OVMX 0 */
	lab_proc_init(&app);
	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_EXMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u, "an async OVMX $ENQ EX at VAX1");
	ovmx_lkid = f29f_rd32(g.last + VMS_OFB_DLM_REQ_LKID);

	ct_check(f29f_real_grant_notify(gn, F29F_VAX1_MASTER_LKID,
					ovmx_lkid ^ 0x00ff0000u) == 0,
		 "the real op-0x02, naming a handle this node never minted");
	ct_check(dlm_req_fsm_grant_notify_body(&g.fsm, (vms_csid_t)CSID_VAX,
					       gn, VMS_CM_BODY_LEN) ==
		 DLM_REQ_E_NOLOCK, "  is refused: no request of ours");
	ct_check(f29f_real_grant_notify(gn, F29F_VAX1_MASTER_LKID,
					ovmx_lkid) == 0,
		 "the real op-0x02 for OUR request ...");
	gn[VMS_OFB_DLM_GRANT_NOTIFY_MODE] = (uint8_t)VMS_LCK_NL;
	ct_check(dlm_req_fsm_grant_notify_body(&g.fsm, (vms_csid_t)CSID_VAX,
					       gn, VMS_CM_BODY_LEN) ==
		 DLM_REQ_E_CODEC,
		 "  ... granting NL, which no real one does (387 of 387): "
		 "refused by the codec");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"  and the lock is still NOT held");
	mixed_down();
}

/*
 * rd vms-f29f: THE PEER-RECEIVE FUZZ for the deferred-grant entry. 20,000
 * mutations of the real VAX op-0x02 -- random bytes anywhere, random lengths
 * (short ones included), the requester handle sometimes ours and sometimes not
 * -- thrown at the shipping FSM with a real request outstanding at "VAX1".
 * Nothing may crash, NOTHING may be sent (nobody answers an op-0x02), and the
 * lock may be granted only by a frame the codec admits naming OUR handle.
 */
static uint32_t f29f_rng = 0x5fa0f29fu;

static uint32_t f29f_rand(void)
{
	f29f_rng ^= f29f_rng << 13;
	f29f_rng ^= f29f_rng >> 17;
	f29f_rng ^= f29f_rng << 5;
	return f29f_rng;
}

static void f29f_fuzz_the_deferred_grant_entry(void)
{
	const uint8_t *frame = evac_captured_frame();
	uint8_t base[VMS_CM_BODY_LEN], gn[VMS_CM_BODY_LEN];
	struct vms_dlm_grant_notify parsed;
	struct vms_proc app;
	uint32_t lkid = 0, ovmx_lkid, i, k, len, sent0;
	uint32_t admitted_ours = 0, bad_grant = 0;
	enum dlm_req_status st;

	printf("-- rd vms-f29f: fuzz the deferred-grant receive (20,000 "
	       "mutated real op-0x02 bodies) --\n");
	if (frame == NULL)
		return;
	labbed_up(0u, 1u, 1u);   /* the lab's LOCKDIRWTs: VAXes 1, OVMX 0 */
	lab_proc_init(&app);
	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_EXMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u, "an async OVMX $ENQ EX at VAX1");
	ovmx_lkid = f29f_rd32(g.last + VMS_OFB_DLM_REQ_LKID);
	ct_check(f29f_real_grant_notify(base, F29F_VAX1_MASTER_LKID,
					ovmx_lkid) == 0,
		 "the real op-0x02 specimen loads");
	sent0 = g.n_sent;

	for (i = 0u; i < 20000u; i++) {
		memcpy(gn, base, sizeof(gn));
		for (k = f29f_rand() % 6u; k > 0u; k--)
			gn[f29f_rand() % VMS_CM_BODY_LEN] = (uint8_t)f29f_rand();
		if ((f29f_rand() & 3u) == 0u)
			f29f_wr32(gn + VMS_OFB_DLM_REQ_LKID, f29f_rand());
		len = (f29f_rand() & 7u) == 0u ?
		      f29f_rand() % (VMS_CM_BODY_LEN + 1u) : VMS_CM_BODY_LEN;
		/* Keep the lock pending: a mutation the codec admits for OUR
		 * handle would grant it, and that is counted, not fuzzed past. */
		if (vms_dlm_grant_notify_parse_body(gn, len, &parsed) ==
		    VMS_CODEC_OK && parsed.req_lkid == ovmx_lkid) {
			admitted_ours++;
			continue;
		}
		st = dlm_req_fsm_grant_notify_body(&g.fsm, (vms_csid_t)CSID_VAX,
						   gn, len);
		if (st == DLM_REQ_OK)
			bad_grant++;
	}
	ct_check_eq_u32(bad_grant, 0u,
			"no body the codec refuses, or naming another handle, "
			"is ever taken as a grant");
	ct_check_eq_u32(g.n_sent - sent0, 0u,
			"*** and NOTHING was sent: an op-0x02 is never "
			"answered ***");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_NLMODE,
			"the lock is still pending after 20,000 hostile frames");
	ct_check(admitted_ours > 0u,
		 "  (the mutator did reach admissible frames for our handle)");
	mixed_down();
}

/*
 * rd vms-7649: OVMX's FILES-11 VOLUME LOCK REACHES THE VAX THAT HOLDS IT.
 *
 * Lab run vms-5fa (2026-11-02, tests/lab/captures/vms-5fa-shared-volume-
 * 20261102): VAX1 MOUNTed EVAC5FA, then OVMXE MOUNTed the same volume through
 * VAX1's MSCP server. VAX1's SDA on "F11B$vEVAC5FA     ": DIRENTR, CSID 0,
 * "Kernel mode", "System", Lock Count 1 -- OVMX's lock never reached it. The
 * one frame OVMXE sent for that name (lock-frames.txt, frame 4965) went to
 * VAX2 and named UIC group 1, access mode 3, hash 0xbe1fb09f: the MOUNTing
 * process's identity, a DIFFERENT resource, whose directory is VAX2. VAX2
 * answered 0xf9 and OVMX mastered a second copy.
 *
 * The bed is the lab's: three members, the VAXes at LOCKDIRWT 1, OVMX at 0.
 * The MOUNT path's own call (vms_lock_acp_vol_standing) runs from a process
 * at UIC group 1, user mode -- the lab's SYSTEM -- on its own thread.
 */
#define E7649_NAME        "F11B$vEVAC5FA     "  /* 18 bytes, as VAX1's SDA */
#define E7649_LAB_GROUP   1u            /* frame 4965 body[44:46]          */
#define E7649_LAB_MODE    3u            /* frame 4965 body[46]             */
#define E7649_LAB_HASH    0xbe1fb09fu   /* frame 4965 body[128:132]        */

struct e7649_standing {
	struct vms_proc *proc;
	uint32_t status;
	uint32_t lkid;
	int remote;
	volatile int done;
};

static void *e7649_mount_thread(void *arg)
{
	struct e7649_standing *r = (struct e7649_standing *)arg;

	r->status = vms_lock_acp_vol_standing(r->proc, E7649_NAME, &r->lkid,
					      &r->remote);
	r->done = 1;
	return NULL;
}

static void *e7649_sync_thread(void *arg)
{
	struct e7649_standing *r = (struct e7649_standing *)arg;

	r->status = vms_lock_acp_vol_ex(r->proc, "F11B$sEVAC5FA     ",
					&r->lkid);
	r->done = 1;
	return NULL;
}

static void e7649_the_volume_lock_goes_to_its_directory(void)
{
	struct e7649_standing r;
	struct vms_resmaster_args rm;
	struct vms_proc mounter;
	pthread_t th;
	uint32_t want_hash = 0, lab_hash = 0;
	int i;

	printf("-- rd vms-7649: OVMX's F11B$v<label> lock is the XQP's "
	       "system-wide, kernel-mode resource, and goes to VAX1 --\n");
	labbed_up(0u, 1u, 1u);
	lab_proc_init(&mounter);   /* UIC group 1, user mode: the lab's SYSTEM */

	/* What the identities resolve to, through the real vector. */
	ct_check(vms_dlm_name_hash_proven((uint16_t)E7649_LAB_GROUP,
					  (uint8_t)E7649_LAB_MODE,
					  (const uint8_t *)E7649_NAME,
					  (uint32_t)strlen(E7649_NAME),
					  &lab_hash) == VMS_DLM_HASH_OK &&
		 lab_hash == E7649_LAB_HASH,
		 "the proven hash of the lab frame's identity (group 1, user "
		 "mode) is the value OVMXE put on the wire");
	ct_check_eq_u32((uint32_t)dir_node_for(E7649_NAME,
					       (uint16_t)E7649_LAB_GROUP,
					       (uint8_t)E7649_LAB_MODE),
			CSID_VAX2,
			"  and that identity's directory is VAX2 -- where the "
			"lab's frame went");
	ct_check_eq_u32((uint32_t)dir_node_for(E7649_NAME, 0u, 0u), CSID_VAX,
			"the XQP's identity (system-wide, kernel) is directed "
			"at VAX1 -- which holds the DIRENTR in the lab");
	(void)vms_dlm_name_hash_proven(0u, 0u, (const uint8_t *)E7649_NAME,
				       (uint32_t)strlen(E7649_NAME),
				       &want_hash);

	memset(&r, 0, sizeof(r));
	r.proc = &mounter;
	ct_check(pthread_create(&th, NULL, e7649_mount_thread, &r) == 0,
		 "the MOUNT's standing NL lock runs on its own thread");
	for (i = 0; i < 400 && g.n_sent == 0u; i++)
		usleep(5000);
	ct_check_eq_u32(g.n_sent, 1u, "one frame leaves this node");
	/* negctl: dlm-7649-f11b-lock-takes-the-mounters-identity */
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"*** addressed to VAX1, the resource's directory -- not "
			"VAX2, where the lab's went ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP] |
			((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP + 1] << 8), 0u,
			"*** naming UIC group 0, system-wide -- not the "
			"mounter's group 1 ***");
	ct_check_eq_u32((uint32_t)g.last[VMS_OFB_DLM_RES_MODE], 0u,
			"*** at access mode 0, kernel -- not the mounter's "
			"user mode ***");
	ct_check_eq_u32(sent_dir_hash(), want_hash,
			"  with the proven hash of THAT identity");

	/* VAX1, the directory AND the master, grants the NL. */
	ct_check(vax_grants(0u, 0x3100069au, (uint8_t)LCK_K_NLMODE) == 0,
		 "VAX1 grants it");
	for (i = 0; i < 400 && !r.done; i++)
		usleep(5000);
	if (!r.done) {
		ct_check(0, "the MOUNT's $ENQW woke on VAX1's grant");
		pthread_detach(th);
		return;
	}
	pthread_join(th, NULL);
	ct_check(r.status == SS__NORMAL && r.lkid != 0u && r.remote == 1,
		 "the standing lock is held as a PROXY: VAX1 masters it");
	read_resmaster(E7649_NAME, &rm);
	ct_check(rm.master_csid == CSID_VAX && rm.is_local_master == 0u,
		 "*** ONE master for the volume lock: VAX1 -- not a second "
		 "copy on this node ***");
	(void)vms_lock_acp_vol_release(&mounter, r.lkid);

	/* ...AND THE XQP's PER-WRITE SYNC LOCK, through the same door. */
	g.n_sent = 0u;
	memset(&r, 0, sizeof(r));
	r.proc = &mounter;
	ct_check(pthread_create(&th, NULL, e7649_sync_thread, &r) == 0,
		 "an XQP write's EX F11B$s lock runs on its own thread");
	for (i = 0; i < 400 && g.n_sent == 0u; i++)
		usleep(5000);
	ct_check(g.n_sent == 1u &&
		 ((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP] |
		  ((uint32_t)g.last[VMS_OFB_DLM_RES_GROUP + 1] << 8)) == 0u &&
		 g.last[VMS_OFB_DLM_RES_MODE] == 0u,
		 "*** the F11B$s request names (group 0, kernel) too ***");
	(void)vax_grants(0u, 0x3100069bu, (uint8_t)LCK_K_EXMODE);
	for (i = 0; i < 400 && !r.done; i++)
		usleep(5000);
	if (!r.done) {
		ct_check(0, "the XQP's $ENQW woke on VAX1's grant");
		pthread_detach(th);
		return;
	}
	pthread_join(th, NULL);
	(void)vms_lock_acp_vol_release(&mounter, r.lkid);
	mixed_down();
}

/*
 * NEGATIVE CONTROL for §1, and the one that keeps a real VAX safe: an identity
 * OUTSIDE the proven coverage puts NO FRAME on the wire -- so a value nobody
 * has watched VMS produce can never reach a VAX's directory.
 *
 * AND IT DOES NOT REFUSE THE CALLER (rd vms-b5b0, the PR #1578 lab
 * regression). It did, and on a real booted node that refusal reached the ACP:
 * 74 file operations failed with SS$_UNSUPPORTED and STARTUP.COM died on
 * `%RMS-E-FNF ... SYS$STARTUP:VMS$VMS.DAT`. The resource is mastered LOCALLY
 * instead -- Baron's option A on rd vms-dc2, which his ruling prefers to
 * refusing in as many words -- counted and said once.
 */
static void an_unproven_identity_sends_nothing_at_the_vax(void)
{
	struct vms_proc app;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st, before;

	printf("-- negative: an identity outside the proven coverage sends "
	       "NOTHING --\n");
	mixed_up(0u, 0u);
	proc_init(&app);
	app.current_mode = PSL_C_SUPER;   /* mode 2: never seen on a wire */
	before = vms_lock_dlm_dir_hash_uncovered();

	st = do_enq(&app, "EVAC$SUPER", LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u,
		 "the $ENQ is GRANTED -- the ACP cannot be refused");
	ct_check_eq_u32(g.n_sent, 0u,
			"*** and NOT ONE FRAME went at the VAX ***");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_uncovered(), before + 1u,
			"*** the local-only mastery is COUNTED, not silent ***");
	read_resmaster("EVAC$SUPER", &rm);
	ct_check_eq_u32(rm.dir_csid, 0u,
			"  and NO directory is asserted for it (INV-6)");
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

/*
 * THE §2 CONTROL THAT RETIRED (rd vms-b5b0): "outside the interim
 * configuration the directory entry is NOT acted on, and the previous
 * behaviour -- master it locally -- stands". That WAS the safety argument
 * while the vector could not be consulted per resource; it is now the hole
 * (two masters for one resource), and §1 above asserts the replacement: at the
 * default LOCKDIRWT the vector is consulted per resource and the request goes
 * where it says. The control that still matters moved with it -- an identity
 * outside the proven coverage sends nothing at all
 * (an_unproven_identity_sends_nothing_at_the_vax).
 */

/*
 * A DIRECTORY ENTRY WITH NO STORED VALUE IS NOW ROUTED, NOT REFUSED (rd
 * vms-b5b0, closing #1568's own honest gap).
 *
 * An op-0x0d registration records a master and carries no value the learner
 * reads, so this executive held NONE for the name and the $ENQ was REFUSED
 * (SS$_UNSUPPORTED) -- honest, and a lock OVMX could not take on a resource the
 * VAX had registered. The value for that identity is now computable and proven,
 * so the request goes to the registered master carrying the value THIS
 * EXECUTIVE computed for it -- which is the same value the VAX itself would
 * compute, that being what "one function cluster-wide" means.
 */
static void an_entry_without_a_stored_value_is_routed(void)
{
	struct vms_dlm_res_ident id;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0, st, expect = 0;
	const char *N = "REGONLY1";

	printf("-- an entry from a REGISTRATION (no stored value): ROUTED --\n");
	mixed_up(1u, 0u);
	proc_init(&app);

	memset(&id, 0, sizeof(id));
	id.hash = 0u;                 /* the entry carries none */
	id.group = OVMX_RES_GROUP;
	id.mode = OVMX_RES_MODE;
	id.name_len = (uint8_t)strlen(N);
	memcpy(id.name, N, id.name_len);
	ct_check(vms_dlm_dir_register(&g.dir, &id, CSID_VAX) == 0,
		 "the VAX registers itself as the master of " "REGONLY1");

	ct_check(vms_dlm_name_hash(OVMX_RES_GROUP, OVMX_RES_MODE,
				   (const uint8_t *)N, (uint32_t)strlen(N),
				   &expect) == VMS_DLM_HASH_OK,
		 "the function answers for that identity");

	st = do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u,
		 "*** the $ENQ is ACCEPTED: the VAX masters it and this node "
		 "can now address a frame at it ***");
	ct_check_eq_u32(g.n_sent, 1u, "one frame left this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"  addressed to the master the registration named");
	ct_check_eq_u32(sent_dir_hash(), expect,
			"*** carrying the value this executive COMPUTED for the "
			"identity -- never a zero where the hash goes ***");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.is_local_master, 0u,
			"  and it did NOT become a second master");
	mixed_down();
}

/*
 * AN UNCOVERED IDENTITY IS STILL PROTECTED FROM THE TWO-MASTER HOLE WHERE THE
 * VECTOR CAN ANSWER WITHOUT A VALUE (rd vms-b5b0).
 *
 * This is the payoff of `dir_all_ours`, and the reason the uncovered-identity
 * fallback is not simply "master it blind". With every vector entry ours the
 * vector cannot name anyone else whatever the value would have been (p. 6-32),
 * so an identity outside the hash's proven coverage is STILL resolved to this
 * node correctly -- and so still goes through this node's own directory table.
 * If a real VAX locked that exact resource first, its own lookup taught us the
 * value, and the request is addressed AT THE VAX carrying the VAX's OWN value:
 * an uncovered identity the cluster has named is routable after all.
 */
static void an_uncovered_identity_the_vax_named_is_still_routed(void)
{
	/* 23 bytes: outside the proven coverage (the driven run pre-registered
	 * it and the VAX never put it on the wire). */
	const char *N = "EVAC$TWENTYTHREEBYTES12";
	const uint32_t VAX_HASH = 0x7c3d0244u;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_resmaster_args rm;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, st, before;

	printf("-- an UNCOVERED identity the VAX named first: still routed "
	       "THERE --\n");
	mixed_up(1u, 0u);                 /* the vector directs everything here */
	proc_init(&app);
	before = vms_lock_dlm_dir_hash_uncovered();

	ct_check((uint32_t)strlen(N) == 23u,
		 "the name is 23 bytes long");
	ct_check(vms_dlm_name_hash_coverage(OVMX_RES_GROUP, OVMX_RES_MODE,
					    23u) == VMS_DLM_HASH_E_COVER,
		 "...which is outside the PROVEN coverage");
	ct_check(vms_ldwv_directs_everything_here(&g.cl.club.ldwv) == 1,
		 "and this vector directs every resource HERE");

	/* The VAX locks it first; its lookup is answered and RECORDED, and its
	 * own value for the resource is learned off that frame. */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, VAX_HASH) == 0,
		 "the VAX's op-0x01 for it is built");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "this node's directory answers 'you master it' and records it");

	/* *** THE $ENQ *** -- no provable value of our own, and it does not
	 * matter: the vector cannot name anyone but us, so the directory table
	 * is consulted and it names the VAX. */
	st = do_enq(&app, N, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u, "the $ENQ is accepted");
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"*** and the MASTER is the VAX -- an uncovered identity "
			"is NOT mastered a second time here ***");
	ct_check_eq_u32(g.n_sent, 1u, "one frame left this node");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX, "  addressed to the VAX");
	ct_check_eq_u32(sent_dir_hash(), VAX_HASH,
			"*** carrying the value THE VAX ITSELF put on the wire "
			"for it -- the one value this executive holds ***");
	ct_check_eq_u32(vms_lock_dlm_dir_hash_uncovered(), before,
			"*** and NO uncovered-identity fallback was taken: the "
			"vector answered without a value ***");
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
	ct_check(vms_lock_dlm_name_mastered_here(N, 0u, OVMX_RES_GROUP,
						 OVMX_RES_MODE) == 1,
		 "  and says so through the read the directory role uses");
	{
		struct vms_dlm_res_ident selfid;

		memset(&selfid, 0, sizeof(selfid));
		selfid.group = OVMX_RES_GROUP;
		selfid.mode = OVMX_RES_MODE;
		selfid.name_len = (uint8_t)strlen(N);
		memcpy(selfid.name, N, selfid.name_len);
		ct_check(vms_dlm_dir_lookup_ident(&g.dir, &selfid, &master,
						  NULL, NULL) ==
			 VMS_DLM_DIR_IDENT_MASTER && master == CSID_OVMX,
			 "*** and the mastery is RECORDED in this node's own "
			 "directory -- which is what stops the next asker "
			 "being told to master it ***");
	}

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
	vax_served_as_master(frame, CSID_VAX, &res);
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

/*
 * ONE FRAME OF THE ci.6 LAB FIRE, read out of the COMMITTED hex transcript
 * (tests/lab/captures/vms-ci6-evac-20261103/ci6evac-031902/ci6-frames.txt --
 * "Bytes from the Ethernet payload (after the 14-byte header)"), so no byte of
 * it is typed into this file. The 14-byte Ethernet header is left zero: the
 * codec reads from VMS_OFF_SYSAP_BODY on. Returns 0, or -1 with the test red.
 */
#define CI6_FRAMES "../../../lab/captures/vms-ci6-evac-20261103/" \
		   "ci6evac-031902/ci6-frames.txt"
static int ci6_frame(unsigned want, uint8_t *frame)
{
	char path[700], line[256];
	FILE *fp;
	int in = 0, got = 0;

	memset(frame, 0, VMS_CM_FRAME_LEN);
	snprintf(path, sizeof(path), "%s/%s", OVMX_FIXTURE_DIR, CI6_FRAMES);
	fp = fopen(path, "r");
	if (fp == NULL) {
		ct_check(0, "the committed ci.6 frame transcript opens");
		return -1;
	}
	while (fgets(line, sizeof(line), fp) != NULL) {
		unsigned n = 0, off = 0, v = 0;
		int used = 0;
		const char *c;

		if (sscanf(line, "frame %u", &n) == 1) {
			if (in)
				break;
			in = (n == want);
			continue;
		}
		if (!in || sscanf(line, " %x%n", &off, &used) != 1)
			continue;
		for (c = line + used; sscanf(c, " %2x%n", &v, &used) == 1;
		     c += used, off++) {
			if (14u + off < VMS_CM_FRAME_LEN)
				frame[14u + off] = (uint8_t)v;
			got++;
		}
	}
	fclose(fp);
	ct_check(got + 14 == (int)VMS_CM_FRAME_LEN,
		 "the transcript frame is the 204-byte DLM class, read whole");
	return got + 14 == (int)VMS_CM_FRAME_LEN ? 0 : -1;
}

/*
 * THE NOQUEUE CASE, ON THE REAL WIRE (rd vms-1e58 C4): SS$_NOTQUEUED is a real
 * refusal of a real conflict, and the request says NOQUEUE IN ITS OWN BYTES.
 *
 * The lab: OVMXE mastered EVAC$WORKLOAD and held it EX; VAX2's EVPROBE asked
 * for EX with LCK$M_NOQUEUE (frame 7310, body[28:30] = 04 00) and OVMXE
 * answered QUEUED (frame 7312), so VAX2's $ENQW completed SS$_NORMAL inside
 * OVMXE's hold. The same request WITHOUT NOQUEUE -- VAX1's EVACWL, frame 5212,
 * body[28:30] = 00 00 -- is the contrast: that one must still queue.
 */
static void a_vax_noqueue_request_is_denied(void)
{
	uint8_t probe[VMS_CM_FRAME_LEN], wl[VMS_CM_FRAME_LEN];
	uint8_t ans[VMS_CM_FRAME_LEN];
	struct vms_dlm_enq_request e;
	struct vms_dlm_enq_response r;
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	struct vms_proc app;
	uint32_t lkid = 0, written = 0;
	uint8_t wireop = 0;
	const uint8_t *ab = ans + VMS_OFF_SYSAP_BODY;
	const uint8_t *pb = probe + VMS_OFF_SYSAP_BODY;

	printf("-- rd vms-1e58 C4: VAX2's CAPTURED EX/NOQUEUE against this "
	       "node's EX is DENIED --\n");
	if (ci6_frame(7310u, probe) != 0 || ci6_frame(5212u, wl) != 0)
		return;
	labbed_up(0u, 0u, 0u);
	lab_proc_init(&app);

	ct_check(vms_dlm_enq_request_parse_body(pb, VMS_CM_BODY_LEN, &wireop,
						&e) == VMS_CODEC_OK &&
		 wireop == VMS_DLM_WIREOP_ENQ,
		 "frame 7310 parses as an op-0x01 ENQ");
	ct_check((e.req_flags & VMS_DLM_REQ_FLAG_NOQUEUE) != 0u,
		 "*** the parser READS body[28:30]'s NOQUEUE bit off VAX2's "
		 "own frame ***");
	/* negctl: dlm-1e58-noqueue-not-read */
	ct_check_eq_u32(vms_dlm_master_req_flags(&e, wireop), LCK_M_NOQUEUE,
			"*** and the arm's translation hands the master "
			"LCK_M_NOQUEUE ***");

	ct_check(do_enq(&app, EVAC_RESNAM, LCK_K_EXMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "this node holds EVAC$WORKLOAD EX (the lab's OVMXE)");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u, "  and masters it");

	vax_served_as_master(probe, CSID_VAX2, &res);
	ct_check_eq_u32(res.outcome, (uint32_t)VMS_DLM_MASTER_DENIED,
			"*** VAX2's captured EX/NOQUEUE is DENIED -- the lab "
			"got QUEUED ***");
	ct_check_eq_u32(res.master_lkid, 0u,
			"  and no lock id is asserted, because no lock exists");
	read_resmaster(EVAC_RESNAM, &rm);
	ct_check_eq_u32(rm.n_granted, 1u,
			"  the resource still has ONE granted lock: ours");
	ct_check_eq_u32(rm.remote_holder_csid, 0u,
			"  and nothing is held for VAX2");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_EXMODE,
			"  this node still holds EX");

	/* THE ANSWER the arm sends for it: the grounded deny, read back by the
	 * shipping requester-side parser exactly as a requester would. */
	ct_check(vms_dlm_enq_response_build_notqueued(pb, VMS_CM_BODY_LEN, ans,
						      sizeof(ans), &written) ==
		 VMS_CODEC_OK && written == VMS_CM_FRAME_LEN,
		 "the deny is built from VAX2's own request");
	/* negctl: dlm-1e58-deny-record-not-written */
	ct_check(ab[34] == VMS_DLM_REPLY_NOTQUEUED && ab[32] == 0xb8u &&
		 ab[33] == 0x09u && ab[35] == 0u,
		 "*** body[32:36] = b8 09 fc 00, the record every real deny "
		 "carries (0x09b8 = SS$_NOTQUEUED) -- not 00 00 fb 00 ***");
	ct_check(ab[8] == 0x82u && ab[9] == VMS_DLM_WIREOP_ENQ,
		 "  a cat-0x82 answer to the op-0x01");
	ct_check(memcmp(ab + 4, pb + 4, 4) == 0 &&
		 memcmp(ab + 20, pb + 20, 8) == 0,
		 "  the transaction token, the PID placeholder and VAX2's own "
		 "handle ride back unchanged");
	ct_check(ab[28] == 0u && ab[29] == 0u && ab[30] == 0u && ab[31] == 0u,
		 "  body[28:32] cleared, the requested mode included");
	ct_check(memcmp(ab + 44, pb + 44, 88) == 0,
		 "  and the identity and the WHOLE name echo back -- no "
		 "'EVAC 00 00 ORKLOAD' (that was the QUEUED shape's body[52:54])");
	ct_check(vms_dlm_enq_response_parse_body(ab, VMS_CM_BODY_LEN, &r) ==
		 VMS_CODEC_OK && r.outcome == VMS_DLM_ENQ_DENIED,
		 "*** and this node's own requester reads it as DENIED ***");

	/* THE CONTRAST: the same EX without NOQUEUE still queues. */
	vax_served_as_master(wl, CSID_VAX, &res);
	ct_check_eq_u32(res.outcome, (uint32_t)VMS_DLM_MASTER_QUEUED,
			"*** VAX1's captured EX WITHOUT NOQUEUE (frame 5212) is "
			"QUEUED, as before ***");
	mixed_down();
}

/* A SUB-LOCK's deny names the master's own S0 address at body[40:44]
 * (55 of 55 real ones): no answer is composed for it. */
static void a_notqueued_sublock_answer_is_refused(void)
{
	uint8_t probe[VMS_CM_FRAME_LEN], ans[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	printf("-- rd vms-1e58 C4: no deny is composed for a sub-lock --\n");
	if (ci6_frame(7310u, probe) != 0)
		return;
	probe[VMS_OFF_SYSAP_BODY + 40] = 0x01u;   /* names a parent master */
	ct_check(vms_dlm_enq_response_build_notqueued(probe + VMS_OFF_SYSAP_BODY,
						      VMS_CM_BODY_LEN, ans,
						      sizeof(ans), &written) ==
		 VMS_CODEC_E_CLASS,
		 "a sub-lock request's deny is refused, not invented");
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
	vax_served_as_master(f1, CSID_VAX, &held);
	ct_check(held.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 held.granted_mode == LCK_K_EXMODE,
		 "the VAX is GRANTED EX by this master (NL is compatible)");

	/*
	 * A SECOND VMS system takes NL (compatible), and then CONVERTS it to
	 * EX. The convert NAMES ITS LOCK BY OUR HANDLE -- which is what a real
	 * op-0x07 does, and since rd vms-b5b0 it is also where the engine takes
	 * the resource's identity from: a real VAX leaves the identity span on
	 * an op-0x07 STALE (docs/design-dlm-name-hash.md §4a), so the LKB the
	 * handle names is the only honest source.
	 */
	{
		struct vms_dlm_master_result nl;

		ct_check(vax_build_enq(f2, VMS_DLM_WIREOP_ENQ, VAX_LKID_2,
				       (uint8_t)LCK_K_NLMODE, N,
				       0x22334455u) == 0,
			 "the second system's op-0x01 NL request is built");
		vax_served_as_master(f2, CSID_VAX2, &nl);
		ct_check(nl.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
			 nl.master_lkid != 0u,
			 "it is GRANTED NL, with a real handle of ours");
		ct_check(vax_build_convert(f2, VAX_LKID_2, nl.master_lkid,
					   (uint8_t)LCK_K_EXMODE) == 0,
			 "and its op-0x07 convert to EX names that handle");
	}
	vax_served_as_master(f2, CSID_VAX2, &queued);
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
	vax_served_as_master(frame, CSID_VAX, &held);
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


/* ==========================================================================
 * 9. rd vms-33a -- THIS NODE HOLDS A LOCK IN A VAX-MASTERED TREE, AND THE VAX
 *    MOVES THE TREE TO A THIRD NODE.
 *
 * End to end over everything that links on the host: the OVMX $ENQ goes to the
 * VAX master through the shipping requester FSM and is granted by the VAX's
 * reply (section 2's path); the old master's op-0x0f and op-0x11 drive the
 * SHIPPING remaster table, whose holder doors are bound -- as vms_dlm_scs.c
 * binds them -- to the SHIPPING engine's vms_lock_dlm_handover_* entry points;
 * the new master's 82/01 is built by the SHIPPING new-master builder
 * (vms_dlm_rm_lock_answer_build, the one OVMX itself answers with); and the
 * release that follows goes through the shipping FSM again.
 *
 * The two old-master frames are RA5's REAL op-0x0f and op-0x11 (rm5-remaster
 * .pcap, fixtures rm-0f-member-req / rm-11-notif) with exactly three cells
 * substituted, and said here: the resource identity and name at body[44:48+L]
 * and body[128:132] (this file's tree, not RMT5), and body[28:32] of the
 * op-0x0f, which names the new master -- this file's VAX2 CSID instead of
 * RA5's 00010002 (they happen to be the same number; it is written anyway).
 * ========================================================================== */
static struct vms_dlm_rm g_rm;
static struct vms_dlm_rm_ops g_rm_ops;
static struct {
	uint32_t n;
	vms_csid_t dst[8];
	uint8_t body[8][VMS_CM_BODY_LEN];
} g_rm_out;

static uint32_t rmo_members(void *ctx, uint32_t *csids, uint32_t max)
{
	(void)ctx;
	return cnxman_club_other_member_csids(&g.cl.club, (vms_csid_t *)csids,
					      max);
}
static int rmo_open(void *c, const struct vms_dlm_adopt_tree *t)
{ (void)c; return vms_lock_dlm_adopt_open(t) == SS__NORMAL; }
static int rmo_lock(void *c, const struct vms_dlm_adopt_tree *t,
		    const struct vms_dlm_adopt_lock_rec *r,
		    struct vms_dlm_adopt_lock_result *o)
{ (void)c; return vms_lock_dlm_adopt_lock(t, r, o) == SS__NORMAL; }
static int rmo_end(void *c, const struct vms_dlm_adopt_tree *t, uint32_t *n)
{ (void)c; return vms_lock_dlm_adopt_end(t, n) == SS__NORMAL; }
static int rmo_abort(void *c, const struct vms_dlm_adopt_tree *t, uint32_t *n)
{ (void)c; return vms_lock_dlm_adopt_abort(t, n) == SS__NORMAL; }
static int rmo_tree_of(void *c, uint32_t l, struct vms_dlm_adopt_tree *o)
{ (void)c; return vms_lock_dlm_adopt_tree_of_lkid(l, o); }
static void rmo_reported(void *c) { (void)c; vms_lock_dlm_adopt_reported(); }
static int rmo_mastered(void *c, const char *n, uint32_t l, uint16_t g_,
			uint8_t m)
{ (void)c; return vms_lock_dlm_name_mastered_here(n, l, g_, m); }
static int rmo_hash(void *c, const char *n, uint32_t l, uint16_t g_, uint8_t m,
		    uint32_t *o)
{ (void)c; return vms_lock_dlm_dir_hash_of(n, l, g_, m, o); }
static int rmo_dir_node(void *c, const char *n, uint32_t l, uint16_t g_,
			uint8_t m, uint32_t *o)
{ (void)c; (void)n; (void)l; (void)g_; (void)m; (void)o; return 0; }
static int rmo_stage(void *c, const uint8_t *b, uint32_t l)
{ (void)c; (void)b; (void)l; return 0; }
static int rmo_send(void *c, uint32_t dst, const uint8_t *b, uint32_t l)
{
	(void)c;
	if (g_rm_out.n >= 8u || l != VMS_CM_BODY_LEN)
		return -1;
	g_rm_out.dst[g_rm_out.n] = (vms_csid_t)dst;
	memcpy(g_rm_out.body[g_rm_out.n], b, VMS_CM_BODY_LEN);
	g_rm_out.n++;
	return 0;
}
static uint8_t rmo_fill(void *c) { (void)c; return 0u; }
static int rmo_valblk(void *c, const struct vms_dlm_adopt_valblk *v)
{ (void)c; return vms_lock_dlm_adopt_valblk(v) == SS__NORMAL; }
static void rmo_log(void *c, const char *m) { (void)c; (void)m; }
/* The three holder doors, exactly as vms_dlm_scs.c binds them. */
static int rmo_ho_next(void *c, const struct vms_dlm_adopt_tree *t,
		       uint32_t n, struct vms_dlm_handover_rec *o)
{ (void)c; return vms_lock_dlm_handover_next(t, n, o); }
static int rmo_ho_rehome(void *c, uint32_t l, uint32_t o, uint32_t n,
			 uint32_t h)
{ (void)c; return vms_lock_dlm_handover_rehome(l, o, n, h) == SS__NORMAL; }
static int rmo_ho_valblk(void *c, const struct vms_dlm_adopt_valblk *v)
{ (void)c; return vms_lock_dlm_handover_valblk(v) == SS__NORMAL; }

static void rm_table_up(void)
{
	memset(&g_rm_ops, 0, sizeof(g_rm_ops));
	memset(&g_rm_out, 0, sizeof(g_rm_out));
	g_rm_ops.ctx = &g;
	g_rm_ops.other_members = rmo_members;
	g_rm_ops.adopt_open = rmo_open;
	g_rm_ops.adopt_lock = rmo_lock;
	g_rm_ops.adopt_end = rmo_end;
	g_rm_ops.adopt_abort = rmo_abort;
	g_rm_ops.adopt_tree_of_lkid = rmo_tree_of;
	g_rm_ops.adopt_reported = rmo_reported;
	g_rm_ops.name_mastered_here = rmo_mastered;
	g_rm_ops.dir_hash = rmo_hash;
	g_rm_ops.dir_node = rmo_dir_node;
	g_rm_ops.stage_reply = rmo_stage;
	g_rm_ops.send = rmo_send;
	g_rm_ops.residue_fill = rmo_fill;
	g_rm_ops.adopt_valblk = rmo_valblk;
	g_rm_ops.log = rmo_log;
	g_rm_ops.ho_next = rmo_ho_next;
	g_rm_ops.ho_rehome = rmo_ho_rehome;
	g_rm_ops.ho_valblk = rmo_ho_valblk;
	vms_dlm_rm_init(&g_rm, &g_rm_ops);
}

/* RA5's real old-master frame, re-addressed to this file's tree (above). */
static int old_master_frame(const char *spec, const char *name, uint32_t hash,
			    uint8_t *body)
{
	struct vms_fixture fx;
	char path[600];
	char err[VMS_FIXTURE_ERRLEN] = "";
	uint32_t n = (uint32_t)strlen(name), i;

	snprintf(path, sizeof(path), "%s/%s.spec", OVMX_FIXTURE_DIR, spec);
	if (vms_fixture_load(path, OVMX_CLEANROOM_MANIFEST, &fx, err,
			     sizeof(err)) != 0 ||
	    fx.origin != VMS_FIXTURE_ORIGIN_CAPTURE) {
		printf("       reason: %s\n", err);
		return -1;
	}
	memcpy(body, fx.bytes + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	body[44] = (uint8_t)OVMX_RES_GROUP;
	body[45] = (uint8_t)(OVMX_RES_GROUP >> 8);
	body[46] = (uint8_t)OVMX_RES_MODE;
	body[47] = (uint8_t)n;
	for (i = 0u; i < n; i++)
		body[48 + i] = (uint8_t)name[i];
	for (i = 0u; i < 4u; i++)
		body[128 + i] = (uint8_t)(hash >> (8u * i));
	return 0;
}

static uint32_t rd32le(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void the_holder_hands_its_lock_to_the_new_master(void)
{
	const char *N = "EVAC$WORKLOAD";
	const uint32_t VAX_HASH = 0x5a3c0117u;
	const uint32_t VAX_HANDLE = 0x7fff0001u;   /* the old master's grant */
	const uint32_t NEW_HANDLE = 0x020004ccu;   /* what VAX2 mints        */
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t body[VMS_CM_BODY_LEN];
	struct vms_dlm_rm_lock_answer a;
	struct vms_resmaster_args rm;
	struct vms_csb *csb;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, i;
	const uint8_t *rec, *end;

	printf("-- rd vms-33a: a VAX moves a tree this node holds a lock in to "
	       "a THIRD node -- end to end --\n");
	mixed_up(1u, 0u);
	csb = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)CSID_VAX2, 1);
	ct_check(csb != NULL, "the new master, a second real VAX, is a member");
	if (csb == NULL)
		return;
	cnxman_csb_set_csid(csb, CSID_VAX2);
	cnxman_csb_set_lockdirwt(csb, 0u);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	(void)cnxman_ldwv_rebuild(&g.cl.club, &g.cnx_ops);
	proc_init(&app);
	rm_table_up();

	/* The VAX masters the tree; this node's $ENQ goes there and is
	 * granted (section 2's path, unchanged). */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_EXMODE, N, VAX_HASH) == 0 &&
		 vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU, "the VAX masters the tree");
	ct_check(do_enq(&app, N, LCK_K_CRMODE, 0u, &lkid) == SS__NORMAL &&
		 g.last_dst == CSID_VAX && vax_grants(lkid, VAX_HANDLE,
						      (uint8_t)LCK_K_CRMODE) == 0 &&
		 lki_granted_mode(&app, lkid) == LCK_K_CRMODE,
		 "this node's $ENQ at CR goes to the VAX and is granted");

	/* The old master's op-0x0f (naming VAX2) and op-0x11. */
	ct_check(old_master_frame("rm-0f-member-req", N, VAX_HASH, body) == 0,
		 "RA5's real op-0x0f loads");
	for (i = 0u; i < 4u; i++)
		body[28 + i] = (uint8_t)(CSID_VAX2 >> (8u * i));
	ct_check(vms_dlm_rm_step(&g_rm, CSID_VAX, 0x02u, 0x0fu, body,
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_HANDLED &&
		 g_rm.ho_armed == 1u && g_rm_out.n == 0u,
		 "the op-0x0f arms the hand-over; nothing is sent yet");
	ct_check(old_master_frame("rm-11-notif", N, VAX_HASH, body) == 0,
		 "RA5's real op-0x11 loads");
	ct_check(vms_dlm_rm_step(&g_rm, CSID_VAX, 0x02u, 0x11u, body,
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_HANDLED,
		 "the op-0x11 is handled");
	ct_check_eq_u32(g_rm_out.n, 1u, "ONE record leaves this node");
	ct_check_eq_u32((uint32_t)g_rm_out.dst[0], CSID_VAX2,
			"*** addressed to the NEW master ***");
	rec = g_rm_out.body[0];
	ct_check(rec[8] == 0x02 && rec[9] == 0x12, "  an op-0x12");
	ct_check_eq_u32(rd32le(rec + 24), lkid,
			"  body[24:28]: THIS node's own lock id, off the LKB");
	ct_check_eq_u32(rd32le(rec + 20), app.vms_pid,
			"  body[20:24]: the owning process's PID, off its PCB");
	ct_check(rec[30] == LCK_K_CRMODE && rec[31] == LCK_K_CRMODE,
		 "  body[30:32]: held at CR, off the LKB");
	ct_check(rd32le(rec + 36) == 0u && rd32le(rec + 40) == 0u,
		 "  a ROOT record: no parent pair");
	ct_check(rec[47] == (uint8_t)strlen(N) &&
		 memcmp(rec + 48, N, strlen(N)) == 0 &&
		 rd32le(rec + 128) == VAX_HASH,
		 "  the resource's name, and the hash the VAX put on the wire");
	ct_check(vms_dlm_shape_fit_for_any_member(rec, VMS_CM_BODY_LEN) &&
		 !vms_dlm_body_is_notification(rec, VMS_CM_BODY_LEN),
		 "  the connection manager would send it to a real VAX, "
		 "opening a transaction for the answer");

	/* VAX2's 82/01, built by the SHIPPING new-master builder. */
	memset(&a, 0, sizeof(a));
	a.master_lkid = NEW_HANDLE;
	ct_check(vms_dlm_rm_lock_answer_build(rec, VMS_CM_BODY_LEN, &a, frame,
					      sizeof(frame), NULL) ==
		 VMS_CODEC_OK, "VAX2's answer is built");
	ct_check(vms_dlm_rm_ho_answer(&g_rm, CSID_VAX, 0x82u, 0x01u,
				      body_of(frame), VMS_CM_BODY_LEN) == 0,
		 "the same answer from the OLD master is not this record's");
	ct_check(vms_dlm_rm_ho_answer(&g_rm, CSID_VAX2, 0x82u, 0x01u,
				      body_of(frame), VMS_CM_BODY_LEN) == 1,
		 "VAX2's answer is consumed");
	ct_check_eq_u32(g_rm_out.n, 2u, "and the end-of-records follows");
	end = g_rm_out.body[1];
	ct_check(end[9] == 0x13 && end[28] == 0u && rd32le(end + 4) == 0u &&
		 g_rm_out.dst[1] == CSID_VAX2 &&
		 rd32le(end + 128) == VAX_HASH,
		 "*** an op-0x13 to VAX2: the holder role, no transaction, the "
		 "tree's hash -- what the new master waits for (rule W2) ***");
	ct_check(vms_dlm_body_is_notification(end, VMS_CM_BODY_LEN),
		 "  and the CM sends it as a notification");

	/* The lock is VAX2's now -- on this node. */
	read_resmaster(N, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX2,
			"*** the resource's master here is now VAX2 ***");
	ct_check_eq_u32(lki_granted_mode(&app, lkid), LCK_K_CRMODE,
			"  and the lock is still held at CR");

	/* rd vms-3ac: the release goes to VAX2, naming VAX2's handle. */
	g.n_sent = 0u;
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the local $DEQ succeeds");
	(void)dlm_req_fsm_tick(&g.fsm);
	ct_check(g.n_sent >= 1u && g.last_dst == CSID_VAX2 &&
		 g.last[VMS_OFB_DLM_OP] == VMS_DLM_WIREOP_DEQ,
		 "*** the release is sent to the NEW master ***");
	ct_check_eq_u32(rd32le(g.last + 20), NEW_HANDLE,
			"*** naming the handle VAX2 minted -- never the old "
			"master's, which bugchecked a real VAX with INVLOCKID "
			"(rd vms-3ac) ***");
	mixed_down();
}


/* ==========================================================================
 * rd vms-3ea: A RESOURCE NOBODY HOLDS HAS NO MASTER (Davis p. 6-31).
 *
 * MEASURED on the 2-node CLUSTER_CONFIG_LAN e2e (dispatch run 41391191761):
 * node A mounted OVMXDATA and dismounted it, and node B's MOUNT of its own
 * OVMXDATA volume was STILL refused %MOUNT-F-VOLALRMNT -- B's request for
 * F11B$vOVMXDATA was granted through a proxy, mastered by a node that held no
 * lock on it. Two engine facts made the idle resource block immortal and left
 * its master on it:
 *   - a value a peer's frame CONFIRMED (the peer echoes the hash it was sent)
 *     flipped the block from "computed" to "learned", and a learned value is
 *     a preservation reason (resource_release);
 *   - the master's own directory claim (dir_claim_self) was never dropped.
 * Both halves below read the engine's own state after the last $DEQ.
 * ========================================================================== */
#define E3EA_NAME "F11B$vOVMXDATA"

static uint32_t e3ea_computed_hash(void)
{
	uint32_t h = 0u;

	ct_check(vms_dlm_name_hash(OVMX_RES_GROUP, OVMX_RES_MODE,
				   (const uint8_t *)E3EA_NAME,
				   (uint32_t)strlen(E3EA_NAME), &h) ==
		 VMS_DLM_HASH_OK, "the proven function covers the identity");
	return h;
}

static void e3ea_the_master_forgets_an_idle_resource(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_res_ident selfid;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, h, held = 0;

	printf("-- rd vms-3ea: the master and directory forgets a resource "
	       "nobody holds --\n");
	mixed_up(1u, 0u);             /* every name is directed HERE */
	proc_init(&app);
	h = e3ea_computed_hash();

	ct_check(do_enq(&app, E3EA_NAME, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "this node takes the standing NL lock and masters the name");

	/* The peer's lookup, carrying the SAME value this node computed. */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_NLMODE, E3EA_NAME, h) == 0,
		 "the peer's op-0x01 is built with the agreeing value");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_SELF && master == CSID_OVMX,
		 "while this node holds a lock, the directory answers "
		 "THIS NODE MASTERS IT");

	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the last local lock goes");

	ct_check(vms_lock_dlm_dir_hash_of(E3EA_NAME, 0u, OVMX_RES_GROUP,
					  OVMX_RES_MODE, &held) == 0,
		 "*** the idle resource block is GONE: a value the wire only "
		 "confirmed is no reason to keep it ***");
	ct_check(vms_lock_dlm_name_mastered_here(E3EA_NAME, 0u, OVMX_RES_GROUP,
						 OVMX_RES_MODE) == 0,
		 "*** this node no longer claims mastery of it ***");
	memset(&selfid, 0, sizeof(selfid));
	selfid.group = OVMX_RES_GROUP;
	selfid.mode = OVMX_RES_MODE;
	selfid.name_len = (uint8_t)strlen(E3EA_NAME);
	memcpy(selfid.name, E3EA_NAME, selfid.name_len);
	ct_check(vms_dlm_dir_lookup_ident(&g.dir, &selfid, &master, NULL,
					  NULL) == VMS_DLM_DIR_IDENT_NONE,
		 "*** and its own directory entry for the name is dropped ***");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "*** so the peer's next lookup is answered YOU MASTER IT, "
		 "not 'this node masters it' ***");
	mixed_down();
}

/* A's whole sequence on the e2e: its own lock, the peer's request SERVED as
 * master, the peer's release, then its own release. */
static void e3ea_served_then_released_leaves_no_master(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_request mr;
	struct vms_dlm_master_result res, rel;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, h;

	printf("-- rd vms-3ea: served the peer, both released, no master --\n");
	mixed_up(1u, 0u);
	proc_init(&app);
	h = e3ea_computed_hash();

	ct_check(do_enq(&app, E3EA_NAME, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL, "this node's own NL lock");
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_NLMODE, E3EA_NAME, h) == 0,
		 "the peer's NL request");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_SELF, "  answered: this node masters it");
	vax_served_as_master(frame, CSID_VAX, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 res.master_lkid != 0u, "  and GRANTED, as the master");

	memset(&mr, 0, sizeof(mr));
	mr.op = VMS_DLM_MREQ_DEQ;
	mr.req_csid = CSID_VAX;
	mr.req_lkid = VAX_LKID_1;
	mr.master_lkid = res.master_lkid;
	memset(&rel, 0, sizeof(rel));
	ct_check(vms_lock_dlm_master_serve(&mr, &rel) == SS__NORMAL &&
		 rel.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "the peer releases its lock at this master");
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "this node releases its own");

	ct_check(vms_lock_dlm_name_mastered_here(E3EA_NAME, 0u, OVMX_RES_GROUP,
						 OVMX_RES_MODE) == 0,
		 "*** nobody holds it: this node does not master it ***");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "*** and the peer's next lookup is told YOU MASTER IT ***");
	mixed_down();
}

/* The claim goes even when a peer's frame has since changed the value the
 * name routes by: the claim was made under the OLD value's directory (this
 * node), and re-resolving with the new one must not leave it behind. */
static void e3ea_the_claim_goes_after_the_wire_moved_the_value(void)
{
	static const char *const names[] = {
		"E3EA$CLAIM1", "E3EA$CLAIM2", "E3EA$CLAIM3", "E3EA$CLAIM4",
		"E3EA$CLAIM5", "E3EA$CLAIM6", "E3EA$CLAIM7", "E3EA$CLAIM8",
		"E3EA$CLAIMA", "E3EA$CLAIMB", "E3EA$CLAIMC", "E3EA$CLAIMD",
		"F11B$vOVMXDATA", "F11B$vOVMXSRCH", "F11B$vOVMXSYS",
		"F11B$vOVMXASYS", "F11B$vOVMXBSYS", "E3EA$X", "E3EA$Y",
	};
	struct vms_dlm_res_ident selfid;
	struct vms_proc app;
	vms_csid_t master = 0, d = 0;
	const char *N;
	uint32_t lkid = 0, h = 0, x = 0, k;

	printf("-- rd vms-3ea: the self-claim goes even after the wire moved "
	       "the value --\n");
	mixed_up(0u, 0u);             /* the V7.3 default: one entry each */
	proc_init(&app);
	N = a_name_directed_at((vms_csid_t)CSID_OVMX, names,
			       (uint32_t)(sizeof(names) / sizeof(names[0])));
	ct_check(N != NULL, "a name the vector directs HERE");
	if (N == NULL) {
		mixed_down();
		return;
	}
	(void)vms_dlm_name_hash_proven(OVMX_RES_GROUP, OVMX_RES_MODE,
				       (const uint8_t *)N, (uint32_t)strlen(N),
				       &h);
	for (k = 1u; k < 64u; k++) {
		x = h ^ (k << 16);
		if (vms_ldwv_resolve(&g.cl.club.ldwv, vms_ldwv_key(x), &d) ==
		    VMS_LDWV_OK && d == (vms_csid_t)CSID_VAX)
			break;
	}
	ct_check(d == (vms_csid_t)CSID_VAX, "a value that directs at the peer");

	ct_check(do_enq(&app, N, LCK_K_NLMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "this node masters the name and claims it");
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash(N, 0u, OVMX_RES_GROUP,
						    OVMX_RES_MODE, x),
			SS__NORMAL, "a peer's frame states another value");
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the last lock goes");

	memset(&selfid, 0, sizeof(selfid));
	selfid.group = OVMX_RES_GROUP;
	selfid.mode = OVMX_RES_MODE;
	selfid.name_len = (uint8_t)strlen(N);
	memcpy(selfid.name, N, selfid.name_len);
	ct_check(vms_dlm_dir_lookup_ident(&g.dir, &selfid, &master, NULL,
					  NULL) == VMS_DLM_DIR_IDENT_NONE,
		 "*** this node's own claim is gone all the same ***");
	mixed_down();
}

/* The same rule for a block that IS kept: a hash some system put on the wire
 * keeps the block (resource_release), but it is data about the name, never a
 * claim to master it. */
static void e3ea_a_kept_block_keeps_no_master(void)
{
	const char *N = "E3EA$KEPT";
	struct vms_dlm_res_ident selfid;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, held = 0;
	const uint32_t WIRE = 0x3ea05a5au;   /* a value the "wire" stated */

	printf("-- rd vms-3ea: a block kept for a learned hash keeps no "
	       "master --\n");
	mixed_up(1u, 0u);             /* every name is directed HERE */
	proc_init(&app);

	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash(N, 0u, OVMX_RES_GROUP,
						    OVMX_RES_MODE, WIRE),
			SS__NORMAL, "a peer's frame stated a value for the name");
	ct_check(do_enq(&app, N, LCK_K_NLMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "this node takes a lock and masters the name");
	ct_check(vms_lock_dlm_name_mastered_here(N, 0u, OVMX_RES_GROUP,
						 OVMX_RES_MODE) == 1,
		 "  while it holds the lock");
	ct_check(do_deq(&app, lkid) == SS__NORMAL, "the last lock goes");

	ct_check(vms_lock_dlm_dir_hash_of(N, 0u, OVMX_RES_GROUP, OVMX_RES_MODE,
					  &held) == 1 && held == WIRE,
		 "the block is KEPT, with the value the wire stated");
	ct_check(vms_lock_dlm_name_mastered_here(N, 0u, OVMX_RES_GROUP,
						 OVMX_RES_MODE) == 0,
		 "*** but it claims no master: nobody holds the resource ***");
	memset(&selfid, 0, sizeof(selfid));
	selfid.group = OVMX_RES_GROUP;
	selfid.mode = OVMX_RES_MODE;
	selfid.name_len = (uint8_t)strlen(N);
	memcpy(selfid.name, N, selfid.name_len);
	ct_check(vms_dlm_dir_lookup_ident(&g.dir, &selfid, &master, NULL,
					  NULL) == VMS_DLM_DIR_IDENT_NONE,
		 "*** and this node's directory no longer names itself ***");
	mixed_down();
}

static void e3ea_the_requester_forgets_a_released_master(void)
{
	struct vms_proc app;
	uint32_t lkid = 0, held = 0, sent, h;

	printf("-- rd vms-3ea: a requester whose last lock is released "
	       "forgets the master --\n");
	mixed_up(0u, 1u);             /* every name is directed at the peer */
	proc_init(&app);
	h = e3ea_computed_hash();

	ct_check(do_enq(&app, E3EA_NAME, LCK_K_NLMODE, 0u, &lkid) ==
		 SS__NORMAL && lkid != 0u,
		 "the $ENQ leaves as a lookup at the peer (a proxy LKB)");
	ct_check_eq_u32((uint32_t)g.last_dst, CSID_VAX,
			"  addressed to the peer");
	/* The peer's grant echoes our request -- hash included, so the learner
	 * sees a value that AGREES with the one this node computed. */
	ct_check(vax_grants(lkid, 0x0a0b0e3au, (uint8_t)LCK_K_NLMODE) == 0,
		 "the peer's grant is taken by the FSM");
	/* ...and any later frame of the peer's naming the identity carries the
	 * SAME value: the learner records it as a confirmation (the arm's
	 * observe_body does exactly this for every inbound request). */
	ct_check_eq_u32(vms_lock_dlm_learn_dir_hash(E3EA_NAME, 0u,
						    OVMX_RES_GROUP,
						    OVMX_RES_MODE, h),
			SS__NORMAL, "the peer confirms the value on the wire");
	sent = g.n_sent;
	ct_check(do_deq(&app, lkid) == SS__NORMAL,
		 "the proxy is released (a DEQ to the master)");
	ct_check(g.n_sent >= sent, "  (the release is the arm's to send)");

	ct_check(vms_lock_dlm_dir_hash_of(E3EA_NAME, 0u, OVMX_RES_GROUP,
					  OVMX_RES_MODE, &held) == 0,
		 "*** the idle block is gone, and with it the master it was "
		 "told: the next $ENQ asks the directory again ***");
	mixed_down();
}

/*
 * rd vms-d63: A VAX's RELEASE THAT WRITES ITS VALUE BLOCK, served by OVMX AS
 * MASTER -- the other direction of the F11B$s protocol: when OVMX masters the
 * volume's tree, a VAX's header change reaches OVMX only if the master STORES
 * the block the VAX's $DEQ carries (mscp-read-write.pcap frame 1481's form:
 * the cell 1 at body[30]/body[32], the block at body[36:52]).
 *
 * The frame goes through the SHIPPING codec both ways, then into the master
 * door as the wire arm fills it (dlm_arm_fill_deq_req: the block is handed on
 * only when the frame is marked). The engine stores it only for a lock it holds
 * for that sender at a WRITE mode (PW/EX) with LCK$M_VALBLK; an unmarked
 * release, and a read-mode holder's, leave the master's block alone.
 */
static void d63_vax_release(uint32_t vax_lkid, uint32_t master_lkid,
			    int marked, const uint8_t *vb,
			    struct vms_dlm_master_result *out)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_deq d, q;
	struct vms_dlm_master_request mr;
	uint32_t written = 0;

	memset(&d, 0, sizeof(d));
	d.req_lkid = vax_lkid;
	d.master_lkid = master_lkid;
	d.write_valblk = (uint8_t)(marked ? 1 : 0);
	memcpy(d.valblk, vb, 16);
	memset(frame, 0, sizeof(frame));
	ct_check(vms_dlm_deq_build(&d, frame, sizeof(frame), &written) ==
		 VMS_CODEC_OK, "  the VAX's op-0x03 is built");
	memset(&q, 0, sizeof(q));
	ct_check(vms_dlm_deq_parse_body(body_of(frame), VMS_CM_BODY_LEN, &q) ==
		 VMS_CODEC_OK, "  and parsed back by the shipping parser");
	/* As dlm_arm_fill_deq_req fills it (vms_dlm_scs.c). */
	memset(&mr, 0, sizeof(mr));
	mr.op = VMS_DLM_MREQ_DEQ;
	mr.req_csid = CSID_VAX;
	mr.req_lkid = q.req_lkid;
	mr.master_lkid = q.master_lkid;
	if (q.write_valblk) {
		memcpy(mr.valblk, q.valblk, sizeof(mr.valblk));
		mr.valblk_present = 1u;
	}
	memset(out, 0, sizeof(*out));
	(void)vms_lock_dlm_master_serve(&mr, out);
}

static void d63_read_block(struct vms_proc *app, uint32_t lkid, uint8_t *vb)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	a.lkmode = LCK_K_CRMODE;
	a.flags = LCK_M_CONVERT | LCK_M_VALBLK;
	vms_ioctl_convert(app, (unsigned long)(void *)&a);
	memcpy(vb, a.valblk, 16);
	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	a.lkmode = LCK_K_NLMODE;
	a.flags = LCK_M_CONVERT;
	vms_ioctl_convert(app, (unsigned long)(void *)&a);
}

static void d63_a_vax_release_writes_the_master_block(void)
{
	const char *N = "D63$VALBLK";
	static const uint8_t vax_vb[16] = { 3, 0, 0, 0, 1, 0, 0, 0 };
	static const uint8_t other[16] = { 9, 0, 0, 0, 9, 0, 0, 0 };
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;
	struct vms_proc app;
	vms_csid_t master = 0;
	uint32_t lkid = 0, m1;
	uint8_t vb[16];

	printf("-- rd vms-d63: a VAX's value-block release, served by OVMX as "
	       "master --\n");
	mixed_up(1u, 0u);
	proc_init(&app);
	ct_check(do_enq(&app, N, LCK_K_NLMODE, LCK_M_VALBLK, &lkid) ==
		 SS__NORMAL, "a local NL holder masters the name here");

	/* The VAX takes PW with LCK$M_VALBLK, as its XQP takes F11B$s. */
	ct_check(vax_build_enq_flags(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_PWMODE, N, 0x5a3c0d63u,
			       0x0001u /* LCK$M_VALBLK */) == 0,
		 "the VAX's PW request is built");
	(void)vax_lookup(frame, CSID_VAX, &master);
	vax_served_as_master(frame, CSID_VAX, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED &&
		 res.master_lkid != 0u, "  granted PW by this master");
	m1 = res.master_lkid;

	/* UNMARKED first: the master's block must not move. */
	d63_vax_release(VAX_LKID_1, m1, 0, other, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "an UNMARKED release releases");
	d63_read_block(&app, lkid, vb);
	ct_check(memcmp(vb, other, 16) != 0,
		 "*** and writes NOTHING: its body[36:52] is a stale buffer ***");

	/* MARKED, from a PW holder: stored. */
	ct_check(vax_build_enq_flags(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_2,
			       (uint8_t)LCK_K_PWMODE, N, 0x5a3c0d63u,
			       0x0001u /* LCK$M_VALBLK */) == 0,
		 "the VAX's next PW request");
	vax_served_as_master(frame, CSID_VAX, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "  granted PW");
	d63_vax_release(VAX_LKID_2, res.master_lkid, 1, vax_vb, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_RELEASED,
		 "a MARKED release releases");
	d63_read_block(&app, lkid, vb);
	ct_check(memcmp(vb, vax_vb, 16) == 0,
		 "*** and the master STORES its block: a local reader now reads "
		 "the VAX's (3,1) -- OVMX learns of the VAX's header change ***");

	/* MARKED, but from a READ-mode holder: refused. */
	ct_check(vax_build_enq_flags(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_CRMODE, N, 0x5a3c0d63u,
			       0x0001u /* LCK$M_VALBLK */) == 0,
		 "the VAX's CR request");
	vax_served_as_master(frame, CSID_VAX, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED, "  granted CR");
	d63_vax_release(VAX_LKID_1, res.master_lkid, 1, other, &res);
	d63_read_block(&app, lkid, vb);
	ct_check(memcmp(vb, vax_vb, 16) == 0,
		 "*** a CR holder's marked release writes nothing: only PW/EX "
		 "may set the block ***");

	(void)do_deq(&app, lkid);
	mixed_down();
}

/*
 * rd vms-d63 (r3): OVMX's F11B$s LOCK ON THE WIRE, ENQ TO $DEQ, AGAINST VAX2'S.
 *
 * Lab fire vms-5fa r3 (PR #2006, tests/lab/captures/vms-5fa-shared-volume-r3-
 * 20261102/ci6evac-235551/f11b-frames.txt): VAX1 granted OVMX's F11B$s
 * requests with an EMPTY value block 48 of 48 times, and OVMX's 48 releases
 * left unmarked and empty -- so VAX1 overwrote OVMX's records again. Two
 * causes, both on the path below and neither visible to the engine-level test:
 *   1. the request left with body[28:30] = 0, where VAX2's request for the same
 *      kind of lock (frame 21, fid 42) carries 0x0239 -- bit 0 is LCK$M_VALBLK,
 *      and a real master hands a block only to a request that sets it;
 *   2. the RELEASE QUEUE the wire arm stages every $DEQ through kept four
 *      fields and dropped the engine's write mark and block.
 * Driven through the shipping engine, the shipping FSM and the shipping release
 * queue, from vms_lock_acp_file_ser on its own thread to the $DEQ frame.
 */
#define D63_VRES        "F11B$vEVAC5FA     "
#define D63_V_MASTER    0x2400000fu
#define D63_S_MASTER    0x0b000666u
/* VAX2's own bytes, frames 21 (ENQ) and 26 ($DEQ) of the r3 capture. */
static const uint8_t d63_vax2_enq_b28[3] = { 0x39, 0x02, 0x04 };   /* body[28:31] */
static const uint8_t d63_vax2_deq_b30_36[6] = { 0x01, 0x00, 0x01, 0x00, 0x00, 0x00 };
static const uint8_t d63_vax2_deq_vb[16] = { 1, 0, 0, 0, 1, 0, 0, 0 };

struct d63w {
	struct vms_proc *proc;
	uint8_t vb[16];
	uint32_t par, lkid, status;
	volatile int done;
};

static void *d63w_thread(void *arg)
{
	struct d63w *w = arg;

	w->status = vms_lock_acp_file_ser(w->proc, D63_VRES, 18u, 12u, w->vb,
					  &w->par, &w->lkid);
	w->done = 1;
	return NULL;
}

static int d63_grant(uint32_t master_lkid, const uint8_t *vb, uint32_t seq)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_response_build_grant(g.last, VMS_CM_BODY_LEN,
					     master_lkid, vb, seq, frame,
					     (uint32_t)sizeof(frame),
					     &written) != VMS_CODEC_OK)
		return -1;
	return dlm_req_fsm_reply_body(&g.fsm, (vms_csid_t)CSID_VAX, 0u,
				      body_of(frame), VMS_CM_BODY_LEN) ==
	       DLM_REQ_OK ? 0 : -1;
}

static void d63_r3_the_f11b_ser_lock_on_the_wire(void)
{
	static const uint8_t granted[16] = { 2, 0, 0, 0, 1, 0, 0, 0 };
	static const uint8_t written_vb[16] = { 3, 0, 0, 0, 1, 0, 0, 0 };
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_proc app;
	vms_csid_t master = 0;
	struct d63w w;
	pthread_t th;
	uint8_t vb[16];
	const uint8_t *deq;

	printf("-- rd vms-d63 r3: OVMX's F11B$s ENQ and $DEQ on the wire, "
	       "against VAX2's --\n");
	labbed_up(0u, 0u, 0u);
	lab_proc_init(&app);

	/* VAX1 holds the volume lock and this node's directory records it. */
	ct_check(vax_build_enq(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID_1,
			       (uint8_t)LCK_K_CRMODE, D63_VRES, 0x5fa3d063u) == 0,
		 "VAX1's F11B$vEVAC5FA request");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "  VAX1 masters the volume's tree");

	memset(&w, 0, sizeof(w));
	w.proc = &app;
	if (pthread_create(&th, NULL, d63w_thread, &w) != 0) {
		ct_check(0, "pthread_create");
		mixed_down();
		return;
	}
	ct_check(f29f_wait(NULL, &g.n_sent, 1u, 400),
		 "the parent's request left this node");
	ct_check(d63_grant(D63_V_MASTER, NULL, 0u) == 0,
		 "VAX1 grants the NL parent");
	ct_check(f29f_wait(NULL, &g.n_sent, 2u, 400),
		 "the F11B$s request left this node");
	ct_check(g.last[VMS_OFB_DLM_OP] == VMS_DLM_WIREOP_ENQ &&
		 g.last[VMS_OFB_DLM_NAME_LEN] == 10u &&
		 memcmp(g.last + VMS_OFB_DLM_NAME, "F11B$s\x0c\x00\x00\x00", 10) == 0,
		 "  an op-0x01 for 'F11B$s' + 0c 00 00 00");
	ct_check(g.last[30] == d63_vax2_enq_b28[2],
		 "  at PW, as VAX2's (body[30] = 4)");
	{
		uint32_t ours = (uint32_t)g.last[28] | ((uint32_t)g.last[29] << 8);
		uint32_t vax2 = (uint32_t)d63_vax2_enq_b28[0] |
				((uint32_t)d63_vax2_enq_b28[1] << 8);

		ct_check((ours & 0x0001u) && (vax2 & 0x0001u),
			 "*** body[28:30] carries VALBLK (0x0001), as VAX2's "
			 "F11B$s request (0x0239): the request asks the master for "
			 "the value block ***");
		ct_check((ours & 0x0010u) && (vax2 & 0x0010u),
			 "*** and SYSTEM (0x0010), as VAX2's: the system-wide "
			 "resource VAX1's own F11B$s lock is on (rd vms-d63 r4) ***");
		ct_check((ours & 0x0008u) == (vax2 & 0x0008u),
			 "  and SYNCSTS (0x0008), as VAX2's");
		ct_check((ours & ~0x0019u) == 0u,
			 "  and nothing this LKB does not hold (VAX2's NOQUOTA and "
			 "NODLCKWT are semantics this lock manager lacks)");
	}

	ct_check(d63_grant(D63_S_MASTER, granted, 677u) == 0,
		 "VAX1 grants it with the block (2,1)");
	ct_check(f29f_wait(&w.done, NULL, 0u, 400), "vms_lock_acp_file_ser returned");
	if (!w.done) {
		pthread_detach(th);
		mixed_down();
		return;
	}
	pthread_join(th, NULL);
	ct_check(w.status == SS__NORMAL && memcmp(w.vb, granted, 16) == 0,
		 "*** the ACP holds the master's block (2,1) ***");

	memcpy(vb, w.vb, 16);
	vms_f11b_ser_next(vb, 1, 0);
	(void)vms_lock_acp_file_ser_release(&app, w.par, w.lkid, vb);
	ct_check(g.n_sent == 4u, "two releases left this node");
	deq = g.hist[1];                    /* the file's lock goes first */
	ct_check(deq[VMS_OFB_DLM_OP] == VMS_DLM_WIREOP_DEQ &&
		 f29f_rd32(deq + VMS_OFB_DLM_MASTER_LKID) == D63_S_MASTER,
		 "  the first is the F11B$s lock's, to its master's handle");
	ct_check(memcmp(deq + 30, d63_vax2_deq_b30_36, 6) == 0,
		 "*** its body[30:36] is VAX2's own form for a written release "
		 "(01 00 01 00 00 00): the WRITE MARK survived the release queue ***");
	ct_check(memcmp(deq + 36, written_vb, 16) == 0,
		 "*** and body[36:52] carries (3,1) -- VAX2's frame 26 carries "
		 "its own block there the same way ***");
	ct_check(memcmp(d63_vax2_deq_vb + 8, written_vb + 8, 8) == 0,
		 "  bytes 8..15 zero, as VAX2's");
	ct_check(g.hist[0][VMS_OFB_DLM_OP] == VMS_DLM_WIREOP_DEQ &&
		 g.hist[0][30] == 0u,
		 "  the NL parent's release writes nothing");
	mixed_down();
}

int main(void)
{
	printf("=== test_dlm_mixed_master (rd vms-025 / vms-db2a / vms-c27: "
	       "ONE master per resource in a MIXED cluster, R1) ===\n");

	the_vector_routes_at_the_default_lockdirwt();
	an_unproven_identity_sends_nothing_at_the_vax();

	the_captured_frame_and_the_held_out_value();
	lab_arm_a_the_captured_enq_is_served_as_master();
	lab_arm_b_the_ovmx_enq_routes_at_the_vax();
	f29f_a_queued_enqw_wakes_on_the_masters_grant();
	f29f_an_op02_grants_nothing_it_does_not_name();
	f29f_fuzz_the_deferred_grant_entry();
	e7649_the_volume_lock_goes_to_its_directory();

	vax_first_then_ovmx_routes_to_the_vax();
	with_no_entry_the_enq_masters_locally();
	an_entry_without_a_stored_value_is_routed();
	an_uncovered_identity_the_vax_named_is_still_routed();
	a_full_directory_table_still_locks();

	ovmx_first_then_the_vax_is_served_as_master();
	d63_a_vax_release_writes_the_master_block();
	d63_r3_the_f11b_ser_lock_on_the_wire();
	a_vax_noqueue_request_is_denied();
	a_notqueued_sublock_answer_is_refused();
	a_convert_names_the_blocking_holder();

	the_vax_departing_releases_its_locks();
	a_departure_never_touches_a_local_lock();

	the_holder_hands_its_lock_to_the_new_master();

	e3ea_the_master_forgets_an_idle_resource();
	e3ea_the_requester_forgets_a_released_master();
	e3ea_a_kept_block_keeps_no_master();
	e3ea_served_then_released_leaves_no_master();
	e3ea_the_claim_goes_after_the_wire_moved_the_value();

	return ct_summary("test_dlm_mixed_master");
}
