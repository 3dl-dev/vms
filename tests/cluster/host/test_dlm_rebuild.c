/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_rebuild.c - rd vms-874f: THE FAILOVER REBUILD, on the REAL lock
 * engine (vms_lock.c) through the REAL table (vms_dlm_rm_fsm.c) and the REAL
 * codec, against RA1b's real records.
 *
 * A tree's master LEFT the cluster (RA1b: VAX1 SHUTDOWN REMOVE_NODE). VMS
 * does not remaster a user tree at shutdown; every SURVIVING HOLDER re-sends
 * each lock it holds in the tree as an op-0x08 record, root first, to the
 * tree's directory (vms_cluster_codec_dlm.h R1..R4). This file plays OVMX in
 * BOTH of RA1b's survivor roles, with locks the shipping $ENQ and grant
 * doors made and a departure the shipping sweep ran:
 *
 *   1. AS VAX3 (the tree's DIRECTORY and a holder: RMT1 CR, S0/S1/S2 CW):
 *      VAX2's real ROOT arrives first and this node's directory answers it
 *      0xf9 -- held, but the master our proxies name has LEFT. Then this
 *      node's own rebuild addresses the master it just installed (RA1b's
 *      directory-holder case), ROOT first; VAX2's REAL answers re-home each
 *      proxy, and each sub goes out only after its parent's answer, carrying
 *      the handle VAX2 minted. Every named cell equals VAX3's real records.
 *   2. AS VAX2 (holder: RMT1 PR, S0/S1 CR; the directory is VAX3): the ROOT
 *      goes to VAX3, VAX3's REAL 0xf9 makes this node the master through the
 *      adoption engine, NO sub goes out -- and then VAX3's REAL records arrive
 *      and are installed and answered 0xfa, every named cell equal to VAX2's
 *      real answers.
 *   3. The ways a rebuild must NOT complete: a non-grant answer, a refused
 *      send, a destination that leaves, no directory yet, an incompatible
 *      survivor lock, a ROOT for a tree this node did not rebuild, a SUB with
 *      a parent this node never minted.
 *
 * The CLUB (who is a member, which node is a name's directory) is the test's,
 * as test_dlm_handover's is; everything the frames assert is read off the
 * engine.
 */
#include "cluster_test.h"
#include "conformance_dump.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_adopt.h"
#include "vms_dlm_dir.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_rm_fsm.h"

#include "ra1b_rmt1_bodies.h"

#include <stdio.h>
#include <string.h>

uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_VAX1  0x00010001u   /* RMT1's master; it LEAVES               */
#define CSID_VAX2  0x00010002u
#define CSID_VAX3  0x00010003u

#define RMT1_HASH  0xef47b8a2u   /* body[128:132] of both real ROOT records */
#define VAX1_ROOT  0x090006feu   /* RA1b-truth: VAX1's handle for VAX3's RMT1 */

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ *
 * The engine, its requester doors, and the CLUB the test plays.
 * ------------------------------------------------------------------ */
static struct vms_dlm_requester_ops eng_ops;
static struct vms_proc g_proc, g_delivery;
static uint32_t g_dir_of_names;      /* what the vector says: 0 = this node */
static int      g_ready = 1;         /* vector built, no transition         */
static uint32_t g_members[4];
static uint32_t g_n_members;

static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	(void)p;
	return SS__NORMAL;
}

static uint32_t eng_dir_resolve(void *ctx, uint32_t hash16, uint32_t *out)
{
	(void)ctx;
	(void)hash16;
	*out = CSID_VAX1;           /* before the departure: VAX1 directs it */
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return 1u;
}

static void proc_init(struct vms_proc *p, uint32_t pid)
{
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	p->vms_pid = pid;
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

/* THIS node's own directory table, the one the arm keeps. */
static struct vms_dlm_dir_entry g_dir_store[64];
static struct vms_dlm_dir g_dir;

static int engine_up(uint32_t self, uint32_t pid)
{
	vms_local_csid = self;
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return 0;
	}
	memset(&eng_ops, 0, sizeof(eng_ops));
	eng_ops.post = eng_post;
	eng_ops.dir_resolve = eng_dir_resolve;
	eng_ops.dir_generation = eng_dir_generation;
	vms_lock_dlm_set_requester_ops(&eng_ops);
	proc_init(&g_proc, pid);
	g_proc.current_mode = 3u;          /* user mode, UIC group 0 */
	proc_init(&g_delivery, 0x20200201u);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	/* The cluster's hash for RMT1, as both real ROOT records carry it. */
	(void)vms_lock_dlm_learn_dir_hash("RMT1", 4u, 0u, 3u, RMT1_HASH);
	(void)vms_dlm_dir_init(&g_dir, g_dir_store, 64u);
	g_ready = 1;
	return 1;
}

static void engine_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

static uint32_t enq(const char *name, uint32_t mode, uint32_t parid)
{
	struct vms_enq_args e;

	memset(&e, 0, sizeof(e));
	e.lkmode = mode;
	e.parid = parid;
	strscpy(e.resnam, name, sizeof(e.resnam));
	vms_ioctl_enq(&g_proc, (unsigned long)(void *)&e);
	return e.status == SS__NORMAL ? e.lkid : 0u;
}

/* VAX1 grants a lock of ours: a proxy naming VAX1 and its handle. */
static int vax1_grants(uint32_t lkid, uint32_t vax1_lkid)
{
	struct vms_dlm_proxy_grant g;

	memset(&g, 0, sizeof(g));
	g.req_lkid = lkid;
	g.master_lkid = vax1_lkid;
	g.master_csid = CSID_VAX1;
	return vms_lock_dlm_proxy_grant_recv(&g) == SS__NORMAL;
}

/* ------------------------------------------------------------------ *
 * The table's doors: the engine's, the test's CLUB, and a wire.
 * ------------------------------------------------------------------ */
#define MAX_SENT 16
static struct { uint32_t csid; uint8_t body[VMS_CM_BODY_LEN]; } g_sent[MAX_SENT];
static uint32_t g_n_sent, g_n_staged;
static uint8_t  g_staged[VMS_CM_BODY_LEN];
static int      g_send_fails;

static uint32_t d_members(void *ctx, uint32_t *c, uint32_t max)
{
	uint32_t i;

	(void)ctx;
	for (i = 0u; i < g_n_members && i < max; i++)
		c[i] = g_members[i];
	return g_n_members;
}

static int d_open(void *ctx, const struct vms_dlm_adopt_tree *t)
{
	(void)ctx;
	return vms_lock_dlm_adopt_open(t) == SS__NORMAL;
}

static int d_lock(void *ctx, const struct vms_dlm_adopt_tree *t,
		  const struct vms_dlm_adopt_lock_rec *r,
		  struct vms_dlm_adopt_lock_result *o)
{
	(void)ctx;
	return vms_lock_dlm_adopt_lock(t, r, o) == SS__NORMAL;
}

static int d_end(void *ctx, const struct vms_dlm_adopt_tree *t, uint32_t *n)
{
	(void)ctx;
	return vms_lock_dlm_adopt_end(t, n) == SS__NORMAL;
}

static int d_abort(void *ctx, const struct vms_dlm_adopt_tree *t, uint32_t *n)
{
	(void)ctx;
	return vms_lock_dlm_adopt_abort(t, n) == SS__NORMAL;
}

static int d_tree_of(void *ctx, uint32_t lk, struct vms_dlm_adopt_tree *o)
{
	(void)ctx;
	return vms_lock_dlm_adopt_tree_of_lkid(lk, o);
}

static void d_reported(void *ctx)
{
	(void)ctx;
	vms_lock_dlm_adopt_reported();
}

static int d_mastered(void *ctx, const char *n, uint32_t l, uint16_t g,
		      uint8_t m)
{
	(void)ctx;
	return vms_lock_dlm_name_mastered_here(n, l, g, m);
}

static int d_hash(void *ctx, const char *n, uint32_t l, uint16_t g, uint8_t m,
		  uint32_t *h)
{
	(void)ctx;
	return vms_lock_dlm_dir_hash_of(n, l, g, m, h);
}

/* The arm's composition: the hash the ENGINE holds, through the vector. */
static int d_dir_node(void *ctx, const char *n, uint32_t l, uint16_t g,
		      uint8_t m, uint32_t *out)
{
	uint32_t h;

	(void)ctx;
	if (!vms_lock_dlm_dir_hash_of(n, l, g, m, &h))
		return 0;
	*out = g_dir_of_names;
	return 1;
}

static int d_stage(void *ctx, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	if (len != VMS_CM_BODY_LEN)
		return -1;
	memcpy(g_staged, b, VMS_CM_BODY_LEN);
	g_n_staged++;
	return 0;
}

static int d_send(void *ctx, uint32_t csid, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	if (g_send_fails || len != VMS_CM_BODY_LEN || g_n_sent >= MAX_SENT)
		return -1;
	g_sent[g_n_sent].csid = csid;
	memcpy(g_sent[g_n_sent].body, b, VMS_CM_BODY_LEN);
	g_n_sent++;
	return 0;
}

static uint8_t d_residue(void *ctx)
{
	(void)ctx;
	return 0u;
}

static int d_valblk(void *ctx, const struct vms_dlm_adopt_valblk *v)
{
	(void)ctx;
	return vms_lock_dlm_adopt_valblk(v) == SS__NORMAL;
}

static void d_log(void *ctx, const char *msg)
{
	(void)ctx;
	printf("    [console] %s\n", msg);
}

static int d_ho_next(void *ctx, const struct vms_dlm_adopt_tree *t,
		     uint32_t nm, struct vms_dlm_handover_rec *o)
{
	(void)ctx;
	return vms_lock_dlm_handover_next(t, nm, o);
}

static int d_ho_rehome(void *ctx, uint32_t h, uint32_t o, uint32_t n,
		       uint32_t lk)
{
	(void)ctx;
	return vms_lock_dlm_handover_rehome(h, o, n, lk) == SS__NORMAL;
}

static int d_ho_valblk(void *ctx, const struct vms_dlm_adopt_valblk *v)
{
	(void)ctx;
	return vms_lock_dlm_handover_valblk(v) == SS__NORMAL;
}

static int d_rb_ready(void *ctx)
{
	(void)ctx;
	return g_ready;
}

static int d_rb_tree_next(void *ctx, uint32_t dep,
			  const struct vms_dlm_adopt_tree *skip, uint32_t n,
			  struct vms_dlm_adopt_tree *o)
{
	(void)ctx;
	return vms_lock_dlm_rebuild_tree_next(dep, skip, n, o);
}

static int d_rb_dir_entry(void *ctx, const struct vms_dlm_res_ident *id,
			  uint32_t *master)
{
	vms_csid_t m = 0u;

	(void)ctx;
	if (vms_dlm_dir_lookup_ident(&g_dir, id, &m, NULL, NULL) !=
	    VMS_DLM_DIR_IDENT_MASTER)
		return 0;
	*master = (uint32_t)m;
	return m == (vms_csid_t)vms_local_csid ? 2 : 1;
}

static int d_rb_dir_claim(void *ctx, const struct vms_dlm_res_ident *id)
{
	(void)ctx;
	return vms_dlm_dir_claim_self(&g_dir, id, vms_local_csid) == 0;
}

static int d_rb_install(void *ctx, const struct vms_dlm_adopt_lock_rec *r,
			struct vms_dlm_adopt_lock_result *o)
{
	(void)ctx;
	return vms_lock_dlm_rebuild_lock(r, o) == SS__NORMAL;
}

/*
 * THE REBUILD STEP (rd vms-bcb). Every real op-0x08 goes inside the barrier's
 * rebuild step; the executive leaves this door UNBOUND (= closed) until a
 * barrier FSM holds that step. Sections 1-3 bind it OPEN: they pin what a
 * record carries and how the table paces it once it may be sent. Section 4
 * leaves it unbound, exactly as the executive does.
 */
static int g_bind_window = 1;

static int d_rb_window(void *ctx)
{
	(void)ctx;
	return 1;
}

static struct vms_dlm_rm_ops g_ops;
static struct vms_dlm_rm g_rm;

static void table_up(uint32_t m0, uint32_t m1)
{
	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.other_members = d_members;
	g_ops.adopt_open = d_open;
	g_ops.adopt_lock = d_lock;
	g_ops.adopt_end = d_end;
	g_ops.adopt_abort = d_abort;
	g_ops.adopt_tree_of_lkid = d_tree_of;
	g_ops.adopt_reported = d_reported;
	g_ops.name_mastered_here = d_mastered;
	g_ops.dir_hash = d_hash;
	g_ops.dir_node = d_dir_node;
	g_ops.stage_reply = d_stage;
	g_ops.send = d_send;
	g_ops.residue_fill = d_residue;
	g_ops.adopt_valblk = d_valblk;
	g_ops.log = d_log;
	g_ops.ho_next = d_ho_next;
	g_ops.ho_rehome = d_ho_rehome;
	g_ops.ho_valblk = d_ho_valblk;
	g_ops.rb_ready = d_rb_ready;
	g_ops.rb_window = g_bind_window ? d_rb_window : NULL;
	g_ops.rb_tree_next = d_rb_tree_next;
	g_ops.rb_dir_entry = d_rb_dir_entry;
	g_ops.rb_dir_claim = d_rb_dir_claim;
	g_ops.rb_install = d_rb_install;
	vms_dlm_rm_init(&g_rm, &g_ops);
	/* VAX1 is already gone from the CLUB when its departure is told. */
	g_members[0] = m0;
	g_members[1] = m1;
	g_n_members = m1 != 0u ? 2u : 1u;
	g_n_sent = 0u;
	g_n_staged = 0u;
	g_send_fails = 0;
}

/* VAX1 LEAVES: the engine's sweep, then the table is told (the arm's order). */
static void vax1_departs(void)
{
	uint32_t found = 0u;

	vms_lock_dlm_member_departed(CSID_VAX1, &found);
	vms_dlm_rm_member_departed(&g_rm, CSID_VAX1);
}

/* A real 82/01 answer, its echo cell (body[24:28]) re-pointed at OUR lock --
 * the one cell that is the requester's own name, and ours differ from VAX3's
 * because THIS engine minted them. Nothing else is touched. */
static void answer_for(uint8_t *out, const uint8_t *real, uint32_t our_lkid)
{
	memcpy(out, real, VMS_CM_BODY_LEN);
	put32(out + 24, our_lkid);
}

static void dump08(const uint8_t *b, uint32_t lkid, uint32_t pid,
		   uint32_t parent, uint32_t parent_master, uint32_t hash)
{
	char st[256];

	snprintf(st, sizeof(st),
		 "lkid=0x%x,epid=0x%x,grmode=%u,rqmode=%u,parent_lkid=0x%x,"
		 "parent_master=0x%x,group=0,acmode=3,namelen=%u,hash=0x%x,"
		 "valblkseq=0x0",
		 lkid, pid, b[30], b[31], parent, parent_master, b[47], hash);
	conf_dump("dlm_rm_08", VMS_OFF_SYSAP_BODY, st, b, VMS_CM_BODY_LEN);
}

static void dump08ans(const uint8_t *a)
{
	char st[96];

	snprintf(st, sizeof(st), "lkid=0x%08x,valblkseq=0x%08x", le32(a + 20),
		 le32(a + 52));
	conf_dump("dlm_rm_08_ans", VMS_OFF_SYSAP_BODY, st, a, VMS_CM_BODY_LEN);
}

/*
 * Every cell rd vms-874f NAMES equals the real record, with OUR own handles in
 * the two cells that carry them. The value block (body[88:108]) is checked by
 * the caller: this node relays none (the deferred half, stated there).
 */
static int named_cells_equal(const uint8_t *ours, const uint8_t *real,
			     uint32_t own, uint32_t own_parent, int with_hash)
{
	if (ours[8] != real[8] || ours[9] != real[9])
		return 0;
	if (memcmp(ours + 20, real + 20, 4) != 0)      /* the PID          */
		return 0;
	if (le32(ours + 24) != own)
		return 0;
	if (ours[30] != real[30] || ours[31] != real[31])
		return 0;
	if (le32(ours + 36) != own_parent ||
	    memcmp(ours + 40, real + 40, 4) != 0)     /* the NEW-master handle */
		return 0;
	if (memcmp(ours + 44, real + 44, 4u + real[47]) != 0)
		return 0;
	if (ours[110] != real[110] || memcmp(ours + 112, real + 112, 16) != 0)
		return 0;
	if (with_hash && memcmp(ours + 128, real + 128, 4) != 0)
		return 0;
	return 1;
}

/* ================================================================== *
 * 1. AS VAX3: the directory and a holder.
 * ================================================================== */
static void test_as_vax3_the_directory_holder(void)
{
	uint32_t root, s0, s1, s2, stage_before;
	struct vms_dlm_res_ident id;
	enum vms_dlm_dir_outcome o;
	vms_csid_t m = 0u;
	uint32_t pm = 0u, i;
	uint8_t ans[VMS_CM_BODY_LEN], frame[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	const uint8_t *real_sub[3] = { ra1b_vax3_s2_req, ra1b_vax3_s1_req,
				       ra1b_vax3_s0_req };
	const uint8_t *real_sub_ans[3] = { ra1b_vax3_s2_ans, ra1b_vax3_s1_ans,
					   ra1b_vax3_s0_ans };
	uint32_t ours[3];

	printf("-- rd vms-874f: OVMX as RA1b's VAX3 -- RMT1's directory and a "
	       "holder -- after VAX1 left\n");
	if (!engine_up(CSID_VAX3, 0x20600216u))     /* VAX3's real PID */
		return;
	table_up(CSID_VAX2, 0u);

	root = enq("RMT1", LCK_K_CRMODE, 0u);
	ct_check(root != 0u && vax1_grants(root, VAX1_ROOT),
		 "RMT1 at CR, granted by VAX1 (a real proxy)");
	s0 = enq("S0", LCK_K_CWMODE, root);
	s1 = enq("S1", LCK_K_CWMODE, root);
	s2 = enq("S2", LCK_K_CWMODE, root);
	ct_check(s0 && s1 && s2 && vax1_grants(s0, 0x07000145u) &&
		 vax1_grants(s1, 0x0c0006f3u) && vax1_grants(s2, 0x0b0003ffu),
		 "S0, S1, S2 at CW under it, granted by VAX1 (RA1b-truth's "
		 "master handles)");

	g_dir_of_names = 0u;                       /* THIS node directs RMT1 */
	vax1_departs();
	/* The kick at the departure found no entry in this node's own
	 * directory: nobody has claimed RMT1, so THIS node masters it. RA1b's
	 * order is the other one -- VAX2's ROOT reached VAX3 first -- and the
	 * replay re-runs it in that order below. */
	ct_check_eq_u32(g_n_sent, 0u,
			"VAX1 left; this node directs RMT1 and nobody has claimed "
			"it: no record goes anywhere");
	ct_check_eq_u32(g_rm.rb_mastered, 1u,
			"  this node masters the tree itself");
	ct_check(vms_lock_dlm_name_mastered_here("RMT1", 4u, 0u, 3u),
		 "  the engine says so");
	{
		struct vms_dlm_res_ident id0;
		vms_csid_t m0 = 0u;

		memset(&id0, 0, sizeof(id0));
		id0.mode = 3u;
		id0.name_len = 4u;
		memcpy(id0.name, "RMT1", 4);
		ct_check(vms_dlm_dir_lookup_ident(&g_dir, &id0, &m0, NULL,
						  NULL) ==
			 VMS_DLM_DIR_IDENT_MASTER && m0 == CSID_VAX3,
			 "  and its own directory names it, so a survivor's ROOT "
			 "is served by the master, never answered 0xf9 again");
	}
	engine_down();

	/* ---- RA1b's ORDER: VAX2's ROOT reaches the directory FIRST ---- */
	if (!engine_up(CSID_VAX3, 0x20600216u))
		return;
	table_up(CSID_VAX2, 0u);
	root = enq("RMT1", LCK_K_CRMODE, 0u);
	(void)vax1_grants(root, VAX1_ROOT);
	s0 = enq("S0", LCK_K_CWMODE, root);
	s1 = enq("S1", LCK_K_CWMODE, root);
	s2 = enq("S2", LCK_K_CWMODE, root);
	(void)vax1_grants(s0, 0x07000145u);
	(void)vax1_grants(s1, 0x0c0006f3u);
	(void)vax1_grants(s2, 0x0b0003ffu);
	g_dir_of_names = 0u;
	g_ready = 0;                       /* the transition is still running */
	vax1_departs();
	ct_check_eq_u32(g_n_sent + g_rm.rb_mastered, 0u,
			"while the directory cannot be resolved NOTHING happens -- "
			"the rebuild waits for the vector");

	/* VAX2's REAL ROOT record arrives at this node, its directory. */
	ct_check(vms_dlm_res_ident_parse_body(ra1b_vax2_rmt1_req,
					      VMS_CM_BODY_LEN, &id) ==
		 VMS_CODEC_OK, "VAX2's real op-0x08 RMT1 parses");
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX2,
					     ra1b_vax2_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_NOT_MINE,
			"a ROOT for a tree this node did not rebuild is the "
			"DIRECTORY's, not the master's");
	ct_check(vms_lock_dlm_name_proxy_master("RMT1", 4u, 0u, 3u, &pm) &&
		 pm == CSID_VAX1,
		 "this node HOLDS RMT1 -- through proxies naming VAX1, which is "
		 "no member any more: the orphan the arm answers from its table");
	o = vms_dlm_dir_lookup(&g_dir, &id, CSID_VAX2, CSID_VAX3, &m);
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_YOU,
			"the directory records VAX2 as master: 'you master it'");
	ct_check(vms_dlm_dir_answer_build(ra1b_vax2_rmt1_req, VMS_CM_BODY_LEN,
					  VMS_DLM_DIR_YOU_MASTER, 0u, frame,
					  sizeof(frame), NULL) == VMS_CODEC_OK,
		 "the 0xf9 answer builds");
	{
		const uint8_t *a = frame + VMS_OFF_SYSAP_BODY;
		const uint8_t *r = ra1b_vax2_rmt1_ans;

		ct_check(a[8] == r[8] && a[9] == r[9] && a[34] == r[34] &&
			 memcmp(a + 10, r + 10, 18) == 0 &&
			 memcmp(a + 39, r + 39, 93) == 0,
			 "*** it equals VAX3's REAL 0xf9 to VAX2 on every named "
			 "cell: 82/01, the echo, body[34] = f9 ***");
	}

	/* The transition is over: the rebuild runs. */
	g_ready = 1;
	vms_dlm_rm_rb_kick(&g_rm);
	ct_check_eq_u32(g_n_sent, 1u, "ONE record goes out");
	ct_check_eq_u32(g_sent[0].csid, CSID_VAX2,
			"*** to VAX2 -- the master this node's directory just "
			"installed, not a directory lookup of itself ***");
	ct_check(g_sent[0].body[8] == 0x02 && g_sent[0].body[9] == 0x08,
		 "  a cat-0x02 op-0x08");
	ct_check(named_cells_equal(g_sent[0].body, ra1b_vax3_rmt1_req, root,
				   0u, 1),
		 "*** EVERY NAMED CELL equals VAX3's real ROOT record: PID "
		 "0x20600216, own lkid, CR/CR, no parent, the identity, body[110] "
		 "and the tail, the tree hash ***");
	ct_check(memcmp(g_sent[0].body + 88, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
			"\0\0\0\0", 20) == 0,
		 "  body[88:108] is twenty zeros: this node relays NO value block "
		 "(it holds no sequence for one -- the deferred half; VAX3's "
		 "carries RMLVB-0123456789 at 1)");
	ct_check(vms_dlm_shape_fit_for_any_member(g_sent[0].body,
						  VMS_CM_BODY_LEN),
		 "  the connection manager would send it to a real VAX");
	ct_check(!vms_dlm_body_is_notification(g_sent[0].body, VMS_CM_BODY_LEN),
		 "  ...opening a transaction for its answer");
	dump08(g_sent[0].body, root, g_proc.vms_pid, 0u, 0u, RMT1_HASH);

	/* VAX2's REAL 0xfa for VAX3's root. */
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX2, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1,
		 "VAX2's real 82/01 for the root is consumed");
	ct_check_eq_u32(g_rm.rb_answers, 1u, "  re-homed, counted");
	ct_check_eq_u32(g_n_sent, 2u, "and the next record goes out");

	stage_before = g_n_sent;
	for (i = 0u; i < 3u; i++) {
		const uint8_t *b = g_sent[g_n_sent - 1u].body;
		uint32_t own = le32(b + 24);
		uint32_t k;

		ours[i] = own;
		k = (own == s2) ? 0u : (own == s1) ? 1u : 2u;
		ct_check(b[9] == 0x08 && le32(b + 36) == root,
			 "  a SUB record, body[36:40] our own root lkid");
		ct_check_eq_u32(le32(b + 40), 0x01000563u,
				"  *** body[40:44] = 0x01000563, the handle VAX2's "
				"REAL answer minted for the root ***");
		ct_check(named_cells_equal(b, real_sub[k], own, root, 0),
			 "*** EVERY NAMED CELL equals VAX3's real SUB record "
			 "(PID, CW/CW, the parent pair, identity, tail) ***");
		ct_check(memcmp(b + 128, "\0\0\0\0", 4) == 0,
			 "  body[128:132] zero: a sub's value is not computable "
			 "(Rule 8), never minted");
		dump08(b, own, g_proc.vms_pid, root, 0x01000563u, 0u);
		answer_for(ans, real_sub_ans[k], own);
		ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX2, 0x82, 0x01,
					      ans, VMS_CM_BODY_LEN) == 1,
			 "  VAX2's real 82/01 for it is consumed");
	}
	(void)stage_before;
	ct_check(ours[0] != ours[1] && ours[1] != ours[2] && ours[0] != ours[2],
		 "three distinct sub-locks were described, one each");
	ct_check_eq_u32(g_n_sent, 4u,
			"four records in all: the root and three subs -- one per "
			"lock, as VAX3 sent");
	ct_check_eq_u32(g_rm.rb_answers, 4u, "four answers re-homed");
	ct_check_eq_u32(g_rm.rb_trees_done, 1u, "*** the tree is done ***");
	ct_check_eq_u32(vms_dlm_rm_rb_stopped(&g_rm), 0u, "nothing stopped");
	ct_check_eq_u32(vms_dlm_rm_rb_in_state(&g_rm, VMS_DLM_RB_ST_AWAIT), 0u,
			"no slot still waiting");
	{
		struct vms_dlm_adopt_tree t;

		ct_check(vms_lock_dlm_rebuild_tree_next(CSID_VAX1, NULL, 0u,
							&t) == 0,
			 "*** no lock of ours names VAX1 any more: every one "
			 "survived, now held at VAX2 ***");
	}
	engine_down();
}

/* ================================================================== *
 * 2. AS VAX2: told 0xf9 -- the master -- and then VAX3's records.
 * ================================================================== */
static void test_as_vax2_told_to_master(void)
{
	uint32_t root, s0, s1, vax3_root_handle = 0u, i;
	uint8_t ans[VMS_CM_BODY_LEN], rec[VMS_CM_BODY_LEN];
	struct vms_dlm_handover_rec hr;
	struct vms_dlm_adopt_tree t;
	const uint8_t *real_sub[3] = { ra1b_vax3_s2_req, ra1b_vax3_s1_req,
				       ra1b_vax3_s0_req };
	const uint8_t *real_sub_ans[3] = { ra1b_vax3_s2_ans, ra1b_vax3_s1_ans,
					   ra1b_vax3_s0_ans };

	printf("-- rd vms-874f: OVMX as RA1b's VAX2 -- told 0xf9 by the "
	       "directory, then VAX3's records arrive\n");
	if (!engine_up(CSID_VAX2, 0x20400217u))     /* VAX2's real PID form */
		return;
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	ct_check(root != 0u && vax1_grants(root, 0x05000024u),
		 "RMT1 at PR, granted by VAX1");
	s0 = enq("S0", LCK_K_CRMODE, root);
	s1 = enq("S1", LCK_K_CRMODE, root);
	ct_check(s0 && s1 &&
		 vax1_grants(s0, 0x0600070au) && vax1_grants(s1, 0x07000069u),
		 "RMT1 PR, S0/S1 CR -- RA1b-truth's VAX2 locks, granted by VAX1");
	g_dir_of_names = CSID_VAX3;                  /* VAX3 directs RMT1 */
	vax1_departs();
	ct_check_eq_u32(g_n_sent, 1u, "VAX1 left: ONE record goes out");
	ct_check_eq_u32(g_sent[0].csid, CSID_VAX3,
			"*** to the tree's DIRECTORY, VAX3 (R2) ***");
	ct_check(named_cells_equal(g_sent[0].body, ra1b_vax2_rmt1_req, root,
				   0u, 1),
		 "*** EVERY NAMED CELL equals VAX2's real ROOT record: PID "
		 "0x20400217, own lkid, PR/PR, no parent, identity, tail, "
		 "the tree hash ***");
	dump08(g_sent[0].body, root, g_proc.vms_pid, 0u, 0u, RMT1_HASH);

	/* VAX3's REAL 0xf9. */
	answer_for(ans, ra1b_vax2_rmt1_ans, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1,
		 "VAX3's real 0xf9 is consumed");
	ct_check_eq_u32(g_rm.rb_mastered, 1u,
			"*** THIS node now masters RMT1, through the adoption "
			"engine ***");
	ct_check(vms_lock_dlm_name_mastered_here("RMT1", 4u, 0u, 3u),
		 "  the engine says so");
	memset(&t, 0, sizeof(t));
	memcpy(t.resnam, "RMT1", 4);
	t.resnam_len = 4u;
	t.res_mode = 3u;
	t.ident_valid = 1u;
	t.old_master_csid = CSID_VAX1;
	ct_check(vms_lock_dlm_handover_next(&t, CSID_VAX3, &hr) == 0,
		 "  every proxy of ours in the tree is a LOCAL lock now -- "
		 "nothing left naming VAX1");
	ct_check_eq_u32(g_n_sent, 1u,
			"*** and NO sub-record goes out: a 0xf9 holder sends none "
			"(VAX2's S0/S1 in RA1b) ***");

	/* VAX3's REAL ROOT record arrives at its new master. */
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_HANDLED,
			"VAX3's real ROOT record is installed and answered");
	vax3_root_handle = le32(g_staged + 20);
	ct_check(vax3_root_handle != 0u, "  body[20:24]: a handle THIS engine "
		 "minted");
	{
		const uint8_t *a = g_staged, *r = ra1b_vax3_rmt1_ans;

		ct_check(a[8] == 0x82 && a[9] == 0x01 && a[34] == 0xfa &&
			 a[28] == r[28] && a[29] == r[29] &&
			 memcmp(a + 24, r + 24, 4) == 0 &&
			 memcmp(a + 36, r + 36, 20) == 0 &&
			 memcmp(a + 56, r + 56, 76) == 0 &&
			 memcmp(a + 10, r + 10, 10) == 0,
			 "*** it equals VAX2's REAL 0xfa on every named cell: "
			 "82/01, VAX3's lkid echoed, body[28]=0x10, 0xfa, and "
			 "RMLVB-0123456789 at sequence 1 read back off the RSB "
			 "the record wrote ***");
		dump08ans(a);
	}
	for (i = 0u; i < 3u; i++) {
		/* VAX3's real SUB records name the parent by VAX2's REAL
		 * handle; this engine minted its own, so that one cell is
		 * re-pointed at it -- the cell the record exists to carry. */
		memcpy(rec, real_sub[i], VMS_CM_BODY_LEN);
		put32(rec + 40, vax3_root_handle);
		ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
						     VMS_CM_BODY_LEN),
				VMS_DLM_RM_HANDLED,
				"VAX3's real SUB record is installed under its "
				"root and answered");
		ct_check(g_staged[34] == 0xfa && le32(g_staged + 20) != 0u &&
			 memcmp(g_staged + 24, real_sub_ans[i] + 24, 4) == 0 &&
			 g_staged[28] == real_sub_ans[i][28] &&
			 memcmp(g_staged + 56, real_sub_ans[i] + 56, 76) == 0,
			 "  *** its 0xfa equals VAX2's real one: the echo, "
			 "body[28]=0x10, the tail ***");
		dump08ans(g_staged);
	}
	ct_check_eq_u32(vms_lock_dlm_rebuild_installed(), 4u,
			"*** four of VAX3's locks exist here now, as master-side "
			"locks held for VAX3 ***");
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_HANDLED,
			"a retransmitted ROOT is answered again...");
	ct_check_eq_u32(le32(g_staged + 20), vax3_root_handle,
			"  ...with the SAME handle (no second lock)");
	ct_check_eq_u32(vms_lock_dlm_rebuild_installed(), 4u,
			"  and nothing new installed");
	engine_down();
}

/* ================================================================== *
 * 3. The ways a rebuild must NOT complete.
 * ================================================================== */
static void setup_vax2_role(void)
{
	uint32_t root, s0;

	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	s0 = enq("S0", LCK_K_CRMODE, root);
	(void)vax1_grants(s0, 0x0600070au);
	g_dir_of_names = CSID_VAX3;
}

static void test_what_stops_a_rebuild(void)
{
	uint8_t ans[VMS_CM_BODY_LEN], rec[VMS_CM_BODY_LEN];
	struct vms_dlm_adopt_tree t;
	uint32_t root;

	printf("-- rd vms-874f: the ways a rebuild must NOT complete\n");

	/* A non-grant answer. */
	setup_vax2_role();
	vax1_departs();
	root = le32(g_sent[0].body + 24);
	answer_for(ans, ra1b_vax3_s0_ans, root);
	ans[34] = 0xfb;                                    /* not a grant */
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1,
		 "a non-grant answer to our record is consumed");
	ct_check_eq_u32(g_rm.rb_answers_refused, 1u, "  ...counted");
	ct_check_eq_u32(g_n_sent, 1u,
			"*** nothing re-homed and nothing more sent: never a claim "
			"to hold less than this node holds ***");
	ct_check(g_rm.rb_trees_done == 0u && g_rm.rb_trees_started == 1u &&
		 vms_dlm_rm_rb_stopped(&g_rm) >= 1u,
		 "  the tree is STOPPED, counted, and not retried");
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 0,
		 "  a late grant for a stopped tree is not ours any more");
	engine_down();

	/* An answer from the wrong system, or for the wrong lock. */
	setup_vax2_role();
	vax1_departs();
	root = le32(g_sent[0].body + 24);
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX1, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 0,
		 "an answer from a system the record did not go to is not ours");
	answer_for(ans, ra1b_vax3_rmt1_ans, root + 1u);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 0,
		 "...nor one naming another lock");
	ct_check_eq_u32(g_rm.rb_answers, 0u, "  nothing re-homed");
	/* ...and its destination leaves mid-rebuild. */
	vms_dlm_rm_member_departed(&g_rm, CSID_VAX3);
	ct_check_eq_u32(g_rm.rb_dst_departed, 1u,
			"the destination left: the tree is stopped, counted");
	engine_down();

	/* A refused send. */
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	g_dir_of_names = CSID_VAX3;
	g_send_fails = 1;
	vax1_departs();
	ct_check_eq_u32(g_rm.rb_records_unsent, 1u,
			"a refused send stops the tree, counted");
	ct_check(vms_dlm_rm_rb_stopped(&g_rm) >= 1u, "  in the stopped total");
	engine_down();

	/* A directory that is no member. */
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	g_dir_of_names = 0x00010009u;
	vax1_departs();
	ct_check_eq_u32(g_n_sent, 0u,
			"a directory that is not a member is never addressed");
	ct_check_eq_u32(g_rm.rb_unaddressable, 1u, "  counted");
	engine_down();

	/* Nothing of ours names the departed master. */
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	g_dir_of_names = CSID_VAX3;
	vax1_departs();
	ct_check_eq_u32(g_n_sent + g_rm.rb_trees_started, 0u,
			"a node holding nothing in a departed master's trees "
			"sends nothing");
	engine_down();

	/* As master: a ROOT for a tree NOT rebuilt here, a SUB with a parent
	 * this node never minted, a non-member, an incompatible lock. */
	setup_vax2_role();
	vax1_departs();
	root = le32(g_sent[0].body + 24);
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_NOT_MINE,
			"before the 0xf9 a survivor's ROOT is not the master's");
	answer_for(ans, ra1b_vax2_rmt1_ans, root);
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	memcpy(rec, ra1b_vax3_s2_req, VMS_CM_BODY_LEN);
	put32(rec + 40, 0x7fff0001u);         /* a handle nobody minted here */
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_REFUSED,
			"a SUB naming a parent handle this engine never minted is "
			"REFUSED, not answered");
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, 0x00010009u,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_REFUSED,
			"a ROOT from a system that is no member is REFUSED");
	memcpy(rec, ra1b_vax3_rmt1_req, VMS_CM_BODY_LEN);
	rec[30] = rec[31] = (uint8_t)LCK_K_EXMODE;      /* EX over our PR */
	put32(rec + 24, 0x0e0004ffu);
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_REFUSED,
			"*** an EX record over the PR this node holds is NOT "
			"installed: never a second incompatible grant ***");
	ct_check_eq_u32(vms_lock_dlm_rebuild_installed(), 0u,
			"  nothing was installed by any of them");
	/* A new transition ends the rebuild's authority over that tree. */
	vms_dlm_rm_rb_transition_begin(&g_rm);
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_NOT_MINE,
			"after the next transition a ROOT is the directory's again");
	memset(&t, 0, sizeof(t));
	ct_check(vms_lock_dlm_rebuild_tree_next(0u, NULL, 0u, &t) == 0,
		 "no departed master names no tree");
	engine_down();

	/* The doors unbound: a departure is recorded, nothing is sent. */
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	g_ops.rb_install = NULL;
	vax1_departs();
	ct_check_eq_u32(g_n_sent, 0u, "with the rebuild doors unbound nothing "
			"is sent");
	ct_check_eq_u32(g_rm.rb_unbound, 1u, "  counted");
	engine_down();
}

/* The 0x08 builder writes the 0x12 builder's cells, with op 0x08, and the
 * new peer-receive decoder reads what it wrote. */
static void test_codec_round_trip(void)
{
	struct vms_dlm_rm_lock_rec r, back;
	uint8_t f12[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	uint8_t f08[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];

	printf("-- rd vms-874f: the op-0x08 record is the op-0x12 record with "
	       "op 0x08\n");
	ct_check(vms_dlm_rb_rec_parse_body(ra1b_vax3_s2_req, VMS_CM_BODY_LEN,
					   &r) == VMS_CODEC_OK,
		 "VAX3's real S2 record parses");
	ct_check(!r.is_root && r.holder_lkid == 0x010004a6u &&
		 r.parent_holder_lkid == 0x0e00049bu &&
		 r.parent_master_lkid == 0x01000563u &&
		 r.granted_mode == 2u && r.requested_mode == 2u,
		 "  a SUB: own lkid, parent pair, CW/CW -- RA1b-truth's");
	ct_check(vms_dlm_rm_lock_rec_parse_body(ra1b_vax3_s2_req,
						VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_E_CLASS,
		 "  the op-0x12 parser refuses it (the opcode is the class)");
	ct_check(vms_dlm_rb_rec_build(&r, 0, f08, sizeof(f08), NULL) ==
		 VMS_CODEC_OK &&
		 vms_dlm_rm_lock_rec_build(&r, 0, f12, sizeof(f12), NULL) ==
		 VMS_CODEC_OK, "both builders take it");
	ct_check(f08[VMS_OFF_SYSAP_BODY + 9] == 0x08 &&
		 f12[VMS_OFF_SYSAP_BODY + 9] == 0x12 &&
		 memcmp(f08 + VMS_OFF_SYSAP_BODY, f12 + VMS_OFF_SYSAP_BODY, 9) ==
		 0 &&
		 memcmp(f08 + VMS_OFF_SYSAP_BODY + 10,
			f12 + VMS_OFF_SYSAP_BODY + 10, VMS_CM_BODY_LEN - 10) == 0,
		 "*** they differ in body[9] and nowhere else ***");
	ct_check(vms_dlm_rb_rec_parse_body(f08 + VMS_OFF_SYSAP_BODY,
					   VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_OK && back.holder_lkid == r.holder_lkid &&
		 back.parent_master_lkid == r.parent_master_lkid,
		 "  and ours reads back");
	r.parent_master_lkid = 0u;
	ct_check(vms_dlm_rb_rec_build(&r, 0, f08, sizeof(f08), NULL) ==
		 VMS_CODEC_E_INVAL, "a HALF parent pair is never built");
}

/* ================================================================== *
 * 4. rd vms-bcb: LAB33A, REPLAYED. VAX1 (LOCKDIRWT 1) masters DEPT; OVMXE
 *    (CSID 00010003, LOCKDIRWT 0) holds DEPT's root at CR with no subs; VAX2
 *    (LOCKDIRWT 1) is DEPT's directory and, once VAX1 leaves, the only other
 *    member. VAX1 SHUTDOWN REMOVE_NODE. The old sender put ONE op-0x08 ROOT
 *    on the wire to VAX2 at the transition's end -- after the barrier's
 *    release of step 12 -- and VAX2 bugchecked LOCKMGRERR (pcap frame 7801).
 *    Every real op-0x08 is sent after the release of step 5 or 6 and answered
 *    before the next release (887/887, dlm_rebuild08_census.py section 8).
 * ================================================================== */
#define CSID_OVMXE 0x00010003u

static void test_lab33a_no_record_outside_the_rebuild_step(void)
{
	uint32_t root;

	printf("-- rd vms-bcb: lab33a replayed -- no op-0x08 outside the "
	       "rebuild step\n");
	g_bind_window = 0;                 /* the executive's binding */

	/* OVMXE holds DEPT, mastered by VAX1; VAX2 is DEPT's directory. */
	(void)engine_up(CSID_OVMXE, 0x10000005u);
	table_up(CSID_VAX2, 0u);
	root = enq("DEPT", LCK_K_CRMODE, 0u);
	ct_check(root != 0u && vax1_grants(root, 0x0b000439u),
		 "OVMXE's DEPT root at CR is a proxy naming VAX1");
	g_dir_of_names = CSID_VAX2;
	vax1_departs();                    /* the transition's end kicks it */
	vms_dlm_rm_rb_kick(&g_rm);         /* ...and so does the beat       */
	ct_check_eq_u32(g_n_sent, 0u,
			"*** NO op-0x08 goes to VAX2 after the barrier (the old "
			"sender's frame 7801 bugchecked it) ***");
	ct_check_eq_u32(g_rm.rb_records_sent, 0u, "  nothing counted as sent");
	ct_check_eq_u32(g_rm.rb_out_of_window, 1u,
			"  the tree is stopped for the step it missed, counted");
	ct_check_eq_u32(g_rm.rb_trees_started, 1u,
			"  ...and not retried by the next kick");
	ct_check(vms_dlm_rm_rb_stopped(&g_rm) >= 1u, "  in the stopped total");
	engine_down();

	/* The same, with OVMXE as DEPT's directory and its own entry naming a
	 * surviving master (RA1b's directory-holder case): still no record. */
	(void)engine_up(CSID_OVMXE, 0x10000005u);
	table_up(CSID_VAX2, 0u);
	root = enq("DEPT", LCK_K_CRMODE, 0u);
	(void)vax1_grants(root, 0x0b000439u);
	g_dir_of_names = 0u;
	{
		struct vms_dlm_res_ident id;
		uint32_t h = 0u;

		memset(&id, 0, sizeof(id));
		id.mode = 3u;
		id.name_len = 4u;
		memcpy(id.name, "DEPT", 4u);
		if (vms_lock_dlm_dir_hash_of("DEPT", 4u, 0u, 3u, &h))
			id.hash = h;
		ct_check(vms_dlm_dir_claim_self(&g_dir, &id, CSID_VAX2) == 0,
			 "OVMXE's own directory names VAX2 DEPT's master");
	}
	vax1_departs();
	ct_check_eq_u32(g_n_sent, 0u,
			"*** the directory-holder sends none outside the step "
			"either ***");
	ct_check_eq_u32(g_rm.rb_out_of_window, 1u, "  counted");
	engine_down();

	g_bind_window = 1;
}

int main(void)
{
	printf("test_dlm_rebuild: the failover rebuild, on the real lock "
	       "engine and RA1b's real records (rd vms-874f)\n");
	test_codec_round_trip();
	test_as_vax3_the_directory_holder();
	test_as_vax2_told_to_master();
	test_what_stops_a_rebuild();
	test_lab33a_no_record_outside_the_rebuild_step();
	return ct_summary("test_dlm_rebuild");
}
