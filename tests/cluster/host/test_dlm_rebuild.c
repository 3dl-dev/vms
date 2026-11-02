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

/*
 * VAX1 grants a lock of ours: a proxy naming VAX1 and its handle -- and, as
 * every real grant does (rd vms-b5b0: 38 of 38), the grant record carrying the
 * resource's value block at body[36:52] and its SEQUENCE at body[52:56]. A
 * resource nobody wrote has block zero, sequence zero.
 */
static int vax1_grants_vb(uint32_t lkid, uint32_t vax1_lkid,
			  const char *valblk, uint32_t seq)
{
	struct vms_dlm_proxy_grant g;

	memset(&g, 0, sizeof(g));
	g.req_lkid = lkid;
	g.master_lkid = vax1_lkid;
	g.master_csid = CSID_VAX1;
	g.valblk_present = 1u;
	if (valblk != NULL)
		memcpy(g.valblk, valblk, VMS_DLM_VALBLK_LEN);
	g.valblk_seq = seq;
	return vms_lock_dlm_proxy_grant_recv(&g) == SS__NORMAL;
}

static int vax1_grants(uint32_t lkid, uint32_t vax1_lkid)
{
	return vax1_grants_vb(lkid, vax1_lkid, NULL, 0u);
}

/* RA1b-truth.txt: VAX1's RMT1 LVB "RMLVB-0123456789", sequence 1 -- what its
 * grants to VAX2 (PR) and VAX3 (CR), both taken LCK$M_VALBLK by RMHOLD.MAR
 * after VAX1's own write, delivered. */
#define RMT1_LVB      "RMLVB-0123456789"
#define RMT1_LVB_SEQ  1u

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

/* The arm's rb_ready (vms_dlm_scs.c dlm_arm_rb_ready, source-scanned by
 * test_dlm_scs_arm): the vector is built, and either no transition is
 * running or the rebuild window is open. */
static int g_in_transition;
static struct vms_dlm_rm g_rm;

static int d_rb_ready(void *ctx)
{
	(void)ctx;
	return g_ready &&
	       (!g_in_transition || vms_dlm_rm_rb_window_open(&g_rm));
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
 * THE REBUILD WINDOW (rd vms-bcb, vms-2fa). Every real op-0x08 goes out after
 * the barrier release of step R (5 for a weighted sender, 6 for a weight-0
 * one) and before the sender's next barrier frame; the barrier FSMs tell the
 * table each release (vms_dlm_rm_rb_barrier_released). Sections 1-3 open it
 * as a weight-0 node's release 6 does, before the departure: they pin what a
 * record carries and how the table paces it once it may be sent. Section 5
 * never opens it (the record lab33a sent after release 12); section 7 drives
 * the releases one by one.
 */
static int g_bind_window = 1;

static struct vms_dlm_rm_ops g_ops;

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
	g_ops.rb_tree_next = d_rb_tree_next;
	g_ops.rb_dir_entry = d_rb_dir_entry;
	g_ops.rb_dir_claim = d_rb_dir_claim;
	g_ops.rb_install = d_rb_install;
	vms_dlm_rm_init(&g_rm, &g_ops);
	if (g_bind_window)
		vms_dlm_rm_rb_barrier_released(&g_rm,
					       vms_dlm_rb_release_step(0u), 0u,
					       0u);
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

/*
 * THE RECORD'S STATE, READ OFF THE LKB -- never off the frame (rd vms-1b68):
 * the gate compares body[30]/[31]/[28]/[110] with these, so a builder that
 * swapped the modes or mis-stated the queue goes red. `queue` is the wire
 * value of the queue the LKB is on (01 granted, 00 converting, ff waiting).
 */
static const struct vms_lock_entry *lkb_of(struct vms_proc *p, uint32_t lkid)
{
	struct vms_lock_entry *l;

	exec_list_for_each_entry(l, &p->locks, proc_list) {
		if (l->lkid == lkid)
			return l;
	}
	return NULL;
}

static void lkb_rec_state(const struct vms_lock_entry *l, char *st, size_t n)
{
	unsigned q, f = 0u;

	if (l == NULL) {
		snprintf(st, n, "nolkb=1");
		return;
	}
	q = !l->waiting ? 0x01u : ((l->flags & LCK_M_CONVERT) ? 0x00u : 0xffu);
	if (l->flags & LCK_M_VALBLK)
		f |= 0x01u;
	if (l->flags & LCK_M_CONVERT)
		f |= 0x02u;
	if (l->flags & LCK_M_SYSTEM)
		f |= 0x10u;
	snprintf(st, n, "grmode=%u,rqmode=%u,queue=0x%02x,recflags=0x%x",
		 (unsigned)l->granted_mode, (unsigned)l->requested_mode, q, f);
}

static void dump08(const uint8_t *b, uint32_t lkid, uint32_t pid,
		   uint32_t parent, uint32_t parent_master, uint32_t hash,
		   uint32_t valblkseq)
{
	char st[320], ls[128];

	lkb_rec_state(lkb_of(&g_proc, lkid), ls, sizeof(ls));
	snprintf(st, sizeof(st),
		 "lkid=0x%x,epid=0x%x,%s,parent_lkid=0x%x,"
		 "parent_master=0x%x,group=0,acmode=3,namelen=%u,hash=0x%x,"
		 "valblkseq=0x%x",
		 lkid, pid, ls, parent, parent_master, b[47], hash,
		 valblkseq);
	conf_dump("dlm_rm_08", VMS_OFF_SYSAP_BODY, st, b, VMS_CM_BODY_LEN);
}

/* The answer's state off the LKB THIS engine installed for the record (its
 * handle is body[20:24]): the queue it put the lock on, and its modes. */
static void dump08ans(const uint8_t *a)
{
	char st[256], ls[128];

	lkb_rec_state(lkb_of(&g_delivery, le32(a + 20)), ls, sizeof(ls));
	snprintf(st, sizeof(st), "lkid=0x%08x,valblkseq=0x%08x,%s",
		 le32(a + 20), le32(a + 52), ls);
	conf_dump("dlm_rm_08_ans", VMS_OFF_SYSAP_BODY, st, a, VMS_CM_BODY_LEN);
}

/*
 * Every cell rd vms-874f NAMES equals the real record, with OUR own handles in
 * the two cells that carry them. The value block (body[88:108]) is checked by
 * the caller (rd vms-8159).
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
	ct_check(root != 0u && vax1_grants_vb(root, VAX1_ROOT, RMT1_LVB,
					      RMT1_LVB_SEQ),
		 "RMT1 at CR, granted by VAX1 (a real proxy) with its block, "
		 "RMLVB-0123456789 at sequence 1");
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
	(void)vax1_grants_vb(root, VAX1_ROOT, RMT1_LVB, RMT1_LVB_SEQ);
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
	ct_check(memcmp(g_sent[0].body + 88, ra1b_vax3_rmt1_req + 88, 20) == 0,
		 "*** body[88:108] equals VAX3's real ROOT record: the block "
		 "VAX1 last GRANTED this lock, RMLVB-0123456789, at its sequence "
		 "1 -- read off the proxy LKB after the master left (rd "
		 "vms-8159) ***");
	ct_check(vms_dlm_shape_fit_for_any_member(g_sent[0].body,
						  VMS_CM_BODY_LEN),
		 "  the connection manager would send it to a real VAX");
	ct_check(!vms_dlm_body_is_notification(g_sent[0].body, VMS_CM_BODY_LEN),
		 "  ...opening a transaction for its answer");
	dump08(g_sent[0].body, root, g_proc.vms_pid, 0u, 0u, RMT1_HASH,
	       RMT1_LVB_SEQ);

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
		ct_check(memcmp(b + 88, real_sub[k] + 88, 20) == 0,
			 "  body[88:108] twenty zeros, as VAX3's real SUB: VAX1 "
			 "granted it a block of zeros at sequence 0, the absent "
			 "form");
		dump08(b, own, g_proc.vms_pid, root, 0x01000563u, 0u, 0u);
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
	ct_check(root != 0u && vax1_grants_vb(root, 0x05000024u, RMT1_LVB,
					      RMT1_LVB_SEQ),
		 "RMT1 at PR, granted by VAX1 with RMLVB-0123456789 at 1");
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
	ct_check(memcmp(g_sent[0].body + 88, ra1b_vax2_rmt1_req + 88, 20) == 0,
		 "*** body[88:108] equals VAX2's real ROOT record: "
		 "RMLVB-0123456789 at 1, the pair VAX1 granted (rd vms-8159) ***");
	dump08(g_sent[0].body, root, g_proc.vms_pid, 0u, 0u, RMT1_HASH,
	       RMT1_LVB_SEQ);

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
	g_bind_window = 0;                 /* no release R: the window shut */

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

/* ================================================================== *
 * 6. WHICH PAIR a holder's record relays (rd vms-8159): the master's last
 *    word for the lock -- its grant, or an op-0x1a that supersedes it --
 *    and NONE once this node's own write replaced the block.
 * ================================================================== */
static int record_pair(const struct vms_dlm_adopt_tree *t, uint32_t lkid,
		       const char *want, uint32_t want_seq)
{
	struct vms_dlm_handover_rec hr;

	memset(&hr, 0, sizeof(hr));
	if (vms_lock_dlm_handover_next(t, CSID_VAX2, &hr) != 1 ||
	    hr.holder_lkid != lkid)
		return 0;
	if (want == NULL)
		return hr.valblk_present == 0u && hr.valblk_seq == 0u;
	return hr.valblk_present == 1u && hr.valblk_seq == want_seq &&
	       memcmp(hr.valblk, want, VMS_DLM_VALBLK_LEN) == 0;
}

static void test_which_pair_the_record_relays(void)
{
	struct vms_dlm_adopt_tree t;
	struct vms_dlm_adopt_valblk v;
	struct vms_enq_args e;
	uint32_t lk;

	printf("-- rd vms-8159: the value block a holder's record relays\n");
	if (!engine_up(CSID_VAX3, 0x20600216u))
		return;
	memset(&t, 0, sizeof(t));
	memcpy(t.resnam, "RMT1", 4);
	t.resnam_len = 4u;
	t.res_mode = 3u;
	t.ident_valid = 1u;
	t.old_master_csid = CSID_VAX1;

	memset(&e, 0, sizeof(e));
	e.lkmode = LCK_K_EXMODE;
	e.flags = LCK_M_VALBLK;
	strscpy(e.resnam, "RMT1", sizeof(e.resnam));
	vms_ioctl_enq(&g_proc, (unsigned long)(void *)&e);
	lk = e.status == SS__NORMAL ? e.lkid : 0u;
	ct_check(lk != 0u && vax1_grants_vb(lk, VAX1_ROOT, "GRANTED-BLOCK-01",
					    0x151u),
		 "RMT1 at EX with LCK$M_VALBLK, granted by VAX1 with a block at "
		 "sequence 0x151");
	ct_check(record_pair(&t, lk, "GRANTED-BLOCK-01", 0x151u),
		 "*** the record carries the GRANT's block and sequence ***");

	memset(&v, 0, sizeof(v));
	v.from_csid = CSID_VAX1;
	v.master_lkid = VAX1_ROOT;
	v.holder_lkid = lk;
	v.valblk_present = 1u;
	v.valblk_seq = 0x152u;
	memcpy(v.valblk, "OP1A-SAYS-THIS-1", VMS_DLM_VALBLK_LEN);
	ct_check_eq_u32(vms_lock_dlm_handover_valblk(&v), SS__NORMAL,
			"VAX1's op-0x1a for the lock lands");
	ct_check(record_pair(&t, lk, "OP1A-SAYS-THIS-1", 0x152u),
		 "*** an op-0x1a SUPERSEDES the grant (251 of 259 real "
		 "op-0x12s after one relay it) ***");
	v.valblk_present = 0u;
	v.valblk_seq = 0u;
	memset(v.valblk, 0, sizeof(v.valblk));
	(void)vms_lock_dlm_handover_valblk(&v);
	ct_check(record_pair(&t, lk, NULL, 0u),
		 "  an op-0x1a carrying none leaves the record the absent form "
		 "(20 of 20 real records after a zero op-0x1a)");

	ct_check(vax1_grants_vb(lk, VAX1_ROOT, "GRANTED-BLOCK-02", 0x160u) &&
		 record_pair(&t, lk, "GRANTED-BLOCK-02", 0x160u),
		 "  a later grant is the master's word again");

	/* THIS node writes the block: EX -> NL with LCK$M_VALBLK. */
	memset(&e, 0, sizeof(e));
	e.lkid = lk;
	e.lkmode = LCK_K_NLMODE;
	e.flags = LCK_M_CONVERT | LCK_M_VALBLK;
	memcpy(e.valblk, "WRITTEN-BY-OVMX!", VMS_DLM_VALBLK_LEN);
	(void)vms_ioctl_convert(&g_proc, (unsigned long)(void *)&e);
	ct_check_eq_u32(e.status, SS__NORMAL, "the down-conversion writing the "
			"block goes to the master");
	{
		struct vms_dlm_proxy_grant g;

		memset(&g, 0, sizeof(g));
		g.req_lkid = lk;
		g.master_lkid = VAX1_ROOT;
		g.master_csid = CSID_VAX1;     /* a grant with no block record */
		(void)vms_lock_dlm_proxy_grant_recv(&g);
	}
	ct_check(record_pair(&t, lk, NULL, 0u),
		 "*** after this node's own WRITE the record carries NO block: "
		 "the granted sequence no longer describes the bytes, and the "
		 "sequence a write advances to is rd vms-85c1's -- never a stale "
		 "sequence beside new bytes ***");
	engine_down();
}


/* ================================================================== *
 * 7. THE WINDOW RIDES THE BARRIER (rd vms-2fa; tests/lab/captures/
 *    vms-2fa-rebuild-step-20261102). The releases arrive one at a time,
 *    as the barrier FSMs tell them; the record must go out on release R
 *    and on no other -- R = 6 for a weight-0 sender (arm 3b, lab33a's
 *    role: frames 21 release 6, 22 the ROOT, 23 the 0xf9, 24 step 7) and
 *    5 for a weighted one (arm 2: 796 of 796 after release 5).
 * ================================================================== */
#define RB_NOW0   1000u               /* the table's clock at release R  */
#define RB_RECNX  20000u              /* RECNXINTERVAL 20 s, in ms       */

/* OVMXE holds DEPT at CR, VAX1 masters it and leaves; VAX2 is the directory.
 * The departure is told at the GO, before release 1, as the arm tells it. */
static uint32_t lab33a_role(void)
{
	uint32_t root;

	g_bind_window = 0;
	(void)engine_up(CSID_OVMXE, 0x10000005u);
	table_up(CSID_VAX2, 0u);
	g_bind_window = 1;
	root = enq("DEPT", LCK_K_CRMODE, 0u);
	ct_check(root != 0u && vax1_grants(root, 0x0b000439u),
		 "OVMXE's DEPT root at CR is a proxy naming VAX1");
	g_dir_of_names = CSID_VAX2;
	g_in_transition = 1;               /* told at the GO, mid-transition */
	vax1_departs();
	return root;
}

/* The transition's end, as the arm runs it: the window shut, then a kick. */
static void transition_ends(void)
{
	g_in_transition = 0;
	vms_dlm_rm_rb_window_close(&g_rm);
	vms_dlm_rm_rb_kick(&g_rm);
}

/* Release `from`..`to`, recording how many records were on the wire after
 * each. */
static void releases(uint32_t from, uint32_t to, uint32_t ldw,
		     uint32_t *sent_after)
{
	uint32_t n;

	for (n = from; n <= to; n++) {
		vms_dlm_rm_rb_barrier_released(&g_rm, n, ldw, RB_NOW0);
		sent_after[n] = g_n_sent;
	}
}

static void test_the_window_rides_the_barrier(void)
{
	uint32_t sent_after[13], root, n;
	uint8_t ans[VMS_CM_BODY_LEN];

	printf("-- rd vms-2fa: the record goes out on release R and on no "
	       "other\n");
	ct_check_eq_u32(vms_dlm_rb_release_step(0u), 6u,
			"R for a weight-0 node is 6 (588 of 588 real records)");
	ct_check_eq_u32(vms_dlm_rb_release_step(1u), 5u,
			"R for a weighted node is 5 (1,377 of 1,377)");
	ct_check_eq_u32(vms_dlm_rb_release_step(3u), 5u,
			"  any non-zero weight");

	/* --- arm 3b / lab33a: a weight-0 survivor --- */
	memset(sent_after, 0, sizeof(sent_after));
	root = lab33a_role();
	ct_check_eq_u32(g_n_sent, 0u, "the departure (at the GO) sends nothing");
	releases(1u, 5u, 0u, sent_after);
	{
		/* Literal labels: the negctl manifest names them. */
		static const char *const early[6] = { "",
			"*** release 1: no record yet (an EARLY record is "
			"outside every real one) ***",
			"*** release 2: no record yet (an EARLY record is "
			"outside every real one) ***",
			"*** release 3: no record yet (an EARLY record is "
			"outside every real one) ***",
			"*** release 4: no record yet (an EARLY record is "
			"outside every real one) ***",
			"*** release 5: no record yet (an EARLY record is "
			"outside every real one) ***" };

		for (n = 1u; n <= 5u; n++)
			ct_check_eq_u32(sent_after[n], 0u, early[n]);
	}
	ct_check(!vms_dlm_rm_rb_window_open(&g_rm), "  the window is shut");
	releases(6u, 6u, 0u, sent_after);
	ct_check_eq_u32(sent_after[6], 1u,
			"*** release 6: the ROOT goes out NOW (arm 3b frame 22), "
			"not at the transition's end (lab33a frame 7801) ***");
	ct_check(g_n_sent == 1u && g_sent[0].csid == CSID_VAX2 &&
		 g_sent[0].body[8] == 0x02 && g_sent[0].body[9] == 0x08 &&
		 le32(g_sent[0].body + 24) == root,
		 "  a cat-0x02 op-0x08 ROOT for our DEPT lock, to the directory "
		 "VAX2");
	ct_check_eq_u32(vms_dlm_rm_rb_outstanding(&g_rm), 1u,
			"*** ONE record unanswered: what the barrier holds step 7 "
			"on ***");
	answer_for(ans, ra1b_vax2_rmt1_ans, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX2, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1,
		 "VAX2's 0xf9 (frame 23) is the answer");
	ct_check_eq_u32(vms_dlm_rm_rb_outstanding(&g_rm), 0u,
			"*** answered: nothing outstanding, step 7 may go ***");
	ct_check_eq_u32(g_rm.rb_mastered, 1u, "  OVMXE now masters DEPT");
	releases(7u, 12u, 0u, sent_after);
	ct_check(!vms_dlm_rm_rb_window_open(&g_rm),
		 "release 7 shut the window");
	ct_check_eq_u32(g_n_sent, 1u, "  and nothing more went out through "
			"release 12");
	transition_ends();
	ct_check_eq_u32(g_n_sent, 1u, "  nor at the transition's end");
	ct_check_eq_u32(g_rm.rb_out_of_window, 0u,
			"  and no tree was stopped for the step");
	ct_check_eq_u32(g_rm.rb_windows_opened, 1u, "one window, at release 6");
	engine_down();

	/* --- arm 2: the same holder, its own LOCKDIRWT 1 --- */
	memset(sent_after, 0, sizeof(sent_after));
	root = lab33a_role();
	releases(1u, 4u, 1u, sent_after);
	ct_check_eq_u32(sent_after[4], 0u,
			"weighted: nothing through release 4");
	releases(5u, 5u, 1u, sent_after);
	ct_check_eq_u32(sent_after[5], 1u,
			"*** weighted: the ROOT goes out on release 5 (arm 2, "
			"796 of 796) ***");
	answer_for(ans, ra1b_vax2_rmt1_ans, root);
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX2, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	releases(6u, 12u, 1u, sent_after);
	ct_check_eq_u32(g_n_sent, 1u,
			"  and release 6 is just another release to it");
	transition_ends();
	engine_down();
}

/* A barrier that did NOT hold: release R+1 arrives with a record still out,
 * and the answer comes after it. The next record (the sub) must not go. */
static void test_no_record_after_the_window_shuts(void)
{
	uint32_t root, s0;
	uint8_t ans[VMS_CM_BODY_LEN];

	printf("-- rd vms-2fa: an answer after release R+1 sends nothing "
	       "more\n");
	g_bind_window = 0;
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	g_bind_window = 1;
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	s0 = enq("S0", LCK_K_CRMODE, root);
	ct_check(s0 != 0u && vax1_grants(s0, 0x0600070au),
		 "RMT1 PR with S0 CR under it, granted by VAX1");
	g_dir_of_names = CSID_VAX3;
	g_in_transition = 1;
	vax1_departs();
	vms_dlm_rm_rb_barrier_released(&g_rm, 6u, 0u, RB_NOW0);
	ct_check_eq_u32(g_n_sent, 1u, "release 6: the ROOT goes");
	vms_dlm_rm_rb_barrier_released(&g_rm, 7u, 0u, RB_NOW0);
	ct_check(!vms_dlm_rm_rb_window_open(&g_rm), "release 7 shuts it");
	answer_for(ans, ra1b_vax3_rmt1_ans, root);   /* a real 0xfa */
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1,
		 "the late 0xfa re-homes the root");
	ct_check_eq_u32(g_n_sent, 1u,
			"*** the SUB it would release is NOT sent: the window "
			"is shut ***");
	ct_check_eq_u32(g_rm.rb_out_of_window, 1u, "  the tree stopped, counted");
	transition_ends();
	engine_down();
}

/* An answer that never comes: bounded by the cluster's RECNXINTERVAL. */
static void test_an_unanswered_record_is_bounded(void)
{
	uint32_t stopped0;

	printf("-- rd vms-2fa: a record nobody answers holds the barrier for "
	       "RECNXINTERVAL, no longer\n");
	(void)lab33a_role();
	vms_dlm_rm_rb_barrier_released(&g_rm, 6u, 0u, RB_NOW0);
	ct_check_eq_u32(vms_dlm_rm_rb_outstanding(&g_rm), 1u,
			"release 6: one record out");
	stopped0 = vms_dlm_rm_rb_stopped(&g_rm);
	ct_check_eq_u32(vms_dlm_rm_rb_expire(&g_rm, RB_NOW0 + RB_RECNX - 1u,
					     RB_RECNX), 0u,
			"  a moment short of RECNXINTERVAL: still held");
	ct_check_eq_u32(vms_dlm_rm_rb_expire(&g_rm, RB_NOW0 + 5000u, 0u), 0u,
			"  no interval, no expiry");
	ct_check_eq_u32(vms_dlm_rm_rb_outstanding(&g_rm), 1u, "  still one");
	ct_check_eq_u32(vms_dlm_rm_rb_expire(&g_rm, RB_NOW0 + RB_RECNX,
					     RB_RECNX), 1u,
			"*** at RECNXINTERVAL the tree is stopped ***");
	ct_check_eq_u32(vms_dlm_rm_rb_outstanding(&g_rm), 0u,
			"  nothing outstanding: the barrier goes on");
	ct_check_eq_u32(g_rm.rb_answer_expired, 1u, "  counted");
	ct_check_eq_u32(vms_dlm_rm_rb_stopped(&g_rm), stopped0 + 1u,
			"  in the stopped total");
	ct_check_eq_u32(g_n_sent, 1u, "  and nothing was re-sent");
	transition_ends();
	engine_down();
}

/* ================================================================== *
 * 8. lab874f REPLAYED (rd vms-427): told 0xf9, this node masters DEPT --
 *    and the RSB it now masters carries its own lock's value block, so a
 *    remote requester's grant carries it too.
 * ================================================================== */

/* VAX2's REAL 0xf9 for OVMXE's DEPT ROOT, frame 5324 of lab874f
 * (tests/lab/captures/vms-874f-rebuild-window-20261102/transition-frames.txt),
 * body[0:132] verbatim. */
static const uint8_t lab874f_vax2_dept_f9[VMS_CM_BODY_LEN] = {
	0x2d, 0x01, 0x81, 0x00, 0x01, 0x00, 0x14, 0x00, 0x82, 0x01, 0x00, 0x00,
	0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x10,
	0x91, 0x03, 0x00, 0x01, 0x01, 0x01, 0x48, 0x00, 0x02, 0x00, 0xf9, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04,
	0x44, 0x45, 0x50, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x52, 0x4d, 0x4c, 0x56, 0x42, 0x2d, 0x30, 0x31,
	0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x3c, 0x21, 0x57, 0x90,
};

#define LAB874F_VAX2_EXT  0x0200050du   /* VAX2's EX lock on DEPT, its SDA */

/* VAX2's $ENQ for DEPT reaching this node as DEPT's master, the way the arm
 * fills the master seam (vms_dlm_scs.c dlm_arm_fill_master_req). */
static uint32_t vax2_asks_dept(uint32_t mode, uint32_t req_lkid,
			       struct vms_dlm_master_result *res)
{
	struct vms_dlm_master_request mr;

	memset(&mr, 0, sizeof(mr));
	mr.op = VMS_DLM_MREQ_ENQ;
	mr.req_csid = CSID_VAX2;
	mr.req_lkid = req_lkid;
	mr.lkmode = mode;
	memcpy(mr.resnam, "DEPT", 4);
	mr.resnam_len = 4u;
	mr.res_group = 0u;
	mr.res_mode = 3u;
	mr.res_ident_valid = 1u;
	memset(res, 0, sizeof(*res));
	return vms_lock_dlm_master_serve(&mr, res);
}

/* OVMXE holds DEPT at CR, granted by VAX1 with RMLVB-0123456789 at 1; VAX1
 * leaves; the ROOT goes to VAX2 and VAX2's REAL 0xf9 makes OVMXE the master. */
static uint32_t lab874f_told_f9(void)
{
	uint32_t root;
	uint8_t ans[VMS_CM_BODY_LEN];

	(void)engine_up(CSID_OVMXE, 0x10000005u);
	table_up(CSID_VAX2, 0u);
	root = enq("DEPT", LCK_K_CRMODE, 0u);
	ct_check(root != 0u && vax1_grants_vb(root, 0x0b000439u, RMT1_LVB,
					      RMT1_LVB_SEQ),
		 "OVMXE's DEPT at CR, granted by VAX1 with RMLVB-0123456789 "
		 "at 1 (lab874f P0: VAX1's SDA, Seqnum 1)");
	g_dir_of_names = CSID_VAX2;
	vax1_departs();
	ct_check(g_n_sent == 1u && g_sent[0].csid == CSID_VAX2 &&
		 g_sent[0].body[9] == 0x08 &&
		 memcmp(g_sent[0].body + 88, lab874f_vax2_dept_f9 + 88, 20) ==
		 0,
		 "the ROOT goes to VAX2 carrying RMLVB-0123456789 at 1 "
		 "(lab874f frame 5322)");
	answer_for(ans, lab874f_vax2_dept_f9, root);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX2, 0x82, 0x01, ans,
				      VMS_CM_BODY_LEN) == 1 &&
		 g_rm.rb_mastered == 1u &&
		 vms_lock_dlm_name_mastered_here("DEPT", 4u, 0u, 3u),
		 "VAX2's real 0xf9 (frame 5324): OVMXE now masters DEPT");
	return root;
}

static void test_lab874f_the_adopted_rsb_keeps_the_block(void)
{
	struct vms_dlm_master_result q, r;
	struct vms_dlm_grant_notify gn;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t root, written = 0u;
	const uint8_t *b = frame + VMS_OFF_SYSAP_BODY;

	printf("-- rd vms-427: lab874f replayed -- told 0xf9, then VAX2's EX "
	       "waits and is granted, WITH the block\n");
	root = lab874f_told_f9();

	/* D2: VAX2's EX waits behind OVMXE's CR. */
	ct_check(vax2_asks_dept(LCK_K_EXMODE, LAB874F_VAX2_EXT, &q) ==
		 SS__NORMAL && q.outcome == VMS_DLM_MASTER_QUEUED &&
		 q.master_lkid != 0u,
		 "VAX2's EX on DEPT waits while OVMXE holds CR (lab874f D2)");

	/* D3: OVMXE's Ctrl-Y -- a LOCAL release that advances the queue. */
	{
		struct vms_deq_args d;

		memset(&d, 0, sizeof(d));
		d.lkid = root;
		vms_ioctl_deq(&g_proc, (unsigned long)(void *)&d);
		ct_check_eq_u32(d.status, SS__NORMAL,
				"OVMXE releases its CR (the Ctrl-Y)");
	}

	/* The deferred grant, read back the way the arm reads it at the send. */
	ct_check_eq_u32(vms_lock_dlm_grant_advance_read(CSID_VAX2,
							q.master_lkid, &r),
			SS__NORMAL, "VAX2's EX is now granted (lab874f D3)");
	/* negctl: dlm-f9-promotion-drops-the-valblk */
	ct_check(r.valblk_present == 1u &&
		 memcmp(r.valblk, RMT1_LVB, VMS_DLM_VALBLK_LEN) == 0 &&
		 r.valblk_seq == RMT1_LVB_SEQ,
		 "*** the grant reads RMLVB-0123456789 at sequence 1 off the "
		 "RSB OVMXE adopted -- its own lock's pair, not the zero "
		 "block at 0 VAX2's SDA showed (lab874f, rd vms-427) ***");

	memset(&gn, 0, sizeof(gn));
	gn.master_lkid = r.deferred_master_lkid;
	gn.req_lkid = r.deferred_req_lkid;
	gn.granted_mode = r.deferred_mode;
	gn.req_mode = r.deferred_req_mode;
	gn.valblk_present = r.valblk_present;
	memcpy(gn.valblk, r.valblk, sizeof(gn.valblk));
	gn.valblk_seq = r.valblk_seq;
	memset(frame, 0, sizeof(frame));
	ct_check(vms_dlm_grant_notify_build(&gn, frame, sizeof(frame),
					    &written) == VMS_CODEC_OK,
		 "the op-0x02 deferred grant builds");
	/* negctl: grant-notify-drops-the-valblk */
	ct_check(b[8] == 0x02 && b[9] == 0x02 &&
		 memcmp(b + 36, RMT1_LVB, 16) == 0 && le32(b + 52) == 1u &&
		 le32(b + 24) == LAB874F_VAX2_EXT && b[30] == LCK_K_EXMODE,
		 "*** and the op-0x02 VAX2 receives carries it at body[36:52] "
		 "with its sequence at body[52:56], as 41 of 42 real deferred "
		 "grants carry the block their master's last grant did ***");
	{
		char st[160];

		/* The DLM sequence stamp every DLM message gets on the way out
		 * (vms_dlm_seq_put, as test_cnxman_coord drives it) -- lab874f's
		 * 3 -- so the gate reads the frame as it leaves. */
		ct_check(vms_dlm_seq_put(frame + VMS_OFF_SYSAP_BODY,
					 VMS_CM_BODY_LEN, 3u) == VMS_CODEC_OK,
			 "  stamped with the cluster's DLM sequence");
		snprintf(st, sizeof(st),
			 "seq=3,mlkid=0x%x,lkid=0x%x,mode=%u,reqmode=%u,"
			 "valblkseq=0x%x",
			 gn.master_lkid, gn.req_lkid, gn.granted_mode,
			 gn.req_mode, gn.valblk_seq);
		conf_dump("dlm_grant_notify", VMS_OFF_SYSAP_BODY, st, b,
			  VMS_CM_BODY_LEN);
	}
	engine_down();

	/* The immediate grant reads the same RSB: a compatible PR is granted
	 * at once, block and sequence with it. */
	printf("-- rd vms-427: ...and an immediate grant reads the same "
	       "RSB\n");
	(void)lab874f_told_f9();
	ct_check(vax2_asks_dept(LCK_K_PRMODE, LAB874F_VAX2_EXT, &q) ==
		 SS__NORMAL && q.outcome == VMS_DLM_MASTER_GRANTED,
		 "VAX2's PR on DEPT is granted at once beside OVMXE's CR");
	ct_check(q.valblk_present == 1u &&
		 memcmp(q.valblk, RMT1_LVB, VMS_DLM_VALBLK_LEN) == 0 &&
		 q.valblk_seq == RMT1_LVB_SEQ,
		 "*** the 82/01 grant returns RMLVB-0123456789 at 1 ***");
	engine_down();
}

/* ================================================================== *
 * 9. RA1b frame 7400 (rd vms-427): a survivor's record does not overwrite a
 *    LATER pair on the RSB a 0xf9 holder masters; a later one replaces it.
 * ================================================================== */
static uint32_t vax2_master_of_rmt1(uint32_t own_seq)
{
	uint32_t root;
	uint8_t ans[VMS_CM_BODY_LEN];

	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants_vb(root, 0x05000024u, RMT1_LVB, own_seq);
	g_dir_of_names = CSID_VAX3;
	vax1_departs();
	answer_for(ans, ra1b_vax2_rmt1_ans, root);
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	return root;
}

static void test_ra1b_7400_the_later_sequence_stays(void)
{
	uint8_t rec[VMS_CM_BODY_LEN];

	printf("-- rd vms-427: RA1b frame 7400 -- the RSB keeps the LATER "
	       "pair\n");
	(void)vax2_master_of_rmt1(2u);
	ct_check_eq_u32(g_rm.rb_mastered, 1u, "told 0xf9 with its own pair at 2");
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3,
					     ra1b_vax3_rmt1_req,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_HANDLED,
			"VAX3's real ROOT record (RMLVB at 1) is installed");
	/* negctl: dlm-rebuild-record-overwrites-a-later-pair */
	ct_check(g_staged[34] == 0xfa &&
		 memcmp(g_staged + 36, RMT1_LVB, 16) == 0 &&
		 le32(g_staged + 52) == 2u,
		 "*** its 0xfa returns the RSB's pair at 2, not the record's 1 "
		 "(RA1b frame 7400: own 2, incoming 1, answer 2) ***");
	engine_down();

	(void)vax2_master_of_rmt1(1u);
	memcpy(rec, ra1b_vax3_rmt1_req, VMS_CM_BODY_LEN);
	put32(rec + 104, 2u);
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_HANDLED,
			"a record carrying the same block at 2 is installed");
	ct_check(g_staged[34] == 0xfa && le32(g_staged + 52) == 2u,
		 "*** a HIGHER sequence replaces the RSB's (3 of 3 real "
		 "answers return the incoming pair then) ***");
	engine_down();
}


/* ================================================================== *
 * 9. rd vms-1b68 / vms-0db: A WAITING OR CONVERTING LOCK OUTLIVES ITS
 *    MASTER. tests/lab/captures/vms-0db-waiting-records-20261102 (arm B,
 *    WQRB): a real holder re-sends its converting (5,1) and waiting (5,0)
 *    locks as op-0x08 records with body[110] 00 / ff and body[108] 04,
 *    after its granted ones; the new master answers each 0xfa with body[28]
 *    0x14 and the record's own body[30:32], and queues it, ungranted.
 * ================================================================== */
#define VAX3_W_HANDLE 0x020004efu    /* arm B: VAX2's answer to the waiter */

/* The grant state of one of g_proc's locks: 0 pending, SS$_ when ended. */
static int lk_state(uint32_t lkid)
{
	struct vms_lock_entry *l;

	exec_list_for_each_entry(l, &g_proc.locks, proc_list) {
		if (l->lkid == lkid)
			return l->grant_state;
	}
	return -1;
}

static uint32_t d_rb_unsent(void *ctx, const struct vms_dlm_adopt_tree *t)
{
	(void)ctx;
	return vms_lock_dlm_rebuild_fail_unsent(t, NULL, 0u);
}

static void d_rb_settle(void *ctx)
{
	(void)ctx;
	(void)vms_lock_dlm_rebuild_settle();
}

/* A survivor table: VAX3 is the directory, the rebuild's end and settle are
 * bound, and this node (VAX2) holds RMT1 at PR (granted by VAX1). */
static uint32_t survivor_up(void)
{
	uint32_t root;

	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	g_ops.rb_unsent = d_rb_unsent;
	g_ops.rb_settle = d_rb_settle;
	root = enq("RMT1", LCK_K_PRMODE, 0u);
	(void)vax1_grants(root, 0x05000024u);
	g_dir_of_names = CSID_VAX3;
	return root;
}

static void test_codec_queue_states(void)
{
	struct vms_dlm_rm_lock_rec r, back;
	struct vms_dlm_rm_lock_answer a;
	struct vms_dlm_rb_modes m;
	struct vms_dlm_rb_mode_cells c;
	uint8_t f[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	uint8_t g[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	uint8_t ans[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	const uint8_t *b = f + VMS_OFF_SYSAP_BODY;
	const uint8_t *ab = ans + VMS_OFF_SYSAP_BODY;

	printf("-- rd vms-0db: body[110] is the queue, body[30] requested, "
	       "body[31] granted\n");
	ct_check(vms_dlm_rb_rec_parse_body(ra1b_vax3_rmt1_req, VMS_CM_BODY_LEN,
					   &r) == VMS_CODEC_OK &&
		 r.queue_state == VMS_DLM_RB_Q_GRANTED,
		 "VAX3's real granted ROOT reads GRANTED (body[110] 01)");
	ct_check(vms_dlm_rb_rec_build(&r, 1, g, sizeof(g), NULL) ==
		 VMS_CODEC_OK &&
		 g[VMS_OFF_SYSAP_BODY + 110] == 0x01u &&
		 g[VMS_OFF_SYSAP_BODY + 108] == 0x00u &&
		 g[VMS_OFF_SYSAP_BODY + 28] == 0x00u,
		 "  a granted record OVMX builds: 01 at [110], 0 at [108] and "
		 "[28], as before");

	/* WQRB's waiter, arm B frame 1071: rq/gr 5/0 st ff. */
	r.queue_state = VMS_DLM_RB_Q_WAITING;
	r.requested_mode = (uint8_t)LCK_K_EXMODE;
	r.granted_mode = (uint8_t)LCK_K_CRMODE;   /* ignored: never granted */
	r.flags = VMS_DLM_REC_FLAG_VALBLK | VMS_DLM_REC_FLAG_SYSTEM | 0x80u;
	ct_check(vms_dlm_rb_rec_build(&r, 1, f, sizeof(f), NULL) ==
		 VMS_CODEC_OK, "a WAITING record is built");
	ct_check(b[30] == 5u && b[31] == 0u && b[110] == 0xffu &&
		 b[108] == 0x04u && b[28] == 0x11u,
		 "*** waiting: (5,0), [110] ff, [108] 04, [28] 11 -- arm B's "
		 "frame 1071 ***");
	ct_check(vms_dlm_rb_rec_parse_body(b, VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_OK && back.queue_state == VMS_DLM_RB_Q_WAITING &&
		 back.requested_mode == 5u && back.granted_mode == 0u,
		 "  and reads back WAITING, requested EX, granted 0");
	memset(&a, 0, sizeof(a));
	a.master_lkid = VAX3_W_HANDLE;
	ct_check(vms_dlm_rm_lock_answer_build(b, VMS_CM_BODY_LEN, &a, ans,
					      sizeof(ans), NULL) ==
		 VMS_CODEC_OK && ab[34] == 0xfau && ab[28] == 0x14u &&
		 ab[30] == 5u && ab[31] == 0u && le32(ab + 20) == VAX3_W_HANDLE,
		 "*** its answer: 0xfa, body[28] 0x14, body[30:32] echoed ***");

	/* WQRB's converter, frame 1069: rq/gr 5/1 st 00. */
	r.queue_state = VMS_DLM_RB_Q_CONVERTING;
	r.granted_mode = (uint8_t)LCK_K_CRMODE;
	r.flags = VMS_DLM_REC_FLAG_VALBLK | VMS_DLM_REC_FLAG_CONVERT |
		  VMS_DLM_REC_FLAG_SYSTEM;
	ct_check(vms_dlm_rb_rec_build(&r, 1, f, sizeof(f), NULL) ==
		 VMS_CODEC_OK && b[30] == 5u && b[31] == 1u &&
		 b[110] == 0x00u && b[108] == 0x04u && b[28] == 0x13u,
		 "*** converting: (5,1), [110] 00, [108] 04, [28] 13 -- frame "
		 "1069 ***");
	ct_check(vms_dlm_rb_rec_parse_body(b, VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_OK && back.queue_state == VMS_DLM_RB_Q_CONVERTING &&
		 back.granted_mode == 1u && back.requested_mode == 5u,
		 "  and reads back CONVERTING, CR held, EX requested");
	ct_check(vms_dlm_rm_lock_rec_build(&r, 1, g, sizeof(g), NULL) ==
		 VMS_CODEC_OK &&
		 memcmp(g + VMS_OFF_SYSAP_BODY + 10, b + 10,
			VMS_CM_BODY_LEN - 10) == 0,
		 "  the op-0x12 record carries the same cells (arm A4/A5)");

	/* A granted, down-converted lock keeps its earlier request: (5,1) at
	 * state 01 is GRANTED (arm A5, VAX1's WQRE root). */
	memcpy(g, f, sizeof(g));
	g[VMS_OFF_SYSAP_BODY + 110] = 0x01u;
	ct_check(vms_dlm_rb_rec_parse_body(g + VMS_OFF_SYSAP_BODY,
					   VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_OK && back.queue_state == VMS_DLM_RB_Q_GRANTED &&
		 back.granted_mode == 1u,
		 "*** (5,1) at [110] 01 is GRANTED at CR: the modes never "
		 "decide the queue ***");
	g[VMS_OFF_SYSAP_BODY + 110] = 0x02u;
	ct_check(vms_dlm_rb_rec_parse_body(g + VMS_OFF_SYSAP_BODY,
					   VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_E_RANGE,
		 "a body[110] naming no queue is REFUSED");
	g[VMS_OFF_SYSAP_BODY + 110] = 0x01u;
	g[VMS_OFF_SYSAP_BODY + 30] = 6u;
	ct_check(vms_dlm_rb_rec_parse_body(g + VMS_OFF_SYSAP_BODY,
					   VMS_CM_BODY_LEN, &back) ==
		 VMS_CODEC_E_RANGE, "...and so is a mode above EX");
	m.queue_state = 9u;
	m.granted_mode = m.requested_mode = m.flags = 0u;
	ct_check(vms_dlm_rb_rec_modes(1, &m, &c) == VMS_CODEC_E_INVAL,
		 "an unknown queue is never encoded");
}

static void test_a_queued_waiter_is_rebuilt(void)
{
	uint8_t ans[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	struct vms_dlm_rm_lock_answer a;
	struct vms_dlm_proxy_grant gr;
	uint32_t root, w;

	printf("-- rd vms-1b68: a waiter VAX1 queued is re-sent to VAX3 and "
	       "granted by it\n");
	root = survivor_up();
	w = enq("RMT1", LCK_K_EXMODE, 0u);
	ct_check(w != 0u && lk_state(w) == 0, "our EX waits at VAX1");
	ct_check_eq_u32(vms_lock_dlm_proxy_queued_recv(w, CSID_VAX1, 0x0700040au),
			SS__NORMAL, "  VAX1 queued it (0xfb, its handle)");
	vax1_departs();
	ct_check(lk_state(w) == 0 && vms_lock_dlm_proxy_survives(w),
		 "*** VAX1 left and the queued waiter is NOT ended ***");
	ct_check(g_n_sent == 1u && le32(g_sent[0].body + 24) == root,
		 "  the granted ROOT goes first");
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	ct_check(g_n_sent == 2u && le32(g_sent[1].body + 24) == w &&
		 g_sent[1].body[9] == 0x08u && g_sent[1].body[30] == 5u &&
		 g_sent[1].body[31] == 0u && g_sent[1].body[110] == 0xffu &&
		 g_sent[1].body[108] == 0x04u && le32(g_sent[1].body + 36) == 0u,
		 "*** then the waiter: op 0x08, (5,0), [110] ff, [108] 04 ***");
	/* The frame-conformance gate sees the waiting record (rd vms-1b68),
	 * its state read off the waiter's own LKB. */
	dump08(g_sent[1].body, w, g_proc.vms_pid, 0u, 0u, RMT1_HASH, 0u);
	memset(&a, 0, sizeof(a));
	a.master_lkid = VAX3_W_HANDLE;
	ct_check(vms_dlm_rm_lock_answer_build(g_sent[1].body, VMS_CM_BODY_LEN,
					      &a, ans, sizeof(ans), NULL) ==
		 VMS_CODEC_OK, "  VAX3 answers it 0xfa");
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01,
				      ans + VMS_OFF_SYSAP_BODY,
				      VMS_CM_BODY_LEN) == 1 &&
		 g_rm.rb_trees_done == 1u && g_rm.rb_waiters_ended == 0u,
		 "  the tree is done and no waiter was ended");
	ct_check(lk_state(w) == 0, "*** still waiting, now at VAX3 ***");
	vms_dlm_rm_rb_window_close(&g_rm);
	ct_check(lk_state(w) == 0, "  the window's close grants nothing VAX3 "
		 "masters");
	memset(&gr, 0, sizeof(gr));
	gr.req_lkid = w;
	gr.master_lkid = VAX3_W_HANDLE;
	gr.master_csid = CSID_VAX3;
	gr.valblk_present = 1u;
	(void)vms_lock_dlm_proxy_grant_recv(&gr);
	ct_check(lk_state(w) == (int)SS__NORMAL,
		 "*** VAX3's grant completes the $ENQ ***");
	engine_down();

	/* Never acknowledged: no survivor, no record. (Its requester block is
	 * failed PATHLOST at the departure -- test_dlm_requester's
	 * test_survivor_keeps_its_block -- which ends it SS$_UNSUPPORTED.) */
	root = survivor_up();
	w = enq("RMT1", LCK_K_EXMODE, 0u);
	vax1_departs();
	ct_check(lk_state(w) == 0 && !vms_lock_dlm_proxy_survives(w),
		 "*** a request VAX1 never acknowledged does not survive ***");
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	ct_check_eq_u32(g_n_sent, 1u, "  and no record of it goes out");
	engine_down();

	/* Acknowledged, but its tree's rebuild is STOPPED: ended, not left. */
	root = survivor_up();
	w = enq("RMT1", LCK_K_EXMODE, 0u);
	(void)vms_lock_dlm_proxy_queued_recv(w, CSID_VAX1, 0x0700040au);
	vax1_departs();
	answer_for(ans, ra1b_vax3_rmt1_ans, root);
	ans[34] = 0xfb;                                    /* not a grant */
	(void)vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01, ans,
				   VMS_CM_BODY_LEN);
	ct_check(lk_state(w) == (int)SS__UNSUPPORTED &&
		 g_rm.rb_waiters_ended == 1u,
		 "*** a survivor whose rebuild stopped ends SS$_UNSUPPORTED: "
		 "no eternal wait ***");
	engine_down();

	/* Acknowledged, but no rebuild can run (doors unbound). */
	root = survivor_up();
	w = enq("RMT1", LCK_K_EXMODE, 0u);
	(void)vms_lock_dlm_proxy_queued_recv(w, CSID_VAX1, 0x0700040au);
	g_ops.rb_install = NULL;
	vax1_departs();
	ct_check(lk_state(w) == (int)SS__UNSUPPORTED,
		 "  ...and so does one no rebuild can carry at all");
	engine_down();
	(void)root;
}

/* rd vms-1b68: a CONVERSION VAX1 queued (PR -> EX on the lock it granted) is
 * re-sent as a CONVERTING record -- (5,3), [110] 00, [108] 04, [28] CONVERT
 * -- and VAX3's later grant completes it. */
static void test_a_queued_conversion_is_rebuilt(void)
{
	uint8_t ans[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	struct vms_dlm_rm_lock_answer a;
	struct vms_dlm_proxy_grant gr;
	struct vms_enq_args e;
	uint32_t root;

	printf("-- rd vms-1b68: a conversion VAX1 queued is re-sent CONVERTING "
	       "and granted by VAX3\n");
	root = survivor_up();
	memset(&e, 0, sizeof(e));
	e.lkid = root;
	e.lkmode = LCK_K_EXMODE;
	e.flags = LCK_M_CONVERT;
	(void)vms_ioctl_convert(&g_proc, (unsigned long)(void *)&e);
	ct_check(e.status == SS__NORMAL && lk_state(root) == 0,
		 "our PR -> EX conversion waits at VAX1");
	ct_check_eq_u32(vms_lock_dlm_proxy_queued_recv(root, CSID_VAX1,
						       0x05000024u),
			SS__NORMAL, "  VAX1 queued it (0xfb)");
	vax1_departs();
	ct_check(lk_state(root) == 0 && vms_lock_dlm_proxy_survives(root),
		 "*** VAX1 left and the queued conversion is NOT ended ***");
	ct_check(g_n_sent == 1u && le32(g_sent[0].body + 24) == root &&
		 g_sent[0].body[30] == 5u && g_sent[0].body[31] == 3u &&
		 g_sent[0].body[110] == 0x00u && g_sent[0].body[108] == 0x04u &&
		 g_sent[0].body[28] == 0x02u,
		 "*** its record: (5,3), [110] 00, [108] 04, [28] CONVERT ***");
	dump08(g_sent[0].body, root, g_proc.vms_pid, 0u, 0u, RMT1_HASH, 0u);
	memset(&a, 0, sizeof(a));
	a.master_lkid = 0x020004edu;            /* arm B: VAX2's converter */
	(void)vms_dlm_rm_lock_answer_build(g_sent[0].body, VMS_CM_BODY_LEN, &a,
					   ans, sizeof(ans), NULL);
	ct_check(vms_dlm_rm_rb_answer(&g_rm, CSID_VAX3, 0x82, 0x01,
				      ans + VMS_OFF_SYSAP_BODY,
				      VMS_CM_BODY_LEN) == 1 &&
		 lk_state(root) == 0,
		 "  VAX3's 0xfa re-homes it, still converting");
	memset(&gr, 0, sizeof(gr));
	gr.req_lkid = root;
	gr.master_lkid = 0x020004edu;
	gr.master_csid = CSID_VAX3;
	gr.valblk_present = 1u;
	(void)vms_lock_dlm_proxy_grant_recv(&gr);
	ct_check(lk_state(root) == (int)SS__NORMAL,
		 "*** VAX3's grant completes the conversion ***");
	engine_down();
}

static void test_this_node_masters_the_waiter(void)
{
	uint8_t rec[VMS_CM_BODY_LEN];
	uint32_t w, n0;

	printf("-- rd vms-1b68: this node is RMT1's directory and its next "
	       "master; its waiter is granted at the window's close\n");
	(void)engine_up(CSID_VAX2, 0x20400217u);
	table_up(CSID_VAX3, 0u);
	g_ops.rb_unsent = d_rb_unsent;
	g_ops.rb_settle = d_rb_settle;
	g_dir_of_names = 0u;                         /* THIS node directs it */
	w = enq("RMT1", LCK_K_EXMODE, 0u);
	(void)vms_lock_dlm_proxy_queued_recv(w, CSID_VAX1, 0x0700040au);
	vax1_departs();
	ct_check(g_rm.rb_mastered == 1u && g_n_sent == 0u,
		 "this node masters RMT1 now; nothing is sent");
	ct_check(lk_state(w) == 0,
		 "*** our EX waiter is NOT granted before the survivors' "
		 "records are in ***");
	/* VAX3's waiting PR, re-sent to us (its ROOT record, re-queued). */
	n0 = vms_lock_dlm_rebuild_installed();
	memcpy(rec, ra1b_vax3_rmt1_req, VMS_CM_BODY_LEN);
	put32(rec + 24, 0x07000404u);
	rec[30] = (uint8_t)LCK_K_PRMODE;
	rec[31] = 0u;
	rec[108] = 0x04u;
	rec[110] = 0xffu;
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_HANDLED,
			"VAX3's WAITING record is installed");
	ct_check(g_staged[34] == 0xfau && g_staged[28] == 0x14u &&
		 g_staged[30] == (uint8_t)LCK_K_PRMODE && g_staged[31] == 0u &&
		 le32(g_staged + 20) != 0u,
		 "*** answered 0xfa, body[28] 0x14, its (body[30], body[31]) "
		 "echoed ***");
	ct_check_eq_u32(vms_lock_dlm_rebuild_installed(), n0 + 1u,
			"  one lock installed, on the waiting queue");
	dump08ans(g_staged);       /* the gate: the answer to a WAITING record */
	rec[110] = 0x02u;
	ct_check_eq_u32(vms_dlm_rm_rb_record(&g_rm, CSID_VAX3, rec,
					     VMS_CM_BODY_LEN),
			VMS_DLM_RM_NOT_MINE,
			"a record whose body[110] names no queue is not taken");
	vms_dlm_rm_rb_window_close(&g_rm);
	ct_check(lk_state(w) == (int)SS__NORMAL,
		 "*** the window closes: our waiter, first in the queue, is "
		 "GRANTED EX ***");
	ct_check(vms_lock_dlm_rebuild_settle() == 0u,
		 "  the settle runs once");
	engine_down();
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
	test_which_pair_the_record_relays();
	test_the_window_rides_the_barrier();
	test_no_record_after_the_window_shuts();
	test_an_unanswered_record_is_bounded();
	test_lab874f_the_adopted_rsb_keeps_the_block();
	test_ra1b_7400_the_later_sequence_stays();
	test_codec_queue_states();
	test_a_queued_waiter_is_rebuilt();
	test_a_queued_conversion_is_rebuilt();
	test_this_node_masters_the_waiter();
	return ct_summary("test_dlm_rebuild");
}
