/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/dlm_requester.c - FC-P4.6's rung-R2 leg: the REAL lock engine, the
 * REAL requester FSM and the REAL Lock Directory Weight Vector, in one host
 * process, against simulated master/directory systems.
 *
 * ==========================================================================
 * WHAT IS REAL HERE, AND IT IS NEARLY EVERYTHING
 * ==========================================================================
 *   the lock engine     src/kernel-core/vms_lock.c, on FC-P4.9's host backend.
 *                       A $ENQ here is the SAME $ENQ the executive runs: the
 *                       same proxy LKB, the same dlm_resolve_master, the same
 *                       dlm_proxy_fill_post, the same condition-variable wait.
 *   the requester FSM   src/kernel-core/vms_dlm_scs_fsm.c, unchanged.
 *   the directory       src/kernel-core/vms_dlm_ldwv.c, filled by the SHIPPING
 *                       cnxman_phase2_commit() from real CSBs on a real CLUB --
 *                       four systems, weights 1/3/0/2, six entries.
 *   the wire            src/kernel-core/vms_cluster_codec_dlm.c, both ways.
 *
 * SIMULATED: the peers' own lock managers, and the LAN (a function call). That
 * is the honest division -- the plan row says "requester vs SIMULATED
 * master/directory", and a second real engine is not possible in one process
 * anyway (vms_lock.c's databases are module globals, correctly).
 *
 * ==========================================================================
 * THE TWO THINGS ONE NODE CANNOT PROVE, WHICH IS WHY THIS RUNG EXISTS
 * ==========================================================================
 *   1. THAT THE LOOKUP LANDS. The requester resolves a hash through ITS copy of
 *      the vector and sends the lookup to the system it names; that system asks
 *      ITS OWN copy -- through the shipping cnxman_dir_lookup_received() --
 *      whether the lookup was addressed to it. That round trip is the CSV-order
 *      hypothesis's falsifier, and it needs a sender and a receiver.
 *   2. THAT A NOVEL NAME IS REFUSED WITHOUT POSTING. Not "returns an error" --
 *      that a $ENQ for a root name no system in this cluster has ever named on
 *      the wire puts ZERO FRAMES on the LAN. The strawman's defect was not a
 *      bad status, it was a frame leaving this node with a hash of 0, which
 *      made a real VAX install OVMX as master of resources it did not master
 *      (the 35/s grant storm). A status-only test passes on that build.
 */
#include <stdio.h>
#include <string.h>

/* The lock shim comes FIRST on this target's include path (see the CMake
 * comment), so these two names resolve to the host backend, exactly as they do
 * for tests/cluster/host/test_lock_dir.c. */
#include "vms_internal.h"
#include "exec_kbackend.h"

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_scs_fsm.h"
#include "vms_frame_compose.h"

/* ==========================================================================
 * The four-system cluster (the same shape scenarios/dlm_directory.c uses, so
 * the two legs are talking about one vector).
 * ========================================================================== */
#define SIM_N 4u
#define SIM_OVMX 3u

static const vms_csid_t g_csid[SIM_N] = {
	0x00010001u, 0x00010002u, 0x00010003u, 0x00010004u
};
static const uint8_t g_weight[SIM_N] = { 1u, 3u, 0u, 2u };
static const char *g_name[SIM_N] = { "VAXA", "VAXB", "VAXC", "OVMXS" };

/* THE EXECUTIVE'S OWN CSID. vms_lock.c reads this global; it is OVMX's entry in
 * the cluster above, so the vector's "own entries read 0" rule and the engine's
 * own routing agree by construction rather than by coincidence. */
uint32_t vms_local_csid = 0x00010004u;

struct simsys {
	struct vms_cluster cl;
	struct cnxman_ops  ops;
	uint32_t           logs;
};

static struct simsys g_sys[SIM_N];
static struct sim_clock g_clock;

static uint32_t sim_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)sim_clock_now_ms(&g_clock);
}

static void sim_log(void *ctx, const char *msg)
{
	(void)msg;
	((struct simsys *)ctx)->logs++;
}

static void sys_form(uint32_t me)
{
	struct simsys *s = &g_sys[me];
	uint32_t i;

	memset(s, 0, sizeof(*s));
	s->ops.now_ms = sim_now_ms;
	s->ops.log = sim_log;
	s->ops.ctx = s;

	memcpy(s->cl.params.scsnode, g_name[me], strlen(g_name[me]));
	s->cl.params.scsnode_len = (uint8_t)strlen(g_name[me]);
	s->cl.params.scssystemid = (uint64_t)g_csid[me];
	s->cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&s->cl);

	for (i = 0; i < SIM_N; i++) {
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
	{
		struct vms_csb *local = cnxman_club_local(&s->cl.club);

		cnxman_csb_set_csid(local, g_csid[me]);
		cnxman_csb_set_lockdirwt(local, g_weight[me]);
		cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
						       VMS_CSB_F_MEMBER));
		cnxman_club_learn_local_csid(&s->cl.club, g_csid[me]);
	}
}

static void cluster_form(void)
{
	struct cnxman_phase2_in in;
	struct cnxman_phase2_stats st;
	uint32_t i;

	for (i = 0; i < SIM_N; i++) {
		sys_form(i);
		memset(&in, 0, sizeof(in));
		(void)cnxman_phase2_commit(&g_sys[i].cl, &in, &st,
					   &g_sys[i].ops);
	}
}

/* ==========================================================================
 * The virtual LAN: one function call, and a record of everything on it
 * ========================================================================== */
#define MAX_WIRE 64

struct wire_frame {
	vms_csid_t dst;
	uint8_t    body[DLM_REQ_BODY_LEN];
	uint32_t   len;
};

static struct wire_frame g_wire[MAX_WIRE];
static uint32_t g_wire_n;

static struct dlm_req_fsm g_fsm;
static struct vms_proc g_proc;

/* who the simulated cluster says masters the resource under test */
static vms_csid_t g_sim_master;
/* what handle that simulated master assigns */
static uint32_t g_sim_master_lkid;
/* the completions/commits the simulated master received */
static uint32_t g_completions_rx;
static uint32_t g_commits_rx;
static uint32_t g_completion_master_lkid;
/* the routing self-check's verdict, from the RECEIVER's own vector */
static uint32_t g_lookups_landed;
static uint32_t g_lookups_misaddressed;

static uint32_t which_sys(vms_csid_t csid)
{
	uint32_t i;

	for (i = 0; i < SIM_N; i++)
		if (g_csid[i] == csid)
			return i;
	return SIM_N;
}

/* Splice a captured body under a test-only link so the shipping codec will
 * classify it. Never transmitted; it exists for the length of one parse. */
static uint32_t splice(const uint8_t *body, uint32_t len, uint8_t *frame)
{
	struct vms_cm_link link;
	uint32_t written = 0;

	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	memcpy(frame + VMS_OFF_SYSAP_BODY, body,
	       len > DLM_REQ_BODY_LEN ? DLM_REQ_BODY_LEN : len);
	return VMS_CM_FRAME_LEN;
}

/*
 * Did the requester address this destination as a DIRECTORY node? Read from the
 * requester arm's own readback, which is the sender's routing decision.
 */
static int sender_addressed_the_directory(uint32_t req_lkid)
{
	const struct dlm_req *r = dlm_req_fsm_find(&g_fsm, req_lkid);

	return r != NULL && r->to_directory != 0u;
}

static uint32_t make_grant_frame(uint8_t *frame, uint32_t req_lkid,
				 uint32_t master_lkid, uint8_t mode)
{
	struct vms_cm_link link;
	uint32_t written = 0;

	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	(void)vms_dlm_enq_response_build_grant(req_lkid, master_lkid, mode,
					       frame, VMS_CM_FRAME_LEN,
					       &written);
	frame[VMS_OFF_DLM_CAT] = (uint8_t)(VMS_DLM_CAT_REQUEST | 0x80u);
	return VMS_CM_FRAME_LEN;
}

/*
 * ONE SIMULATED SYSTEM RECEIVES ONE cat-0x02 MESSAGE.
 *
 * It behaves the way Davis pp. 6-31/6-32 says a system behaves, and it makes
 * every decision from ITS OWN state: its own copy of the vector decides whether
 * it is the directory (through the SHIPPING cnxman_dir_lookup_received), and
 * the scenario's own g_sim_master decides who masters the tree.
 */
static void peer_receive(uint32_t sys, const uint8_t *body, uint32_t len)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t rframe[VMS_CM_FRAME_LEN];
	struct vms_frame_info fi;
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;
	uint32_t flen = splice(body, len, frame);
	vms_wire_view_t v;
	uint8_t op;

	if (vms_frame_classify(frame, flen, &fi) != VMS_CODEC_OK)
		return;

	/* The completion/commit pair has no parser (PROVISIONAL), so its
	 * opcode and its master handle are read through the codec's own
	 * published offsets. */
	vms_wire_view_init(&v, frame, flen);
	op = vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
	if (op == VMS_DLM_WIREOP_COMPLETE_PROVISIONAL) {
		g_completions_rx++;
		g_completion_master_lkid =
			vms_wire_get_le32(&v, VMS_OFF_DLM_COMPLETE_MASTER_LKID);
		return;
	}
	if (op == VMS_DLM_WIREOP_COMMIT_PROVISIONAL) {
		g_commits_rx++;
		return;
	}

	if (vms_dlm_enq_request_parse(frame, flen, &fi, &opcode, &req) !=
	    VMS_CODEC_OK)
		return;

	/*
	 * THE ORDER SELF-CHECK, run on the RECEIVER's own vector -- and only
	 * for a frame that really IS a directory lookup.
	 *
	 * op-0x01 is the same opcode either way (spec §4(f).1): what makes a
	 * frame a LOOKUP rather than a request is that the sender addressed it
	 * through the weight vector instead of at a master the cluster named.
	 * On a real wire the receiver infers that from its own vector; here the
	 * harness reads the SENDER's own routing decision out of the requester
	 * arm's readback (dlm_req_fsm_find on the frame's OWN req_lkid ->
	 * to_directory), which is the same
	 * fact without the inference. Running the check on a request addressed
	 * to a MASTER would be asking "are you the directory for a frame nobody
	 * sent you as a lookup", and its answer means nothing.
	 */
	if (sender_addressed_the_directory(req.req_pid_or_lkid)) {
		if (cnxman_dir_lookup_received(&g_sys[sys].cl.club,
					       req.dir_hash,
					       g_csid[SIM_OVMX],
					       &g_sys[sys].ops))
			g_lookups_landed++;
		else
			g_lookups_misaddressed++;
	}

	if (g_csid[sys] == g_sim_master) {
		/* This system masters the tree: it GRANTS, with a handle of its
		 * own choosing. */
		uint32_t rlen = make_grant_frame(rframe, req.req_pid_or_lkid,
						 g_sim_master_lkid, req.mode);

		(void)dlm_req_fsm_reply(&g_fsm, g_csid[sys], 0u, rframe, rlen);
		return;
	}

	/* It is the directory but not the master: outcome 2. */
	(void)dlm_req_fsm_redirect(&g_fsm, req.req_pid_or_lkid, g_sim_master);
}

static int fsm_send(void *ctx, vms_csid_t dst, const uint8_t *body,
		    uint32_t len)
{
	uint32_t sys = which_sys(dst);

	(void)ctx;
	if (g_wire_n < MAX_WIRE) {
		struct wire_frame *w = &g_wire[g_wire_n++];

		memset(w, 0, sizeof(*w));
		w->dst = dst;
		w->len = len > DLM_REQ_BODY_LEN ? DLM_REQ_BODY_LEN : len;
		memcpy(w->body, body, w->len);
	}
	if (sys >= SIM_N)
		return -1;
	peer_receive(sys, body, len);
	return 0;
}

/* ==========================================================================
 * The FSM's engine doors, bound to the REAL vms_lock.c entry points
 * ========================================================================== */
static int fsm_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		      vms_csid_t dst, struct vms_dlm_proxy_post *out)
{
	(void)ctx;
	return vms_lock_dlm_proxy_refill_post(req_lkid, op, dst, out) ==
	       (uint32_t)SS__NORMAL ? 0 : -1;
}

static int fsm_dir_resolve(void *ctx, uint16_t hash16, vms_csid_t *out)
{
	(void)ctx;
	return vms_ldwv_resolve(&g_sys[SIM_OVMX].cl.club.ldwv, hash16, out) ==
	       VMS_LDWV_OK ? 0 : -1;
}

static uint32_t fsm_dir_generation(void *ctx)
{
	(void)ctx;
	return vms_ldwv_generation(&g_sys[SIM_OVMX].cl.club.ldwv);
}

static int fsm_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			     vms_csid_t master_csid)
{
	(void)ctx;
	return vms_lock_dlm_record_master(resnam, req_lkid, master_csid) ==
	       (uint32_t)SS__NORMAL ? 0 : -1;
}

static int fsm_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_assume_mastery(resnam, req_lkid) ==
	       (uint32_t)SS__NORMAL ? 0 : -1;
}

static int fsm_grant(void *ctx, const struct vms_dlm_proxy_grant *g)
{
	(void)ctx;
	return vms_lock_dlm_proxy_grant_recv(g) == (uint32_t)SS__NORMAL ? 0 : -1;
}

static int fsm_blkast(void *ctx, uint32_t req_lkid)
{
	(void)ctx;
	return vms_lock_dlm_proxy_blkast_recv(req_lkid) ==
	       (uint32_t)SS__NORMAL ? 0 : -1;
}

static int fsm_learn(void *ctx, const char *resnam, uint16_t hash16)
{
	(void)ctx;
	return vms_lock_dlm_learn_dir_hash(resnam, hash16) ==
	       (uint32_t)SS__NORMAL ? 0 : -1;
}

/*
 * enum dlm_req_fail_reason -> SS$_. The MAPPING is FC-P4.8's glue in
 * production; it is written out here because R2 drives the whole chain.
 *
 * Only two codes are used, and deliberately: SS$_NOTQUEUED for a master that
 * really declined, and SS$_UNSUPPORTED for everything else -- which is the
 * status the engine's OWN member-departure path already gives a proxy whose
 * master is gone (vms_lock.c dlm_proxies_master_departed). This tree defines no
 * SS$_TIMEOUT and no SS$_PATHLOST, and inventing a number for either would be
 * inventing a published constant (Rule 8). FC-P4.6 reports that as the residual
 * it is; a distinct code is an operator matter, not a test's.
 */
static void fsm_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	uint32_t st = (why == DLM_REQ_FAIL_NOTQUEUED)
			      ? (uint32_t)SS__NOTQUEUED
			      : (uint32_t)SS__UNSUPPORTED;

	(void)ctx;
	(void)vms_lock_dlm_proxy_fail(req_lkid, st);
}

static uint32_t fsm_now(void *ctx)
{
	(void)ctx;
	return (uint32_t)sim_clock_now_ms(&g_clock);
}

static void fsm_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

static struct dlm_req_ops g_fsm_ops;

/* ==========================================================================
 * The ENGINE's requester ops -- what vms_lock.c posts through. `post` is the
 * FSM, and the directory resolver is the SHIPPING vector. This is the FC-P4.8
 * binding, written out here so R2 exercises the real chain.
 * ========================================================================== */
static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	return dlm_req_fsm_post(&g_fsm, p) == DLM_REQ_OK
		       ? (uint32_t)SS__NORMAL
		       : (uint32_t)SS__UNSUPPORTED;
}

static uint32_t eng_dir_resolve(void *ctx, uint16_t hash16, uint32_t *out)
{
	(void)ctx;
	return fsm_dir_resolve(NULL, hash16, out) == 0 ? (uint32_t)SS__NORMAL
						       : (uint32_t)SS__UNSUPPORTED;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return fsm_dir_generation(NULL);
}

static void bind_everything(void)
{
	struct vms_dlm_requester_ops eng;

	memset(&g_fsm_ops, 0, sizeof(g_fsm_ops));
	g_fsm_ops.send = fsm_send;
	g_fsm_ops.refill_post = fsm_refill;
	g_fsm_ops.dir_resolve = fsm_dir_resolve;
	g_fsm_ops.dir_generation = fsm_dir_generation;
	g_fsm_ops.record_master = fsm_record_master;
	g_fsm_ops.assume_mastery = fsm_assume;
	g_fsm_ops.grant_recv = fsm_grant;
	g_fsm_ops.blkast_deliver = fsm_blkast;
	g_fsm_ops.learn_dir_hash = fsm_learn;
	g_fsm_ops.fail = fsm_fail;
	g_fsm_ops.now_ms = fsm_now;
	g_fsm_ops.log = fsm_log;
	dlm_req_fsm_init(&g_fsm, &g_fsm_ops);

	memset(&eng, 0, sizeof(eng));
	eng.post = eng_post;
	eng.dir_resolve = eng_dir_resolve;
	eng.dir_generation = eng_dir_generation;
	vms_lock_dlm_set_requester_ops(&eng);
}

/* ==========================================================================
 * Harness
 * ========================================================================== */
void vms_ast_notify_arrival(struct vms_proc *proc);
void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
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

static uint32_t do_enq(const char *resnam, uint32_t lkmode, uint32_t *lkid_out)
{
	struct vms_enq_args a;

	memset(&a, 0, sizeof(a));
	a.lkmode = lkmode;
	strscpy(a.resnam, resnam, sizeof(a.resnam));
	vms_ioctl_enq(&g_proc, (unsigned long)(void *)&a);
	if (lkid_out)
		*lkid_out = a.lkid;
	return a.status;
}

static void do_getlki(uint32_t lkid, struct vms_getlki_args *out)
{
	memset(out, 0, sizeof(*out));
	out->lkid = lkid;
	vms_ioctl_getlki(&g_proc, (unsigned long)(void *)out);
}

/*
 * Teach OVMX a root name's directory hash the ONLY way it can be taught: a
 * frame another system sent, carrying both the name and its own 16-bit value,
 * through the shipping dlm_req_fsm_observe().
 */
static void wire_teaches_hash(const char *resnam, uint16_t hash)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_cm_link link;
	struct vms_dlm_enq_request req;
	uint32_t written = 0;

	memset(&req, 0, sizeof(req));
	req.mode = VMS_LCK_PR;
	req.req_pid_or_lkid = 0x5150u;      /* the SENDER's handle, not ours */
	req.dir_hash = hash;
	req.dir_hash_valid = 1u;
	req.name_len = (uint8_t)strlen(resnam);
	memcpy(req.name, resnam, req.name_len);

	memset(&link, 0, sizeof(link));
	memset(frame, 0, sizeof(frame));
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	(void)vms_dlm_enq_request_build(&req, VMS_DLM_WIREOP_ENQ, frame,
					VMS_CM_FRAME_LEN, &written);
	(void)dlm_req_fsm_observe(&g_fsm, frame, VMS_CM_FRAME_LEN);
}

/* Find a 16-bit value the SHIPPING vector routes to `want` -- the scenario
 * never computes a hash from a name, it picks a wire value with a known
 * destination, which is exactly what the "hash from the wire" rung permits. */
static int hash_routing_to(vms_csid_t want, uint16_t *out)
{
	uint32_t h;

	for (h = 0; h < 65536u; h++) {
		vms_csid_t csid = 0;

		if (vms_ldwv_resolve(&g_sys[SIM_OVMX].cl.club.ldwv,
				     (uint16_t)h, &csid) != VMS_LDWV_OK)
			continue;
		if (csid == want) {
			*out = (uint16_t)h;
			return 0;
		}
	}
	return -1;
}

static void reset_wire(void)
{
	g_wire_n = 0;
	g_completions_rx = 0;
	g_commits_rx = 0;
	g_completion_master_lkid = 0;
	g_lookups_landed = 0;
	g_lookups_misaddressed = 0;
}

/* ==========================================================================
 * 1. A NOVEL ROOT NAME: refused, and NOTHING goes on the LAN
 * ========================================================================== */
static void novel_name_posts_nothing(void)
{
	uint32_t lkid = 0xdeadu, st;

	printf("--- a root name no system has ever named: refused, zero "
	       "frames ---\n");
	reset_wire();

	st = do_enq("OVMX$NEVER_SEEN", LCK_K_EXMODE, &lkid);
	ct_check_eq_u32(st, (uint32_t)SS__UNSUPPORTED,
			"$ENQ is refused SS$_UNSUPPORTED");
	ct_check_eq_u32(lkid, 0u, "no lock handle was invented");
	ct_check_eq_u32(g_wire_n, 0u,
			"*** NOT ONE FRAME went on the LAN ***");
	ct_check_eq_u32(g_fsm.hash_unknown_refused + g_fsm.lookups_sent, 0u,
			"the requester arm was never even reached: the ENGINE "
			"refused first");
}

/* ==========================================================================
 * 2. THE FULL CROSS-NODE PATH, end to end through the real chain
 * ========================================================================== */
static void cross_node_enq_resolves_and_grants(void)
{
	struct vms_getlki_args gk;
	struct vms_dlm_proxy_post db;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_frame_info fi;
	struct vms_dlm_enq_request sent;
	uint8_t opcode = 0;
	uint16_t hash = 0;
	uint32_t lkid = 0, st, flen;

	printf("--- lookup -> directory -> master -> grant -> completion "
	       "---\n");
	reset_wire();

	/* The vector routes this value to VAXB; VAXB is the directory, VAXA
	 * masters the tree. Two DIFFERENT systems, which is the case a
	 * single-node test cannot construct. */
	ct_check(hash_routing_to(g_csid[1], &hash) == 0,
		 "the shipping vector routes some wire value to VAXB");
	g_sim_master = g_csid[0];
	g_sim_master_lkid = 0x00C0FFEEu;

	wire_teaches_hash("F11B$aSYSDSK1", hash);
	ct_check_eq_u32(g_fsm.hashes_learned, 1u,
			"the hash was LEARNED from a frame, never computed");

	st = do_enq("F11B$aSYSDSK1", LCK_K_EXMODE, &lkid);
	ct_check(st == (uint32_t)SS__NORMAL && lkid != 0u,
		 "$ENQ creates a real proxy LKB and posts");

	ct_check(g_wire_n >= 2u, "at least a lookup and a request went out");
	ct_check_eq_u32(g_wire[0].dst, g_csid[1],
			"frame 1 went to the DIRECTORY node the vector named");
	ct_check_eq_u32(g_lookups_landed, 1u,
			"*** and the RECEIVER's own vector agrees it was "
			"addressed to it ***");
	ct_check_eq_u32(g_lookups_misaddressed, 0u,
			"no lookup was misaddressed");

	/* The hash on the wire is byte-for-byte the value the wire taught us. */
	flen = splice(g_wire[0].body, g_wire[0].len, frame);
	ct_check(vms_frame_classify(frame, flen, &fi) == VMS_CODEC_OK &&
		 vms_dlm_enq_request_parse(frame, flen, &fi, &opcode, &sent) ==
			 VMS_CODEC_OK,
		 "the lookup parses as a cat-02 op-01");
	ct_check_eq_u32(sent.dir_hash, hash,
			"*** body[10:12] is the value the WIRE taught us ***");
	ct_check_eq_u32(sent.req_pid_or_lkid, lkid,
			"body[20:24] is the executive's own lock handle");

	ct_check_eq_u32(g_wire[1].dst, g_csid[0],
			"frame 2 went to the MASTER the directory named");
	ct_check_eq_u32(g_fsm.redirects_followed, 1u, "outcome 2, followed");

	/* The grant landed in the REAL lock database. */
	do_getlki(lkid, &gk);
	ct_check_eq_u32(gk.status, (uint32_t)SS__NORMAL, "$GETLKI succeeds");
	ct_check_eq_u32(gk.granted_mode, (uint32_t)LCK_K_EXMODE,
			"*** the proxy is GRANTED at EX in the real engine ***");
	/*
	 * $GETLKI carries no master handle, so the master's identity is read
	 * out of the executive the same way the FSM reads it -- through
	 * vms_lock_dlm_proxy_refill_post(), the INV-6 chokepoint itself.
	 */
	memset(&db, 0, sizeof(db));
	ct_check_eq_u32(vms_lock_dlm_proxy_refill_post(lkid, VMS_DLM_POST_ENQ,
						       0u, &db),
			(uint32_t)SS__NORMAL, "the lock database re-reads");
	ct_check_eq_u32(db.master_lkid, 0x00C0FFEEu,
			"it holds the MASTER's own handle");
	ct_check_eq_u32(db.master_csid, g_csid[0], "and the master's CSID");
	ct_check_eq_u32(db.to_directory, 0u,
			"and no longer needs the directory for this tree");

	/* And the completion the master received names THAT handle -- read out
	 * of the lock database, not off the grant frame. */
	ct_check_eq_u32(g_completions_rx, 1u, "the master got the completion");
	ct_check_eq_u32(g_commits_rx, 1u, "and the commit");
	ct_check_eq_u32(g_completion_master_lkid, db.master_lkid,
			"*** and its master handle IS the one the lock "
			"database holds -- the executive's, not the frame's ***");
}

/* ==========================================================================
 * 3. Outcome 1: the directory node IS the master (p. 6-31, the common case)
 * ========================================================================== */
static void directory_is_the_master(void)
{
	struct vms_getlki_args gk;
	struct vms_dlm_proxy_post db;
	uint16_t hash = 0;
	uint32_t lkid = 0, st;

	printf("--- outcome 1: the directory node masters it, one round trip "
	       "---\n");
	reset_wire();

	ct_check(hash_routing_to(g_csid[0], &hash) == 0,
		 "the vector routes another wire value to VAXA");
	g_sim_master = g_csid[0];        /* the directory IS the master */
	g_sim_master_lkid = 0x00BEEF01u;

	wire_teaches_hash("F11B$aUSERDSK", hash);
	st = do_enq("F11B$aUSERDSK", LCK_K_PWMODE, &lkid);
	ct_check(st == (uint32_t)SS__NORMAL && lkid != 0u, "$ENQ posts");

	ct_check_eq_u32(g_wire[0].dst, g_csid[0], "one lookup, to VAXA");
	ct_check_eq_u32(g_lookups_landed, 1u, "it landed");
	do_getlki(lkid, &gk);
	ct_check_eq_u32(gk.granted_mode, (uint32_t)LCK_K_PWMODE,
			"granted at PW without a redirect");
	memset(&db, 0, sizeof(db));
	(void)vms_lock_dlm_proxy_refill_post(lkid, VMS_DLM_POST_ENQ, 0u, &db);
	ct_check_eq_u32(db.master_lkid, 0x00BEEF01u, "with the master's handle");
	ct_check_eq_u32(g_completions_rx, 1u, "and the completion went back");
	ct_check_eq_u32(g_completion_master_lkid, db.master_lkid,
			"and the completion carried the executive's value");
}

/* ==========================================================================
 * 4. A member departs while a request is outstanding
 * ========================================================================== */
static void master_departs_mid_request(void)
{
	struct vms_getlki_args gk;
	uint16_t hash = 0;
	uint32_t lkid = 0, st, n;

	printf("--- a member leaves with a request outstanding at it ---\n");
	reset_wire();

	ct_check(hash_routing_to(g_csid[1], &hash) == 0, "a value routing to VAXB");
	/* Nobody answers: the simulated master is a system that is not there. */
	g_sim_master = 0u;
	g_sim_master_lkid = 0u;

	wire_teaches_hash("F11B$aSILENT", hash);
	st = do_enq("F11B$aSILENT", LCK_K_EXMODE, &lkid);
	ct_check(st == (uint32_t)SS__NORMAL && lkid != 0u,
		 "$ENQ posts and returns a handle (async)");
	ct_check(dlm_req_fsm_find(&g_fsm, lkid) != NULL,
		 "the request is outstanding in the requester arm");

	n = dlm_req_fsm_peer_gone(&g_fsm, g_csid[1]);
	ct_check_eq_u32(n, 1u, "the departure ends exactly that request");
	ct_check(dlm_req_fsm_find(&g_fsm, lkid) == NULL,
		 "the wire record is gone");

	do_getlki(lkid, &gk);
	ct_check_eq_u32(gk.granted_mode, (uint32_t)LCK_K_NLMODE,
			"*** and the proxy was NOT granted anything ***");
}

int main(void)
{
	printf("== FC-P4.6 R2: the requester against a simulated cluster ==\n");
	sim_clock_init(&g_clock, 0);

	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return 1;
	}
	proc_init(&g_proc);
	cluster_form();
	ct_check(g_sys[SIM_OVMX].cl.club.ldwv.valid != 0u,
		 "the SHIPPING Phase 2 filled OVMX's directory vector");
	ct_check_eq_u32(g_sys[SIM_OVMX].cl.club.ldwv.n, 6u,
			"1 + 3 + 0 + 2 == six entries (Davis p. 6-32)");
	bind_everything();

	novel_name_posts_nothing();
	cross_node_enq_resolves_and_grants();
	directory_is_the_master();
	master_departs_mid_request();

	return ct_summary("dlm_requester");
}
