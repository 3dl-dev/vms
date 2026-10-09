/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/dlm_mixed_evacuation.c - rd vms-025 / vms-db2a / vms-c27(3)'s
 * rung-R2 leg: THE WORKLOAD'S LOCK MOVES BETWEEN A REAL-VMS NODE AND AN OVMX
 * NODE, ONE MASTER THROUGHOUT, over the virtual clock.
 *
 * ==========================================================================
 * WHAT MAKES THIS R2 AND NOT A SECOND R1
 * ==========================================================================
 * tests/cluster/host/test_dlm_mixed_master.c drives each decision on ONE node.
 * Two things cannot be seen there, and both are the reason the configuration
 * this work rests on is a REAL configuration and not a local assumption:
 *
 *   1. THAT BOTH SYSTEMS' INDEPENDENTLY BUILT VECTORS AGREE that every root
 *      name's directory node is the OVMX system. "This node is the sole
 *      directory node" is a read of THIS node's copy; it is only TRUE of the
 *      cluster if the OTHER system's copy, built separately from its own CSBs,
 *      sends its lookups here. Davis p. 6-32's "logically equivalent" is a
 *      two-node property and it is swept here over all 65536 values a 16-bit
 *      wire hash can carry -- so no name can be directed anywhere else.
 *   2. THE SEQUENCE, in time. The evacuation is not one decision: the lock is
 *      held on the VMS side, acquired on the OVMX side, handed back, and then
 *      survives the VMS node LEAVING. Each step is driven on the virtual clock
 *      through the SHIPPING requester FSM (including its beat), with the peer
 *      answering in frames the SHIPPING codec built.
 *
 * NOTHING IS MODELLED except the VMS system's own lock manager (it is
 * scripted) and the LAN (a function call). The engine, both CLUBs, both
 * vectors, the directory table, the requester FSM and the codec are the
 * shipping objects -- the same division scenarios/dlm_requester.c states.
 *
 * WHERE EACH VALUE COMES FROM (rd vms-b5b0). For a name the "VAX" names first,
 * the VAX supplies its own 32-bit value as a real system does on every op-0x01
 * (p. 6-50) and this executive LEARNS it. For a name OVMX names first, the
 * engine COMPUTES it with the proven function -- and the scripted VAX
 * DIRECTORY checks, on receipt, both that the frame arrived at the node its
 * OWN vector names for that value and that the value is the one the function
 * predicts for the identity the frame carries. A real VAX does exactly those
 * two things with it (p. 6-50: the directory node indexes its Resource Hash
 * Table with the RECEIVED value), which is why getting it wrong is a grant
 * storm and not an error return.
 */
#include <stdio.h>
#include <string.h>

/* lock_shim/ is FIRST on this target's include path, so these two resolve to
 * FC-P4.9's host backend -- the same recipe scenarios/dlm_requester.c uses. */
#include "vms_internal.h"
#include "exec_kbackend.h"

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_dir.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_scs_fsm.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_hash.h"

/* ==========================================================================
 * The two systems
 *
 * CSV slots 2 and 4 (the low 16 bits of a CSID index the Cluster System
 * Vector, p. 7-25). BOTH SYSTEMS ARE AT THE V7.3 DEFAULT LOCKDIRWT 0, so
 * p. 6-32's "one entry per system when no system has a weight" gives a
 * TWO-ENTRY vector and about half of all root names are directed AT THE VAX.
 * That is the configuration rd vms-b5b0 landed and the interim could not serve:
 * the premise of this file used to be the opposite (every VAX at 0, OVMX above
 * 0, so nothing was ever directed at the VAX and no hash was needed).
 * ========================================================================== */
/* The resource identity the workload's locks carry: the sim's app process runs
 * at access mode USER with uic 0, so (group 0, mode 3) -- and the scripted VMS
 * node's frames carry the same, because these scenarios are about ONE resource
 * (rd vms-b5b0). */
#define WL_GROUP  0u
#define WL_MODE   3u

#define SIM_VAX   0u
#define SIM_OVMX  1u
#define SIM_N     2u

static const vms_csid_t g_csid[SIM_N] = { 0x00010002u, 0x00010004u };
static const uint8_t    g_weight[SIM_N] = { 0u, 0u };
static const char      *g_name[SIM_N] = { "VAX1", "OVMXS" };

#define CSID_VAX   0x00010002u
#define CSID_OVMX  0x00010004u

/* THE EXECUTIVE'S OWN CSID: the OVMX system's entry above, so the vector's
 * "own entries read 0" rule and the engine's routing agree by construction. */
uint32_t vms_local_csid = CSID_OVMX;

void vms_ast_notify_arrival(struct vms_proc *proc) { (void)proc; }

struct simsys {
	struct vms_cluster cl;
	struct cnxman_ops  ops;
	uint32_t           logs;
};

static struct simsys g_sys[SIM_N];
static struct sim_clock g_clock;

static uint32_t sim_now(void *ctx)
{
	(void)ctx;
	return (uint32_t)sim_clock_now_ms(&g_clock);
}

/*
 * MOVE THE VIRTUAL CLOCK. The harness owns the clock -- sim_engine.c sets this
 * same field as it drains its event queue (sim_clock.h: "What a node's
 * ops.now_ms returns"), and this scenario has no event queue to drain because
 * its peer is scripted rather than simulated. Time is therefore INJECTED, which
 * is the property the design asks of every FSM: no clock of its own.
 */
static void clock_advance(uint32_t ms)
{
	g_clock.now_ms += (uint64_t)ms;
}

static void sim_log(void *ctx, const char *msg)
{
	(void)msg;
	((struct simsys *)ctx)->logs++;
}

/* One system's CLUB, built from its OWN CSBs -- its own copy of the vector,
 * through the SHIPPING Phase 2 fill. `deal_backwards` deals the peers in the
 * other order on one system, so a vector built in ALLOCATION order instead of
 * CSV order would disagree and be caught. */
static void sys_form(uint32_t me, int deal_backwards)
{
	struct simsys *s = &g_sys[me];
	struct vms_csb *local;
	uint32_t k;

	memset(s, 0, sizeof(*s));
	s->ops.now_ms = sim_now;
	s->ops.log = sim_log;
	s->ops.ctx = s;

	memcpy(s->cl.params.scsnode, g_name[me], strlen(g_name[me]));
	s->cl.params.scsnode_len = (uint8_t)strlen(g_name[me]);
	s->cl.params.scssystemid = (uint64_t)g_csid[me];
	s->cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&s->cl);

	for (k = 0u; k < SIM_N; k++) {
		uint32_t i = deal_backwards ? (SIM_N - 1u - k) : k;
		struct vms_csb *csb;

		if (i == me)
			continue;
		csb = cnxman_club_alloc_csb(&s->cl.club,
					    (vms_scs_sysid_t)g_csid[i], 1);
		cnxman_csb_set_csid(csb, g_csid[i]);
		cnxman_csb_set_lockdirwt(csb, g_weight[i]);
		cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
						     VMS_CSB_F_MEMBER));
	}
	local = cnxman_club_local(&s->cl.club);
	cnxman_csb_set_csid(local, g_csid[me]);
	cnxman_csb_set_lockdirwt(local, g_weight[me]);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));
	s->cl.club.local_csid = g_csid[me];
	s->cl.club.local_csid_valid = 1u;

	(void)cnxman_ldwv_rebuild(&s->cl.club, &s->ops);
}

/* ==========================================================================
 * 1. BOTH COPIES AGREE, FOR EVERY VALUE A WIRE HASH CAN CARRY
 *
 * Davis p. 6-32's "logically equivalent" is a two-node property, and it is what
 * the whole scheme rests on: a lookup one system addresses must arrive at the
 * node the OTHER system would also have named. Each node's copy differs only in
 * that its OWN entries read 0 (p. 6-32, Fig. 6-18), so the comparison
 * normalises that reading away and then demands EQUALITY for all 65536 keys.
 *
 * This replaces the interim's weaker claim ("every name is directed at the OVMX
 * system", which was true only of the sole-directory configuration): at the
 * default LOCKDIRWT both systems direct about half the names at each other, and
 * THAT is what has to agree (rd vms-b5b0).
 * ========================================================================== */
static vms_csid_t who_directs(uint32_t which_sys, uint16_t key)
{
	vms_csid_t who = 0;

	if (vms_ldwv_resolve(&g_sys[which_sys].cl.club.ldwv, key, &who) !=
	    VMS_LDWV_OK)
		return 0;
	/* 0 is the vector's way of saying "your own entry". */
	return who == 0u ? g_csid[which_sys] : who;
}

static void both_vectors_name_the_same_directory(void)
{
	uint32_t h, disagreed = 0, at_vax = 0, at_ovmx = 0;

	printf("-- both systems' own vectors name the SAME directory node, for "
	       "all 65536 keys --\n");
	sys_form(SIM_VAX, 0);
	sys_form(SIM_OVMX, 1);          /* discovered its peer the other way */

	ct_check(g_sys[SIM_VAX].cl.club.ldwv.valid &&
		 g_sys[SIM_OVMX].cl.club.ldwv.valid,
		 "both systems built a vector from their own CSBs");
	ct_check(g_sys[SIM_VAX].cl.club.ldwv.n == 2u &&
		 g_sys[SIM_OVMX].cl.club.ldwv.n == 2u,
		 "the V7.3 DEFAULT (every member LOCKDIRWT 0) gives one entry "
		 "per system (p. 6-32)");
	ct_check(vms_ldwv_all_ovmx(&g_sys[SIM_OVMX].cl.club.ldwv) == 0,
		 "and the VAX is not proven to run this implementation");

	for (h = 0u; h < 0x10000u; h++) {
		vms_csid_t a = who_directs(SIM_VAX, (uint16_t)h);
		vms_csid_t b = who_directs(SIM_OVMX, (uint16_t)h);

		if (a == 0u || b == 0u || a != b) {
			disagreed++;
			continue;
		}
		if (a == (vms_csid_t)CSID_VAX)
			at_vax++;
		else
			at_ovmx++;
	}
	ct_check_eq_u32(disagreed, 0u,
			"*** all 65536 keys resolve to the SAME system through "
			"BOTH independently-built copies ***");
	ct_check(at_vax > 0u && at_ovmx > 0u,
			"*** and the work is SPLIT: some keys are directed at "
			"the VAX, some here -- which is what a default-weight "
			"cluster is, and what the interim configuration never "
			"had to handle ***");
	printf("   %u keys directed at the VAX, %u here\n",
	       (unsigned)at_vax, (unsigned)at_ovmx);
}

/* ==========================================================================
 * 2. The OVMX node, for real, against a scripted VMS peer
 * ========================================================================== */

struct ovmx_node {
	struct vms_dlm_dir       dir;
	struct vms_dlm_dir_entry dir_store[64];
	struct dlm_req_fsm       fsm;
	struct dlm_req_ops       fsm_ops;
	struct vms_dlm_requester_ops eng_ops;

	uint32_t n_sent;
	vms_csid_t last_dst;
	uint8_t  last[VMS_CM_BODY_LEN];
};

static struct ovmx_node o;
static struct vms_proc o_delivery;      /* the rd vms-c27 delivery proc */
static struct vms_proc o_app;           /* the workload's process       */

static struct vms_ldwv *ovmx_ldwv(void)
{
	return &g_sys[SIM_OVMX].cl.club.ldwv;
}

/* The engine's ask, as a wire identity -- dlm_arm_ask_to_ident. */
static int o_ask_to_ident(const struct vms_dlm_dir_ask *q,
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

static uint32_t o_dir_local_lookup(void *ctx, const struct vms_dlm_dir_ask *q,
				   struct vms_dlm_dir_local *out)
{
	struct vms_dlm_res_ident id;
	enum vms_dlm_dir_ident_outcome r;
	vms_csid_t master = 0u;
	uint32_t hash = 0u;
	uint8_t hash_known = 0u;

	(void)ctx;
	if (q == NULL || out == NULL || o_ask_to_ident(q, &id) != 0)
		return SS__BADPARAM;
	r = vms_dlm_dir_lookup_ident(&o.dir, &id, &master, &hash, &hash_known);
	if (r == VMS_DLM_DIR_IDENT_INVAL)
		return SS__UNSUPPORTED;
	if (r == VMS_DLM_DIR_IDENT_NONE) {
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

static uint32_t o_dir_claim_self(void *ctx, const struct vms_dlm_dir_ask *q)
{
	struct vms_dlm_res_ident id;

	(void)ctx;
	if (q == NULL || o_ask_to_ident(q, &id) != 0)
		return SS__BADPARAM;
	return vms_dlm_dir_claim_self(&o.dir, &id, (vms_csid_t)CSID_OVMX) == 0 ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__INSFMEM;
}

static uint32_t o_dir_resolve(void *ctx, uint32_t hash, uint32_t *out)
{
	vms_csid_t c = 0;

	(void)ctx;
	if (vms_ldwv_resolve(ovmx_ldwv(), vms_ldwv_key(hash), &c) !=
	    VMS_LDWV_OK)
		return SS__UNSUPPORTED;
	*out = (uint32_t)c;
	return SS__NORMAL;
}

static uint32_t o_dir_generation(void *ctx)
{
	(void)ctx;
	return vms_ldwv_generation(ovmx_ldwv());
}

static int o_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	o.n_sent++;
	o.last_dst = dst;
	memset(o.last, 0, sizeof(o.last));
	memcpy(o.last, body, len > sizeof(o.last) ? sizeof(o.last) : len);
	return 0;
}

static int o_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		    vms_csid_t dst, struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, (uint32_t)dst,
					      out) == SS__NORMAL ? 0 : -1;
}

static int o_fsm_dir_resolve(void *ctx, uint32_t hash, vms_csid_t *out)
{
	uint32_t c = 0;

	if (o_dir_resolve(ctx, hash, &c) != SS__NORMAL)
		return -1;
	*out = (vms_csid_t)c;
	return 0;
}

static int o_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			   vms_csid_t master)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid, (uint32_t)master) ==
	       SS__NORMAL ? 0 : -1;
}

static int o_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) == SS__NORMAL ?
	       0 : -1;
}

static int o_grant_recv(void *ctx, const struct vms_dlm_proxy_grant *gr)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(gr) == SS__NORMAL ? 0 : -1;
}

static int o_blkast(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) == SS__NORMAL ? 0 : -1;
}

static int o_learn(void *ctx, const char *resnam, uint16_t group, uint8_t mode,
		   uint32_t hash)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, group, mode, hash) ==
	       SS__NORMAL ? 0 : -1;
}

static void o_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	(void)ctx;
	(void)why;
	(void)vms_lock_dlm_proxy_fail(req_lkid, SS__UNSUPPORTED);
}

static void o_log(void *ctx, const char *m) { (void)ctx; (void)m; }

static uint32_t o_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p == NULL)
		return SS__BADPARAM;
	return dlm_req_fsm_post(&o.fsm, p) == DLM_REQ_OK ?
	       (uint32_t)SS__NORMAL : (uint32_t)SS__UNSUPPORTED;
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

static void ovmx_up(void)
{
	memset(&o, 0, sizeof(o));
	(void)vms_dlm_dir_init(&o.dir, o.dir_store,
			       (uint32_t)(sizeof(o.dir_store) /
					  sizeof(o.dir_store[0])));

	o.fsm_ops.send           = o_send;
	o.fsm_ops.refill_post    = o_refill;
	o.fsm_ops.dir_resolve    = o_fsm_dir_resolve;
	o.fsm_ops.dir_generation = o_dir_generation;
	o.fsm_ops.record_master  = o_record_master;
	o.fsm_ops.assume_mastery = o_assume;
	o.fsm_ops.grant_recv     = o_grant_recv;
	o.fsm_ops.blkast_deliver = o_blkast;
	o.fsm_ops.learn_dir_hash = o_learn;
	o.fsm_ops.fail           = o_fail;
	o.fsm_ops.now_ms         = sim_now;
	o.fsm_ops.log            = o_log;
	o.fsm_ops.ctx            = &o;
	dlm_req_fsm_init(&o.fsm, &o.fsm_ops);

	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	o.eng_ops.post             = o_post;
	o.eng_ops.dir_resolve      = o_dir_resolve;
	o.eng_ops.dir_generation   = o_dir_generation;
	o.eng_ops.dir_local_lookup = o_dir_local_lookup;
	o.eng_ops.dir_claim_self   = o_dir_claim_self;
	o.eng_ops.ctx              = &o;
	vms_lock_dlm_set_requester_ops(&o.eng_ops);

	proc_init(&o_delivery);
	o_delivery.current_mode = PSL_C_USER;
	proc_init(&o_app);
	o_app.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&o_delivery);
	vms_lock_dlm_set_local_csid(CSID_OVMX);
}

static void ovmx_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ==========================================================================
 * 2b. The scripted VMS peer, and the arm's two roles restated
 * ========================================================================== */

static const uint8_t *body_of(const uint8_t *f) { return f + VMS_OFF_SYSAP_BODY; }

static int vax_enq_frame(uint8_t *frame, uint8_t opcode, uint32_t vax_lkid,
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
	/* body[44:48]: the identity that qualifies the name (rd vms-b5b0). The
	 * scripted VMS node names the SAME domain the OVMX workload does --
	 * which is what makes it one resource with two claimants. */
	r.res_group = WL_GROUP;
	r.res_acmode = WL_MODE;
	r.res_ident_valid = 1u;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_request_build(&r, opcode, frame, VMS_CM_FRAME_LEN,
					 &written) == VMS_CODEC_OK ? 0 : -1;
}

/* THE DIRECTORY ROLE (the arm's dlm_arm_handle_request observe + the table
 * ask), for a frame from a system that is not this implementation. */
static enum vms_dlm_dir_outcome vax_lookup(const uint8_t *frame,
					   vms_csid_t from,
					   vms_csid_t *out_master)
{
	struct vms_dlm_res_ident id;
	char nm[VMS_DLM_NAME_MAX + 1];
	uint32_t hash = 0, i;

	if (vms_dlm_res_ident_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					 &id) != VMS_CODEC_OK)
		return VMS_DLM_DIR_ANSWER_NONE;
	if (vms_dlm_dir_hash_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					&hash) == VMS_CODEC_OK) {
		for (i = 0u; i < id.name_len && id.name[i] != 0u; i++)
			nm[i] = (char)id.name[i];
		nm[i] = '\0';
		(void)vms_lock_dlm_learn_dir_hash(nm, id.group, id.mode, hash);
	}
	return vms_dlm_dir_lookup(&o.dir, &id, from, (vms_csid_t)CSID_OVMX,
				  out_master);
}

/* THE MASTER ROLE (dlm_arm_serve_enq_frame). */
static void vax_served(const uint8_t *frame, vms_csid_t from, uint32_t flags,
		       struct vms_dlm_master_result *out)
{
	struct vms_dlm_enq_request e;
	struct vms_dlm_master_request mr;
	uint8_t wireop = 0;
	uint32_t i;

	memset(out, 0, sizeof(*out));
	out->outcome = (uint8_t)VMS_DLM_MASTER_REFUSED;
	if (vms_dlm_enq_request_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					   &wireop, &e) != VMS_CODEC_OK)
		return;
	memset(&mr, 0, sizeof(mr));
	mr.op = (wireop == VMS_DLM_WIREOP_CONVERT) ? VMS_DLM_MREQ_CONVERT :
						     VMS_DLM_MREQ_ENQ;
	mr.req_csid = (uint32_t)from;
	mr.req_lkid = e.req_pid_or_lkid;
	mr.master_lkid = e.master_lkid;
	mr.lkmode = e.mode;
	mr.flags = flags;
	mr.res_group = e.res_group;
	mr.res_mode = e.res_acmode;
	mr.res_ident_valid = e.res_ident_valid;
	for (i = 0u; i < e.name_len && i < sizeof(mr.resnam) - 1u; i++)
		mr.resnam[i] = (char)e.name[i];
	mr.resnam[i] = '\0';
	(void)vms_lock_dlm_master_serve(&mr, out);
}

/* The VMS master's GRANT for a request this node sent it. */
static int vax_grants(uint32_t our_lkid, uint32_t vax_master_lkid, uint8_t mode)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_response_build_grant(our_lkid, vax_master_lkid, mode,
					     frame, (uint32_t)sizeof(frame),
					     &written) != VMS_CODEC_OK)
		return -1;
	return dlm_req_fsm_reply_body(&o.fsm, (vms_csid_t)CSID_VAX, 0u,
				      body_of(frame), VMS_CM_BODY_LEN) ==
	       DLM_REQ_OK ? 0 : -1;
}

static uint32_t do_enq(const char *resnam, uint32_t mode, uint32_t flags,
		       uint32_t *lkid)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = mode;
	a.flags = flags;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	vms_ioctl_enq(&o_app, (unsigned long)(void *)&a);
	if (lkid != NULL)
		*lkid = a.lkid;
	return a.status;
}

static uint32_t do_deq(uint32_t lkid)
{
	struct vms_deq_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_deq(&o_app, (unsigned long)(void *)&a);
	return a.status;
}

static uint32_t granted_mode(uint32_t lkid)
{
	struct vms_getlki_args a;

	memset(&a, 0, sizeof(a));
	a.lkid = lkid;
	vms_ioctl_getlki(&o_app, (unsigned long)(void *)&a);
	return a.status == SS__NORMAL ? a.granted_mode : 0xffffffffu;
}

static void read_resmaster(const char *resnam, struct vms_resmaster_args *out)
{
	memset(out, 0, sizeof(*out));
	strscpy(out->resnam, resnam, sizeof(out->resnam));
	vms_ioctl_get_resmaster(NULL, (unsigned long)(void *)out);
}

/* ==========================================================================
 * 2c. NAMES, CHOSEN BY THE VECTOR (rd vms-b5b0)
 *
 * At the default LOCKDIRWT half the names are directed at the VAX, so a
 * scenario has to say WHICH half it is about -- and ask the real vector rather
 * than hardcode a name whose key could change if the hash ever did. Every
 * candidate is a plausible cluster resource name.
 * ========================================================================== */
static const char *const g_cands[] = {
	"EVAC$W", "EVAC$WK", "EVAC$WKL", "EVAC$WKLD", "EVAC$WORK",
	"EVAC$WORKL", "EVAC$WORKLD", "EVAC$JOB", "EVAC$Q", "EVAC$QUEUE",
	"EVAC$VOL", "EVAC$FILE", "EVAC$DB", "EVAC$IDX", "EVAC$LOG"
};

static uint32_t predicted_hash(const char *name)
{
	uint32_t h = 0;

	if (vms_dlm_name_hash_proven(WL_GROUP, WL_MODE,
				     (const uint8_t *)name,
				     (uint32_t)strlen(name), &h) !=
	    VMS_DLM_HASH_OK)
		return 0u;
	return h;
}

static const char *name_directed_at(vms_csid_t want)
{
	uint32_t i;

	for (i = 0u; i < (uint32_t)(sizeof(g_cands) / sizeof(g_cands[0])); i++) {
		uint32_t h = predicted_hash(g_cands[i]);

		if (h != 0u && who_directs(SIM_OVMX, vms_ldwv_key(h)) == want)
			return g_cands[i];
	}
	return NULL;
}

/* ==========================================================================
 * 2d. THE SCRIPTED VAX *DIRECTORY NODE*
 *
 * It does what a real directory node does with a lookup, and what it CHECKS is
 * the point of this leg (rd vms-b5b0):
 *
 *   1. that the frame arrived at the node ITS OWN copy of the vector names for
 *      the value the frame carries -- the misaddressing a wrong hash causes;
 *   2. that the value IS the one the function predicts for the identity the
 *      frame carries, so the directory would index its own Resource Hash Table
 *      at the right chain (p. 6-50) instead of missing the name and installing
 *      the sender as master of somebody else's resource;
 *   3. and then it ANSWERS, with the SHIPPING builder, from the request's own
 *      bytes.
 * ========================================================================== */
struct vax_dir_seen {
	uint32_t lookups;
	uint32_t misaddressed;      /* arrived at the wrong node by OUR vector */
	uint32_t hash_mismatch;     /* not the value the function predicts     */
	uint8_t  answer[VMS_CM_BODY_LEN];
	uint32_t answer_len;
};

static struct vax_dir_seen g_vaxdir;

static int vax_directory_receives(const uint8_t *body, uint8_t status,
				  uint32_t master_csid)
{
	struct vms_dlm_res_ident id;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0, predicted;

	g_vaxdir.lookups++;
	if (vms_dlm_res_ident_parse_body(body, VMS_CM_BODY_LEN, &id) !=
	    VMS_CODEC_OK)
		return -1;

	/* (1) did it come to the right node, by the VAX's OWN vector? */
	if (who_directs(SIM_VAX, vms_ldwv_key(id.hash)) !=
	    (vms_csid_t)CSID_VAX)
		g_vaxdir.misaddressed++;

	/* (2) is the value the one this identity hashes to? */
	{
		char nm[VMS_DLM_NAME_MAX + 1];
		uint32_t i;

		for (i = 0u; i < id.name_len; i++)
			nm[i] = (char)id.name[i];
		nm[i] = '\0';
		predicted = predicted_hash(nm);
		if (predicted != id.hash)
			g_vaxdir.hash_mismatch++;
	}

	/* (3) the answer, built by the shipping builder from the request. */
	memset(frame, 0, sizeof(frame));
	memcpy(frame + VMS_OFF_SYSAP_BODY, body, VMS_CM_BODY_LEN);
	if (vms_dlm_dir_answer_build(body, VMS_CM_BODY_LEN, status,
				     master_csid, frame,
				     (uint32_t)sizeof(frame), &written) !=
	    VMS_CODEC_OK)
		return -1;
	memcpy(g_vaxdir.answer, frame + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	g_vaxdir.answer_len = VMS_CM_BODY_LEN;
	return 0;
}

/* ==========================================================================
 * 3. THE EVACUATION, in order, on the clock
 * ========================================================================== */
/*
 * THE NAME THE "VAX LOCKED IT FIRST" LEGS USE. It has to be one the vector
 * directs HERE: a lookup the VMS node addresses at this node is only coherent
 * if its own copy names this node for that value (section 1 proves the two
 * copies agree), and rd vms-b5b0 made that a per-name question instead of a
 * configuration. Resolved at setup from the real vector.
 */
#define WL        (wl_here())
/*
 * THE VALUE THE SCRIPTED VMS NODE PUTS ON THE WIRE for that name, and it is
 * the function's own output -- not an invented constant (rd vms-b5b0). A real
 * VAX's value for an identity IS what the determined function computes for it
 * (that is what the two proof legs establish), so a scripted peer carrying any
 * other value would be modelling a VMS node that disagrees with the evidence --
 * and, concretely, would send its lookups to a different directory node than
 * the one both vectors name, making the scenario incoherent.
 */
#define WL_HASH   (predicted_hash(WL))
#define VAX_LKID  0x0a0b0001u

static const char *wl_here(void)
{
	const char *n = name_directed_at((vms_csid_t)CSID_OVMX);

	return n != NULL ? n : "EVAC$WORKLOAD";
}

/*
 * THE LEG rd vms-b5b0 EXISTS FOR: an OVMX-FIRST name whose DIRECTORY IS THE
 * VAX. The request crosses the wire to the VAX's directory, which checks that
 * it arrived at the node its own vector names and that the value is the one the
 * identity hashes to, and answers. Both published outcomes are driven:
 *
 *   0xf9 "nobody masters it -- YOU do"  -> this node becomes the master and the
 *                                         $ENQ completes from a REAL local
 *                                         grant (p. 6-31 outcome 3).
 *   0xf8 "the master is X"              -> this node records X and re-addresses
 *                                         its request there (outcome 2).
 */
static void an_ovmx_first_name_goes_to_the_vax_directory(void)
{
	const char *n;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, st;

	printf("-- an OVMX-FIRST name whose directory is the VAX: it goes "
	       "there --\n");
	sim_clock_init(&g_clock, 31000u);
	ovmx_up();
	memset(&g_vaxdir, 0, sizeof(g_vaxdir));

	n = name_directed_at((vms_csid_t)CSID_VAX);
	ct_check(n != NULL, "the vector directs some root name at the VAX");
	if (n == NULL) {
		ovmx_down();
		return;
	}

	/* *** THE $ENQ: nobody in this cluster has ever named it. *** */
	st = do_enq(n, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u, "the $ENQ is accepted");
	ct_check_eq_u32(o.n_sent, 1u, "one frame left this node");
	ct_check_eq_u32((uint32_t)o.last_dst, CSID_VAX,
			"*** addressed to the VAX -- the DIRECTORY node the "
			"vector names for the value the engine computed ***");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_NLMODE,
			"  and nothing is granted while it waits");

	/* *** THE VAX's DIRECTORY RECEIVES IT *** */
	ct_check(vax_directory_receives(o.last, VMS_DLM_DIR_YOU_MASTER, 0u) == 0,
		 "the VAX's directory read the frame and built its answer");
	ct_check_eq_u32(g_vaxdir.lookups, 1u, "exactly one lookup reached it");
	ct_check_eq_u32(g_vaxdir.misaddressed, 0u,
			"*** and it arrived at the node the VAX's OWN vector "
			"names for that value: no misaddressing ***");
	ct_check_eq_u32(g_vaxdir.hash_mismatch, 0u,
			"*** carrying exactly the value the resource's identity "
			"hashes to -- which is what lets a real directory find "
			"the right chain (p. 6-50) ***");

	/* *** THE ANSWER *** -- 0xf9, parsed by the shipping codec and
	 * dispatched by the shipping FSM, with no hand-held outcome. */
	clock_advance(20u);
	ct_check(dlm_req_fsm_reply_body(&o.fsm, (vms_csid_t)CSID_VAX, 0u,
					g_vaxdir.answer, g_vaxdir.answer_len) ==
		 DLM_REQ_OK,
		 "the 0xf9 answer is accepted (parsed as a DIRECTORY answer, "
		 "not misread as a DENY)");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_EXMODE,
			"*** the $ENQ COMPLETED: this node assumed mastery and "
			"granted the lock locally (p. 6-31 outcome 3) ***");
	read_resmaster(n, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u,
			"  and the lock database says this node masters it");
	ct_check_eq_u32(rm.dir_csid, CSID_VAX,
			"  while the DIRECTORY is still the VAX: the two roles "
			"are different nodes, which is the normal case");
	ct_check(do_deq(lkid) == SS__NORMAL, "and it releases");
	ovmx_down();

	/* ---- the same, answered 0xf8: the master is somewhere else ---- */
	printf("-- ... and the 0xf8 REDIRECT: the request re-addresses --\n");
	sim_clock_init(&g_clock, 41000u);
	ovmx_up();
	memset(&g_vaxdir, 0, sizeof(g_vaxdir));
	lkid = 0;
	st = do_enq(n, LCK_K_EXMODE, 0u, &lkid);
	ct_check(st == SS__NORMAL && lkid != 0u, "the $ENQ is accepted again");
	ct_check(vax_directory_receives(o.last, VMS_DLM_DIR_REDIRECT,
					CSID_VAX) == 0,
		 "the VAX's directory answers 'the master is VAX1'");
	o.n_sent = 0u;
	ct_check(dlm_req_fsm_reply_body(&o.fsm, (vms_csid_t)CSID_VAX, 0u,
					g_vaxdir.answer, g_vaxdir.answer_len) ==
		 DLM_REQ_OK, "the 0xf8 answer is accepted");
	read_resmaster(n, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"*** the master the directory named is RECORDED in the "
			"lock database -- which is what makes the retry's "
			"destination an executive read ***");
	ct_check(o.n_sent >= 1u && o.last_dst == (vms_csid_t)CSID_VAX,
		 "  and the request is re-addressed THERE");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_NLMODE,
			"  with nothing granted on a routing fact alone");
	ovmx_down();
}

static void the_workload_lock_moves_vms_to_ovmx(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_resmaster_args rm;
	vms_csid_t master = 0;
	uint32_t lkid = 0;

	printf("-- the VMS node holds the workload lock; OVMX takes it over "
	       "--\n");
	sim_clock_init(&g_clock, 12345u);
	ovmx_up();

	/* t0: the VMS node locks the workload resource. Its lookup comes HERE,
	 * because its own vector says so (section 1), and this node records it
	 * as the master and serves nothing else. */
	ct_check(vax_enq_frame(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID,
			       (uint8_t)LCK_K_EXMODE, WL, WL_HASH) == 0,
		 "the VMS node's op-0x01 for the workload lock is built");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_YOU && master == CSID_VAX,
		 "this node, its directory, answers 'you master it'");

	/* t0+50ms: the OVMX standby asks for the same lock. */
	clock_advance(50u);
	ct_check(do_enq(WL, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL &&
		 lkid != 0u, "the OVMX side's $ENQ is accepted");
	read_resmaster(WL, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"*** ONE master, and it is the VMS node ***");
	ct_check_eq_u32(rm.is_local_master, 0u,
			"  OVMX did not master the same resource a second "
			"time");
	ct_check_eq_u32(o.n_sent, 1u, "one request went on the wire");
	ct_check_eq_u32((uint32_t)o.last_dst, CSID_VAX, "  to the VMS node");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_NLMODE,
			"  and the requester is genuinely waiting (NL)");

	/* t0+2.05s: no answer yet, and the FSM's own retransmit deadline
	 * (DLM_REQ_RETRY_MS) has passed. Its beat re-sends -- from a FRESH read
	 * of the lock database, never from the frame it sent before. */
	clock_advance(DLM_REQ_RETRY_MS + 50u);
	o.n_sent = 0u;
	(void)dlm_req_fsm_tick(&o.fsm);
	ct_check(o.n_sent >= 1u && o.last_dst == (vms_csid_t)CSID_VAX,
		 "the retransmit ladder re-addresses the SAME master");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_NLMODE,
			"  and nothing was granted locally while it waited "
			"(no fabricated completion)");

	/* t0+2s: the VMS process $DEQs, and its master grants the OVMX
	 * requester -- naming OUR handle, which is the only way its reply can
	 * find the right lock. */
	clock_advance(1000u);
	ct_check(vax_grants(lkid, 0x7fff0001u, (uint8_t)LCK_K_EXMODE) == 0,
		 "the VMS master's grant arrives");
	ct_check_eq_u32(granted_mode(lkid), LCK_K_EXMODE,
			"*** THE WORKLOAD'S LOCK IS NOW HELD ON THE OVMX NODE "
			"-- EX, from a real grant by the real master ***");
	read_resmaster(WL, &rm);
	ct_check_eq_u32(rm.master_csid, CSID_VAX,
			"  and the master is STILL the VMS node: the lock "
			"moved, the mastery did not");

	/* And it can be given back. */
	ct_check(do_deq(lkid) == SS__NORMAL,
		 "the OVMX side can release it again");
	ovmx_down();
}

static void a_name_ovmx_masters_is_served_to_the_vms_node(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;
	struct vms_resmaster_args rm;
	vms_csid_t master = 0;
	uint32_t lkid = 0;

	printf("-- the other direction: OVMX masters it and SERVES the VMS "
	       "node --\n");
	sim_clock_init(&g_clock, 7000u);
	ovmx_up();

	ct_check(do_enq(WL, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL,
		 "the OVMX side locks the workload resource first");
	read_resmaster(WL, &rm);
	ct_check_eq_u32(rm.is_local_master, 1u, "  and masters it");

	clock_advance(100u);
	ct_check(vax_enq_frame(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID,
			       (uint8_t)LCK_K_EXMODE, WL, WL_HASH) == 0,
		 "the VMS node's op-0x01 is built");
	ct_check(vax_lookup(frame, CSID_VAX, &master) ==
		 VMS_DLM_DIR_ANSWER_SELF && master == CSID_OVMX,
		 "*** its lookup is answered THIS NODE MASTERS IT -- never "
		 "'you master it', which would be a second master ***");

	/* NOQUEUE first: a real conflict, refused honestly. */
	vax_served(frame, CSID_VAX, LCK_M_NOQUEUE, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_DENIED,
		 "a NOQUEUE request is DENIED (SS$_NOTQUEUED), from a real "
		 "incompatibility with a real holder");

	/* Then the queueing form, and the handover. */
	vax_served(frame, CSID_VAX, 0u, &res);
	ct_check(res.outcome == (uint8_t)VMS_DLM_MASTER_QUEUED,
		 "the queueing form is really QUEUED here");
	clock_advance(500u);
	ct_check(do_deq(lkid) == SS__NORMAL, "the OVMX holder releases");
	read_resmaster(WL, &rm);
	ct_check_eq_u32(rm.remote_holder_csid, CSID_VAX,
			"*** and the lock is now GRANTED to the VMS node, held "
			"for its cluster identity by this executive ***");
	ovmx_down();
}

static void the_vms_node_leaves_and_the_standby_runs(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result held;
	struct vms_resmaster_args rm;
	uint32_t lkid = 0, nl = 0, released = 0;

	printf("-- the VMS node departs: its locks go and the standby is "
	       "granted --\n");
	sim_clock_init(&g_clock, 20000u);
	ovmx_up();

	/* OVMX masters the tree (it touched it first at NL and let go), the
	 * VMS node holds EX across the wire, and the OVMX standby queues. */
	ct_check(do_enq(WL, LCK_K_NLMODE, 0u, &nl) == SS__NORMAL &&
		 do_deq(nl) == SS__NORMAL, "OVMX masters the resource");
	ct_check(vax_enq_frame(frame, VMS_DLM_WIREOP_ENQ, VAX_LKID,
			       (uint8_t)LCK_K_EXMODE, WL, WL_HASH) == 0 &&
		 (vax_served(frame, CSID_VAX, 0u, &held), 1) &&
		 held.outcome == (uint8_t)VMS_DLM_MASTER_GRANTED,
		 "the VMS node holds EX, granted by this master");
	ct_check(do_enq(WL, LCK_K_EXMODE, 0u, &lkid) == SS__NORMAL &&
		 granted_mode(lkid) == LCK_K_NLMODE,
		 "the OVMX standby is queued behind it");

	/* *** THE DEPARTURE *** -- the membership path, not a process death. */
	clock_advance(3000u);
	(void)dlm_req_fsm_peer_gone(&o.fsm, (vms_csid_t)CSID_VAX);
	vms_lock_dlm_member_departed(CSID_VAX, NULL);
	(void)vms_lock_dlm_release_csid_locks(CSID_VAX, &released);

	ct_check_eq_u32(granted_mode(lkid), LCK_K_EXMODE,
			"*** the standby holds EX: the workload can take over "
			"after the VMS node leaves ***");
	read_resmaster(WL, &rm);
	ct_check_eq_u32(rm.remote_holder_csid, 0u,
			"  and no lock is left held for the departed system");
	ct_check(do_deq(lkid) == SS__NORMAL, "and it releases cleanly");
	ovmx_down();
}

int main(void)
{
	printf("=== sim/dlm_mixed_evacuation (rd vms-025 / vms-db2a / "
	       "vms-c27: the workload's lock moves, ONE master, R2) ===\n");

	both_vectors_name_the_same_directory();
	an_ovmx_first_name_goes_to_the_vax_directory();
	the_workload_lock_moves_vms_to_ovmx();
	a_name_ovmx_masters_is_served_to_the_vms_node();
	the_vms_node_leaves_and_the_standby_runs();

	return ct_summary("sim/dlm_mixed_evacuation");
}
