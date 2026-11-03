/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_dirreg.c - rd vms-fdeb: a MASTER re-registers its directory entries
 * with cat-0x02 op-0x0d after release 4 of every state transition, on the REAL
 * lock engine (vms_lock.c), the remaster table (vms_dlm_rm_fsm.c) and the codec.
 *
 * Phase 1 of a transition discards every directory entry cluster-wide (Davis
 * pp. 6-33, 7-40/7-41) and each master re-registers its own. Until rd vms-fdeb
 * OVMX sent none, so after any transition a real directory held no entry for a
 * resource OVMX masters and answered the next lookup 0xf9 -- a second master.
 *
 * The real frames below are one record and its answer between two REAL V7.3
 * VAXes (tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap,
 * frames 116 and 121 as tools/cluster/transition_dlm_census.py's frames()
 * indexes them): 08:00:2b:7a:2a:ac re-registers MSCP$LOADBAL (kernel mode,
 * group 0) with 08:00:2b:03:7e:61 after the coordinator's release 4, and the
 * directory answers 82/0d with the record echoed and body[34] = 0xf9. That
 * record's residue happens to be all zero -- one of 15 real records like it,
 * each answered by a real VAX -- so it is byte-for-byte what this node's
 * builder writes outside the connection manager's body[0:8] and body[12:16].
 *
 * What is proved, each on the real engine:
 *   1. nothing goes before release 4; release 4 sends one record per ROOT
 *      resource this node masters whose directory is ANOTHER member -- not a
 *      sub-resource, not one this node directs itself;
 *   2. every record is the real one in every byte a real sender writes, with
 *      the hash the ENGINE computed equal to the real body[128:132];
 *   3. the barrier is held while any is unanswered; the REAL 82/0d answer
 *      releases it and makes that directory the one told (so the last $DEQ
 *      removes the entry there, rd vms-c55), while an answer from another
 *      node, or for another name, is not ours;
 *   4. an answer that is not 0xf9, or none within RECNXINTERVAL, stops the
 *      window and lets the barrier go -- never a hang, never a resend loop.
 *
 * Clean-room: our own lab captures only. No VMS binary or source.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_adopt.h"
#include "vms_dlm_hash.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_rm_fsm.h"

#include <stdio.h>
#include <string.h>

uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_SELF  0x00010005u   /* this node: the master                  */
#define CSID_DIR   0x00010002u   /* the directory node the vector names    */
#define CSID_OTHER 0x00010003u

#define BODY_LEN   VMS_CM_BODY_LEN

/* frame 116: 08:00:2b:7a:2a:ac -> 08:00:2b:03:7e:61, 02/0d MSCP$LOADBAL */
static const uint8_t real_req[BODY_LEN] = {
	0x08, 0x00, 0x03, 0x00, 0x06, 0x00, 0x5f, 0x1e, 0x02, 0x0d, 0x00, 0x00,
	0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c,
	0x4d, 0x53, 0x43, 0x50, 0x24, 0x4c, 0x4f, 0x41, 0x44, 0x42, 0x41, 0x4c,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x45, 0xa3, 0x35,
};

/* frame 121: 08:00:2b:03:7e:61 -> 08:00:2b:7a:2a:ac, its 82/0d answer */
static const uint8_t real_ans[BODY_LEN] = {
	0x08, 0x00, 0x08, 0x00, 0x06, 0x00, 0x5f, 0x1e, 0x82, 0x0d, 0x00, 0x00,
	0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf9, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c,
	0x4d, 0x53, 0x43, 0x50, 0x24, 0x4c, 0x4f, 0x41, 0x44, 0x42, 0x41, 0x4c,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x45, 0xa3, 0x35,
};

#define REAL_NAME     "MSCP$LOADBAL"
#define SECOND_NAME   "F11B$vDIRREG"
#define SELF_NAME     "F11B$vSELFDIR"   /* the vector says THIS node directs it */
#define SUB_NAME      "SUBLOCK"

/* ------------------------------------------------------------------ *
 * The engine's doors: the vector, and what the engine posts.
 * ------------------------------------------------------------------ */
static struct vms_dlm_requester_ops eng_ops;
static struct vms_proc g_proc, g_delivery;
static uint32_t g_dir = 0u;              /* 0: the vector names THIS node   */
static uint32_t g_self_hash;             /* the one hash this node directs  */
static uint32_t g_removals, g_removal_dst;

static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p->op == VMS_DLM_POST_DIR_REMOVE) {
		g_removals++;
		g_removal_dst = p->dst_csid;
	}
	return SS__NORMAL;
}

static uint32_t eng_dir_resolve(void *ctx, uint32_t hash, uint32_t *out)
{
	(void)ctx;
	*out = (g_dir == 0u || hash == g_self_hash) ? CSID_SELF : g_dir;
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return g_dir == 0u ? 1u : 2u;
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

static int engine_up(void)
{
	vms_local_csid = CSID_SELF;
	g_dir = 0u;
	g_self_hash = 0u;
	g_removals = 0u;
	g_removal_dst = 0u;
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return 0;
	}
	memset(&eng_ops, 0, sizeof(eng_ops));
	eng_ops.post = eng_post;
	eng_ops.dir_resolve = eng_dir_resolve;
	eng_ops.dir_generation = eng_dir_generation;
	vms_lock_dlm_set_requester_ops(&eng_ops);
	proc_init(&g_proc, 0x20200210u);
	g_proc.current_mode = 0u;      /* kernel mode, as MSCP$LOADBAL is held */
	proc_init(&g_delivery, 0x20200201u);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	return 1;
}

static void engine_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

static uint32_t enq(const char *name, uint32_t parid)
{
	struct vms_enq_args e;

	memset(&e, 0, sizeof(e));
	e.lkmode = LCK_K_EXMODE;
	e.parid = parid;
	strscpy(e.resnam, name, sizeof(e.resnam));
	vms_ioctl_enq(&g_proc, (unsigned long)(void *)&e);
	return e.status == SS__NORMAL ? e.lkid : 0u;
}

static uint32_t deq(uint32_t lkid)
{
	struct vms_deq_args d;

	memset(&d, 0, sizeof(d));
	d.lkid = lkid;
	vms_ioctl_deq(&g_proc, (unsigned long)(void *)&d);
	return d.status;
}

/* ------------------------------------------------------------------ *
 * The table's doors, as the arm binds them (vms_dlm_scs.c dlm_arm_dr_*).
 * ------------------------------------------------------------------ */
#define MAX_SENT 16
static struct { uint32_t csid; uint8_t body[BODY_LEN]; } g_sent[MAX_SENT];
static uint32_t g_n_sent;
static int      g_send_fails;

static int d_send(void *ctx, uint32_t csid, const uint8_t *b, uint32_t len)
{
	(void)ctx;
	if (g_send_fails || len != BODY_LEN || g_n_sent >= MAX_SENT)
		return -1;
	g_sent[g_n_sent].csid = csid;
	memcpy(g_sent[g_n_sent].body, b, BODY_LEN);
	g_n_sent++;
	return 0;
}

static void d_log(void *ctx, const char *msg)
{
	(void)ctx;
	printf("    [console] %s\n", msg);
}

static void to_tree(const struct vms_dlm_res_ident *id,
		    struct vms_dlm_adopt_tree *t)
{
	memset(t, 0, sizeof(*t));
	memcpy(t->resnam, id->name, id->name_len);
	t->resnam_len = id->name_len;
	t->res_group = id->group;
	t->res_mode = id->mode;
	t->ident_valid = 1u;
}

static int d_dr_next(void *ctx, const struct vms_dlm_res_ident *skip,
		     uint32_t n_skip, struct vms_dlm_res_ident *out,
		     uint32_t *dir)
{
	struct vms_dlm_adopt_tree sk[VMS_DLM_DR_WINDOW], t;
	uint32_t i, hash = 0u;

	(void)ctx;
	for (i = 0u; i < n_skip && i < VMS_DLM_DR_WINDOW; i++)
		to_tree(&skip[i], &sk[i]);
	if (!vms_lock_dlm_dirreg_next(sk, n_skip, &t, &hash, dir))
		return 0;
	memset(out, 0, sizeof(*out));
	memcpy(out->name, t.resnam, t.resnam_len);
	out->name_len = t.resnam_len;
	out->group = t.res_group;
	out->mode = t.res_mode;
	out->hash = hash;
	return 1;
}

static int d_dr_told(void *ctx, const struct vms_dlm_res_ident *id,
		     uint32_t dir)
{
	struct vms_dlm_adopt_tree t;

	(void)ctx;
	to_tree(id, &t);
	return vms_lock_dlm_dirreg_told(&t, dir);
}

static struct vms_dlm_rm_ops g_ops;
static struct vms_dlm_rm g_rm;

static void table_up(void)
{
	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.send = d_send;
	g_ops.log = d_log;
	g_ops.dr_next = d_dr_next;
	g_ops.dr_told = d_dr_told;
	vms_dlm_rm_init(&g_rm, &g_ops);
	g_n_sent = 0u;
	g_send_fails = 0;
}

/* A transition: Phase 1 discards, the vector moves to CSID_DIR, the barrier
 * releases 1..last. */
static void transition_to(uint32_t last, uint32_t now)
{
	uint32_t s;

	(void)vms_lock_dlm_directory_discarded();
	vms_dlm_rm_dr_close(&g_rm);
	g_dir = CSID_DIR;
	for (s = 1u; s <= last; s++)
		vms_dlm_rm_dr_barrier_released(&g_rm, s, now);
}

static int sent_names(uint32_t i, const char *name)
{
	return g_sent[i].body[47] == (uint8_t)strlen(name) &&
	       memcmp(&g_sent[i].body[48], name, strlen(name)) == 0;
}

static int sent_index(const char *name)
{
	uint32_t i;

	for (i = 0u; i < g_n_sent; i++)
		if (sent_names(i, name))
			return (int)i;
	return -1;
}

/* ------------------------------------------------------------------ *
 * 1 + 2: release 4 sends the real record, and only for what is owed one.
 * ------------------------------------------------------------------ */
static uint32_t g_lk_real, g_lk_second, g_lk_self, g_lk_sub;

static int masters_four(void)
{
	if (!engine_up())
		return 0;
	g_lk_real = enq(REAL_NAME, 0u);
	g_lk_second = enq(SECOND_NAME, 0u);
	g_lk_self = enq(SELF_NAME, 0u);
	g_lk_sub = g_lk_real ? enq(SUB_NAME, g_lk_real) : 0u;
	ct_check(g_lk_real && g_lk_second && g_lk_self && g_lk_sub,
		 "this node masters three roots and one sub-lock (the vector "
		 "names this node before the transition)");
	/* The vector after the transition keeps SELF_NAME's directory here. */
	ct_check(vms_dlm_name_hash(0u, 0u, (const uint8_t *)SELF_NAME,
				   (uint32_t)strlen(SELF_NAME), &g_self_hash) ==
		 VMS_DLM_HASH_OK, "SELF_NAME's hash");
	return g_lk_real && g_lk_second && g_lk_self && g_lk_sub;
}

static void test_release_4_sends_the_real_record(void)
{
	int i;
	uint32_t k, n;

	printf("-- release 4 re-registers what this node masters\n");
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	transition_to(3u, 100u);
	ct_check_eq_u32(g_n_sent, 0u, "nothing goes at releases 1..3");
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 0u,
			"...and nothing holds the barrier");
	vms_dlm_rm_dr_barrier_released(&g_rm, VMS_DLM_DR_REL_STEP, 100u);
	ct_check_eq_u32(g_n_sent, 2u,
			"release 4: one op-0x0d per root mastered here whose "
			"directory is another member");
	ct_check(sent_index(SUB_NAME) < 0, "...none for a sub-resource");
	ct_check(sent_index(SELF_NAME) < 0,
		 "...none for a root this node directs itself");
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 2u,
			"both hold the barrier until answered");

	i = sent_index(REAL_NAME);
	ct_check(i >= 0 && g_sent[i].csid == CSID_DIR,
		 "MSCP$LOADBAL goes to the directory node the vector names");
	if (i < 0) {
		engine_down();
		return;
	}
	/* Every byte a real sender writes, against the real record. body[0:8]
	 * and body[12:16] are the connection manager's (stamped at send). */
	n = 0u;
	for (k = 0u; k < BODY_LEN; k++) {
		if (k < 8u || (k >= 12u && k < 16u))
			continue;
		if (g_sent[i].body[k] != real_req[k]) {
			printf("    body[%u] ours %02x real %02x\n", k,
			       g_sent[i].body[k], real_req[k]);
			n++;
		}
	}
	ct_check_eq_u32(n, 0u,
			"the record is the real one (frame 116) in every byte "
			"outside the connection manager's envelope and header");
	ct_check(g_sent[i].body[128] == 0x09 && g_sent[i].body[129] == 0x45 &&
		 g_sent[i].body[130] == 0xa3 && g_sent[i].body[131] == 0x35,
		 "...including the hash the ENGINE computed (0x35a34509)");
	ct_check(vms_dlm_shape_fit_for_any_member(g_sent[i].body, BODY_LEN),
		 "RULE C admits it at a real member");

	/* Releasing 4 again (a retransmitted release) sends nothing new. */
	vms_dlm_rm_dr_barrier_released(&g_rm, VMS_DLM_DR_REL_STEP, 100u);
	ct_check_eq_u32(g_n_sent, 2u, "a repeated release 4 sends nothing twice");
	engine_down();
}

/* ------------------------------------------------------------------ *
 * 3: the real answer releases the barrier and records the directory.
 * ------------------------------------------------------------------ */
static void answer_from(uint32_t from, const uint8_t *body)
{
	(void)vms_dlm_rm_dr_answer(&g_rm, from, body[8], body[9], body,
				   BODY_LEN);
}

static void test_the_real_answer_is_taken(void)
{
	uint8_t other[BODY_LEN];
	int i;

	printf("-- the directory's real 82/0d answer\n");
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	transition_to(4u, 100u);
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 2u, "two out");

	ct_check(!vms_dlm_rm_dr_answer(&g_rm, CSID_OTHER, real_ans[8],
				       real_ans[9], real_ans, BODY_LEN),
		 "the answer from a node the record did not go to is not ours");
	ct_check(!vms_dlm_rm_dr_answer(&g_rm, CSID_DIR, real_req[8],
				       real_req[9], real_req, BODY_LEN),
		 "a REQUEST is not an answer");
	memcpy(other, real_ans, BODY_LEN);
	other[48] = 'X';
	ct_check(!vms_dlm_rm_dr_answer(&g_rm, CSID_DIR, other[8], other[9],
				       other, BODY_LEN),
		 "an answer naming another resource is not ours");
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 2u, "still two out");

	ct_check(vms_dlm_rm_dr_answer(&g_rm, CSID_DIR, real_ans[8],
				      real_ans[9], real_ans, BODY_LEN),
		 "the REAL 82/0d (frame 121) answers our MSCP$LOADBAL record");
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 1u, "one left");
	ct_check_eq_u32(g_rm.dr_told, 1u,
			"the engine records that directory as told");
	ct_check(!vms_dlm_rm_dr_answer(&g_rm, CSID_DIR, real_ans[8],
				       real_ans[9], real_ans, BODY_LEN),
		 "the same answer twice is not taken twice");

	i = sent_index(SECOND_NAME);
	ct_check(i >= 0, "the second record is on the wire");
	if (i >= 0) {
		/* Its answer, by the real recipe: echo, 0x82, body[34] 0xf9. */
		memcpy(other, g_sent[i].body, BODY_LEN);
		other[8] = 0x82;
		other[12] = 0x01;   /* the header the CM stamped on ours */
		other[14] = 0x03;
		other[34] = 0xf9;
		answer_from(CSID_DIR, other);
	}
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 0u,
			"every record answered: the barrier goes on");
	vms_dlm_rm_dr_barrier_released(&g_rm, 5u, 100u);
	ct_check(!g_rm.dr_open, "release 5 shuts the window");

	/* rd vms-c55, now reachable after a transition: the directory that
	 * answered is the one the last $DEQ removes the entry from. */
	(void)deq(g_lk_sub);
	g_removals = 0u;
	ct_check_eq_u32(deq(g_lk_real), SS__NORMAL, "MSCP$LOADBAL's last lock goes");
	ct_check(g_removals == 1u && g_removal_dst == CSID_DIR,
		 "*** the re-registered entry is removed from the directory "
		 "that answered ***");
	engine_down();
}

/* ------------------------------------------------------------------ *
 * The window: more resources than slots.
 * ------------------------------------------------------------------ */
static void test_the_window_refills(void)
{
	char nm[16];
	uint32_t i, lk[10], answered = 0u;

	printf("-- more resources than the window\n");
	if (!engine_up())
		return;
	for (i = 0u; i < 10u; i++) {
		snprintf(nm, sizeof(nm), "DIRREG$%02u", i);
		lk[i] = enq(nm, 0u);
	}
	table_up();
	transition_to(4u, 100u);
	ct_check_eq_u32(g_n_sent, VMS_DLM_DR_WINDOW,
			"the window's worth go at once");
	while (answered < g_n_sent && answered < MAX_SENT) {
		uint8_t a[BODY_LEN];

		memcpy(a, g_sent[answered].body, BODY_LEN);
		a[8] = 0x82;
		a[12] = 0x01;
		a[14] = 0x03;
		a[34] = 0xf9;
		answer_from(g_sent[answered].csid, a);
		answered++;
	}
	ct_check_eq_u32(g_n_sent, 10u, "each answer sends the next: all ten");
	ct_check_eq_u32(g_rm.dr_told, 10u, "...and every one is recorded told");
	ct_check_eq_u32(vms_dlm_rm_dr_outstanding(&g_rm), 0u, "none left");
	for (i = 0u; i < 10u; i++)
		(void)deq(lk[i]);
	engine_down();
}

/* ------------------------------------------------------------------ *
 * 4: what stops the window -- and never hangs the barrier.
 * ------------------------------------------------------------------ */
static void test_what_stops_it(void)
{
	uint8_t a[BODY_LEN];
	uint32_t before;

	printf("-- what stops a re-registration\n");
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	transition_to(4u, 100u);
	memcpy(a, real_ans, BODY_LEN);
	a[34] = 0xf8;
	before = g_n_sent;
	ct_check(vms_dlm_rm_dr_answer(&g_rm, CSID_DIR, a[8], a[9], a, BODY_LEN),
		 "an answer not stamped 0xf9 is ours...");
	ct_check(g_rm.dr_stopped && g_rm.dr_answers_odd == 1u &&
		 g_rm.dr_told == 0u,
		 "...not taken as held, and the window STOPS");
	ct_check_eq_u32(g_n_sent, before, "...so nothing more is sent");
	engine_down();

	/* No answer at all: RECNXINTERVAL bounds the hold. */
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	transition_to(4u, 1000u);
	ct_check_eq_u32(vms_dlm_rm_dr_expire(&g_rm, 1000u + 19999u, 20000u),
			0u, "nothing expires inside RECNXINTERVAL");
	ct_check_eq_u32(vms_dlm_rm_dr_expire(&g_rm, 1000u + 20000u, 20000u),
			2u, "both expire at RECNXINTERVAL");
	ct_check(vms_dlm_rm_dr_outstanding(&g_rm) == 0u && g_rm.dr_stopped,
		 "the barrier goes on; the window is stopped");
	g_removals = 0u;
	(void)deq(g_lk_sub);
	(void)deq(g_lk_real);
	ct_check_eq_u32(g_removals, 0u,
			"an entry no directory confirmed is never removed");
	engine_down();

	/* The way out refuses: stopped at once, nothing held. */
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	g_send_fails = 1;
	transition_to(4u, 100u);
	ct_check(g_rm.dr_stopped && g_rm.dr_send_failed == 1u &&
		 vms_dlm_rm_dr_outstanding(&g_rm) == 0u,
		 "a refused send stops the window and holds nothing");
	engine_down();

	/* Unbound doors: nothing at all, counted. */
	memset(&g_ops, 0, sizeof(g_ops));
	vms_dlm_rm_init(&g_rm, &g_ops);
	vms_dlm_rm_dr_barrier_released(&g_rm, VMS_DLM_DR_REL_STEP, 0u);
	ct_check(g_rm.dr_unbound == 1u && vms_dlm_rm_dr_outstanding(&g_rm) == 0u,
		 "with the doors unbound nothing is sent and nothing held");

	/* A new transition's start abandons what was in flight. */
	if (!masters_four()) {
		engine_down();
		return;
	}
	table_up();
	transition_to(4u, 100u);
	vms_dlm_rm_dr_close(&g_rm);
	ct_check(vms_dlm_rm_dr_outstanding(&g_rm) == 0u &&
		 g_rm.dr_abandoned == 2u,
		 "a transition's start abandons the records in flight");
	answer_from(CSID_DIR, real_ans);
	ct_check_eq_u32(g_rm.dr_told, 0u,
			"...and a late answer to one is not ours");
	engine_down();
}

int main(void)
{
	printf("test_dlm_dirreg: a master re-registers its directory entries "
	       "after release 4 (rd vms-fdeb)\n");
	test_release_4_sends_the_real_record();
	test_the_real_answer_is_taken();
	test_the_window_refills();
	test_what_stops_it();
	return ct_summary("test_dlm_dirreg");
}
