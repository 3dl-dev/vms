/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_stale_directory.c - rd vms-b4d's R1: A TREE MASTERED BY HAND-OVER
 * LEAVES NO STALE DIRECTORY ENTRY, AND A NODE THE VECTOR DOES NOT DIRECT A NAME
 * TO NEVER ANSWERS "YOU MASTER IT" FOR IT.
 *
 * ==========================================================================
 * THE TWO-MASTER SEQUENCE THIS FILE MAKES UNREPEATABLE (lab1b68 wire-1610)
 * ==========================================================================
 *   1. VAX1 handed the WKLD tree to OVMXE by remaster (op 0x0e..0x14). VAX2,
 *      the name's DIRECTORY node, then named OVMXE as its master.
 *   2. OVMXE's last lock on WKLD went. It sent VAX2 nothing -- a tree mastered
 *      by hand-over carried no record that a directory held an entry for it --
 *      so VAX2 went on naming OVMXE.
 *   3. VAX1's next $ENQ: VAX2 redirected it to OVMXE (0xf8). OVMXE, which is
 *      NOT WKLD's directory node, found no entry and answered "you master it"
 *      (0xf9). VAX1 mastered WKLD locally.
 *   4. A later OVMXE $ENQ mastered it too: two masters, two EX holders.
 *
 * Two fixes, one per step, each proven here and each with its own negative
 * control in host_defects.sh:
 *   (a) dlm-b4d-adopted-tree-dir-untold: an adopted tree records which
 *       directory names it (vms_lock.c dlm_adopt_note_directory), so the
 *       op-0x04 removal goes when its last lock goes -- step 2.
 *   (b) dlm-b4d-undirected-answers-you: the directory lookup takes the
 *       vector's answer for the name (vms_dlm_dir_lookup_directed); undirected,
 *       a name with no entry is not answered at all -- step 3.
 *
 * ==========================================================================
 * WHAT IS REAL HERE
 * ==========================================================================
 *   the ENGINE     src/kernel-core/vms_lock.c on FC-P4.9's host backend: the
 *                  adoption doors (vms_dlm_adopt.h), the master-side serve the
 *                  holder's release goes through, and resource_release's own
 *                  removal post.
 *   the CLUB       a real struct vms_club with three real CSBs whose Lock
 *                  Directory Weight Vector is built by the SHIPPING
 *                  cnxman_ldwv_rebuild; which node directs a name is the real
 *                  vector over the PROVEN resource-name hash.
 *   the DIRECTORY  src/kernel-core/vms_dlm_dir.c -- the shipping table, twice:
 *                  OVMX's own, and a second instance standing in for VAX2's.
 *
 * SIMULATED: the VAXes' lock managers (scripted) and the LAN. The glue TU
 * vms_dlm_scs.c is not host-linkable; that its op-0x01 directory lookup passes
 * the vector's answer into vms_dlm_dir_lookup_directed is pinned by the source
 * scan in test_dlm_scs_arm.c.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"
#include "vms_dlm_adopt.h"
#include "vms_dlm_dir.h"
#include "vms_dlm_ldwv.h"
#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_hash.h"

#include <stdio.h>
#include <string.h>

uint32_t vms_local_csid = 0x00010004u;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* lab1b68's three systems: OVMXE the new master, VAX1 the old master and the
 * holder, VAX2 a directory node. */
#define CSID_OVMX  0x00010004u
#define CSID_VAX1  0x00010002u
#define CSID_VAX2  0x00010003u

#define VAX1_LKID  0x0a0b0001u   /* VAX1's own handle for its lock */

#define RES_GROUP 0u
#define RES_MODE  0u

/* ==========================================================================
 * 0. The harness
 * ========================================================================== */

#define MAX_POSTS 8u

struct b4d {
	struct vms_cluster       cl;
	struct cnxman_ops        cnx_ops;
	struct vms_dlm_requester_ops eng_ops;
	uint32_t now_ms;

	/* What the engine asked to be sent. */
	uint32_t n_posts;
	struct vms_dlm_proxy_post posts[MAX_POSTS];
};

static struct b4d g;
static struct vms_proc g_delivery;

static void proc_init(struct vms_proc *p)
{
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	p->vms_pid = 0x10000001u;
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

static uint32_t cnx_now(void *ctx) { (void)ctx; return g.now_ms; }
static void cnx_log(void *ctx, const char *msg) { (void)ctx; (void)msg; }

static void csb_member(vms_csid_t csid, uint8_t wt, int local)
{
	struct vms_csb *csb;

	if (local)
		csb = cnxman_club_local(&g.cl.club);
	else
		csb = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)csid, 1);
	cnxman_csb_set_csid(csb, csid);
	cnxman_csb_set_lockdirwt(csb, wt);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
}

/* Three members at LOCKDIRWT 1 each: the real vector spreads the names over
 * all three, so each test picks a name by WHICH node the vector names. */
static void club_up(void)
{
	memset(&g.cl, 0, sizeof(g.cl));
	memset(&g.cnx_ops, 0, sizeof(g.cnx_ops));
	g.cnx_ops.now_ms = cnx_now;
	g.cnx_ops.log = cnx_log;
	g.cnx_ops.ctx = &g;

	memcpy(g.cl.params.scsnode, "OVMXE", 5);
	g.cl.params.scsnode_len = 5;
	g.cl.params.scssystemid = (uint64_t)CSID_OVMX;
	g.cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&g.cl);

	csb_member(CSID_VAX1, 1u, 0);
	csb_member(CSID_VAX2, 1u, 0);
	csb_member(CSID_OVMX, 1u, 1);
	g.cl.club.local_csid = CSID_OVMX;
	g.cl.club.local_csid_valid = 1u;

	(void)cnxman_ldwv_rebuild(&g.cl.club, &g.cnx_ops);
}

/* The engine's two vector reads -- what vms_dlm_scs.c binds. */
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

/* The engine's one way out: recorded, never sent. */
static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	if (p == NULL)
		return SS__BADPARAM;
	if (g.n_posts < MAX_POSTS)
		g.posts[g.n_posts] = *p;
	g.n_posts++;
	return SS__NORMAL;
}

static void engine_up(void)
{
	memset(&g.eng_ops, 0, sizeof(g.eng_ops));
	g.now_ms = 1000u;
	g.n_posts = 0u;
	vms_local_csid = CSID_OVMX;
	club_up();
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	g.eng_ops.post           = eng_post;
	g.eng_ops.dir_resolve    = eng_dir_resolve;
	g.eng_ops.dir_generation = eng_dir_generation;
	g.eng_ops.ctx            = &g;
	vms_lock_dlm_set_requester_ops(&g.eng_ops);
	proc_init(&g_delivery);
	g_delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	vms_lock_dlm_set_local_csid(CSID_OVMX);
}

static void engine_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* The proven hash of a name, and the node the real vector directs it to
 * (CSID_OVMX for "this node"). 0 when the name is outside the coverage. */
static int name_hash(const char *name, uint32_t *h)
{
	return vms_dlm_name_hash_proven(RES_GROUP, RES_MODE,
					(const uint8_t *)name,
					(uint32_t)strlen(name), h) ==
	       VMS_DLM_HASH_OK;
}

static vms_csid_t dir_node_for(const char *name)
{
	vms_csid_t csid = 0;
	uint32_t h = 0;

	if (!name_hash(name, &h))
		return 0;
	if (vms_ldwv_resolve(&g.cl.club.ldwv, vms_ldwv_key(h), &csid) !=
	    VMS_LDWV_OK)
		return 0;
	return csid == 0u ? (vms_csid_t)CSID_OVMX : csid;
}

/* A name the real vector directs at `want`, from a fixed candidate list. */
static char g_name[16];

static const char *name_directed_at(vms_csid_t want)
{
	unsigned i;

	for (i = 0u; i < 200u; i++) {
		snprintf(g_name, sizeof(g_name), "WKLD%u", i);
		if (dir_node_for(g_name) == want)
			return g_name;
	}
	return NULL;
}

static void ident_of(const char *name, struct vms_dlm_res_ident *id)
{
	uint32_t h = 0;

	memset(id, 0, sizeof(*id));
	(void)name_hash(name, &h);
	id->hash = h;
	id->group = RES_GROUP;
	id->mode = RES_MODE;
	id->name_len = (uint8_t)strlen(name);
	memcpy(id->name, name, id->name_len);
}

/* The vector's answer for a name AT THIS NODE, read exactly as the arm reads
 * it (vms_dlm_scs.c dlm_arm_dir_is_ours). */
static int directed_here(const struct vms_dlm_res_ident *id)
{
	if (!g.cl.club.ldwv.valid)
		return 1;
	return vms_ldwv_is_ours(&g.cl.club.ldwv, vms_ldwv_key(id->hash));
}

/*
 * VAX1 HANDS THE TREE OVER (op 0x0e..0x14): one root record, VAX1's own lock
 * at EX. Returns the handle this engine minted for it.
 */
static uint32_t adopt_from_vax1(const char *name)
{
	struct vms_dlm_adopt_tree t;
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	uint32_t n = 0;

	memset(&t, 0, sizeof(t));
	snprintf(t.resnam, sizeof(t.resnam), "%s", name);
	t.resnam_len = (uint8_t)strlen(name);
	t.old_master_csid = CSID_VAX1;
	t.res_group = RES_GROUP;
	t.res_mode = RES_MODE;
	t.ident_valid = 1u;

	memset(&r, 0, sizeof(r));
	snprintf(r.resnam, sizeof(r.resnam), "%s", name);
	r.resnam_len = (uint8_t)strlen(name);
	r.holder_csid = CSID_VAX1;
	r.holder_lkid = VAX1_LKID;
	r.res_group = RES_GROUP;
	r.res_mode = RES_MODE;
	r.ident_valid = 1u;
	r.granted_mode = LCK_K_EXMODE;
	r.requested_mode = LCK_K_EXMODE;

	memset(&o, 0, sizeof(o));
	if (vms_lock_dlm_adopt_open(&t) != SS__NORMAL)
		return 0u;
	if (vms_lock_dlm_adopt_lock(&t, &r, &o) != SS__NORMAL)
		return 0u;
	if (vms_lock_dlm_adopt_end(&t, &n) != SS__NORMAL || n != 1u)
		return 0u;
	return o.master_lkid;
}

/* VAX1 releases its lock -- an op-0x03 through the shipping master seam. */
static uint32_t vax1_releases(uint32_t master_lkid)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result out;

	memset(&r, 0, sizeof(r));
	memset(&out, 0, sizeof(out));
	r.op = VMS_DLM_MREQ_DEQ;
	r.req_csid = CSID_VAX1;
	r.req_lkid = VAX1_LKID;
	r.master_lkid = master_lkid;
	return vms_lock_dlm_master_serve(&r, &out);
}

/* The one removal the engine posted, or NULL. */
static const struct vms_dlm_proxy_post *the_removal(void)
{
	uint32_t i, n = g.n_posts < MAX_POSTS ? g.n_posts : MAX_POSTS;
	const struct vms_dlm_proxy_post *found = NULL;

	for (i = 0u; i < n; i++) {
		if (g.posts[i].op == VMS_DLM_POST_DIR_REMOVE) {
			if (found != NULL)
				return NULL;   /* two removals is not "the" one */
			found = &g.posts[i];
		}
	}
	return found;
}

/* ==========================================================================
 * 1. (a) THE ADOPTED TREE'S DIRECTORY IS TOLD WHEN ITS LAST LOCK GOES
 * ========================================================================== */
static void adopted_tree_release_tells_its_directory(void)
{
	const char *name;
	const struct vms_dlm_proxy_post *rm;
	struct vms_dlm_res_ident id;
	uint32_t mlk;

	printf("-- 1. an adopted tree's last lock going sends its directory the op-0x04\n");
	engine_up();
	name = name_directed_at(CSID_VAX2);
	ct_check(name != NULL, "b4d: the real vector directs some WKLD name at VAX2");
	if (name == NULL) {
		engine_down();
		return;
	}
	ident_of(name, &id);

	/* lab1b68: VAX1 (old master) -> OVMXE (new), VAX2 the directory. */
	mlk = adopt_from_vax1(name);
	ct_check(mlk != 0u, "b4d: the tree VAX1 hands over is adopted with VAX1's EX lock");
	ct_check(vms_lock_dlm_name_mastered_here(name, (uint32_t)strlen(name),
						 RES_GROUP, RES_MODE),
		 "b4d: this node masters the adopted tree");
	ct_check_eq_u32(g.n_posts, 0u, "b4d: completing the adoption sends nothing");

	ct_check_eq_u32(vax1_releases(mlk), SS__NORMAL,
			"b4d: VAX1's release of its lock is served by the master");
	rm = the_removal();
	ct_check(rm != NULL,
		 "b4d: the last lock going posts exactly one directory removal");
	if (rm != NULL) {
		ct_check_eq_u32(rm->dst_csid, CSID_VAX2,
				"b4d: the removal goes to VAX2, the directory the vector names");
		ct_check_eq_u32(rm->to_directory, 1u,
				"b4d: the removal is addressed to a directory");
		ct_check_eq_u32(rm->master_csid, CSID_OVMX,
				"b4d: the removal names this node as the master forgetting it");
		ct_check(rm->resnam_len == id.name_len &&
			 memcmp(rm->resnam, id.name, id.name_len) == 0,
			 "b4d: the removal names the adopted tree's root");
		ct_check(rm->dir_hash_known && rm->dir_hash == id.hash,
			 "b4d: the removal carries the name's proven hash");
	}
	ct_check(!vms_lock_dlm_name_mastered_here(name, (uint32_t)strlen(name),
						  RES_GROUP, RES_MODE),
		 "b4d: after the release this node no longer masters it");
	engine_down();

	/* VARY THE TOPOLOGY: the old master IS the directory (the heldout{1,2}
	 * shape: VAX1 the directory, the removal back to VAX1). */
	engine_up();
	name = name_directed_at(CSID_VAX1);
	ct_check(name != NULL, "b4d: the real vector directs some WKLD name at VAX1");
	if (name != NULL) {
		mlk = adopt_from_vax1(name);
		ct_check(mlk != 0u && vax1_releases(mlk) == SS__NORMAL,
			 "b4d: a tree whose directory is its old master is adopted and released");
		rm = the_removal();
		ct_check(rm != NULL && rm->dst_csid == CSID_VAX1,
			 "b4d: its removal goes to the old master, the directory it named");
	}
	engine_down();

	/* ...and a tree whose directory is THIS node puts nothing on the wire:
	 * there is no other system holding an entry for it. */
	engine_up();
	name = name_directed_at(CSID_OVMX);
	ct_check(name != NULL, "b4d: the real vector directs some WKLD name at this node");
	if (name != NULL) {
		mlk = adopt_from_vax1(name);
		ct_check(mlk != 0u && vax1_releases(mlk) == SS__NORMAL,
			 "b4d: a tree this node directs is adopted and released");
		ct_check(the_removal() == NULL,
			 "b4d: no removal goes on the wire for a tree this node directs");
	}
	engine_down();
}

/* ==========================================================================
 * 2. (b) A NODE THE VECTOR DOES NOT DIRECT A NAME TO NEVER SAYS "YOU MASTER IT"
 * ========================================================================== */
static void undirected_request_is_never_you(void)
{
	struct vms_dlm_dir dir;
	struct vms_dlm_dir_entry store[16];
	struct vms_dlm_res_ident id;
	const char *name;
	vms_csid_t m = 0;
	enum vms_dlm_dir_outcome o;

	printf("-- 2. an undirected op-0x01 with no entry gets no 0xf9\n");
	engine_up();
	name = name_directed_at(CSID_VAX2);
	ct_check(name != NULL, "b4d: a name the vector directs at VAX2, for the lookup");
	if (name == NULL) {
		engine_down();
		return;
	}
	ident_of(name, &id);
	ct_check(!directed_here(&id),
		 "b4d: the vector's answer at this node is: not directed here");

	/* VAX1's op-0x01, redirected here by VAX2's stale entry. Empty table. */
	(void)vms_dlm_dir_init(&dir, store, 16u);
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAX1, CSID_OVMX,
					directed_here(&id), &m);
	ct_check(o != VMS_DLM_DIR_ANSWER_YOU,
		 "b4d: an undirected name with no entry is never answered you-master-it");
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_NONE,
			"b4d: an undirected name with no entry is not answered at all");
	ct_check(vms_dlm_dir_find(&dir, &id) == NULL,
		 "b4d: nothing is recorded for an undirected name");
	ct_check_eq_u32(dir.undirected_refused, 1u,
			"b4d: the refusal is counted (undirected_refused)");
	ct_check_eq_u32(dir.answered_you, 0u,
			"b4d: answered_you stays 0 for the undirected request");

	/* An entry naming the REQUESTER is answered YOU by a directory; this node
	 * is not one for the name, so it is not answered either. */
	(void)vms_dlm_dir_register(&dir, &id, CSID_VAX1);
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAX1, CSID_OVMX, 0, &m);
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_NONE,
			"b4d: an undirected entry naming the requester is not answered");

	/* An entry naming ANOTHER master is still a redirect to it. */
	(void)vms_dlm_dir_remove(&dir, &id, CSID_VAX1);
	(void)vms_dlm_dir_register(&dir, &id, CSID_VAX2);
	m = 0;
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAX1, CSID_OVMX, 0, &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_REDIRECT && m == CSID_VAX2,
		 "b4d: an undirected entry naming another master is still redirected");

	/* An entry naming THIS node is still this node's to serve. */
	(void)vms_dlm_dir_remove(&dir, &id, CSID_VAX2);
	(void)vms_dlm_dir_claim_self(&dir, &id, CSID_OVMX);
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAX1, CSID_OVMX, 0, &m);
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_SELF,
			"b4d: an undirected entry naming this node is served as master");

	/* THE CONTROL: the same name at the node the vector DOES direct it to is
	 * answered exactly as before -- "you master it", recorded. */
	(void)vms_dlm_dir_init(&dir, store, 16u);
	m = 0;
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAX1, CSID_OVMX, 1, &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_YOU && m == CSID_VAX1,
		 "b4d: a directed name with no entry is still answered you-master-it");
	ct_check(vms_dlm_dir_find(&dir, &id) != NULL,
		 "b4d: ...and recorded, as the directory node records it");
	ct_check_eq_u32(dir.undirected_refused, 0u,
			"b4d: a directed lookup counts no refusal");
	engine_down();
}

/* ==========================================================================
 * 3. THE WHOLE SEQUENCE: ONE MASTER, WHICHEVER WAY VAX1'S NEXT $ENQ GOES
 * ========================================================================== */
static void two_masters_cannot_form(void)
{
	struct vms_dlm_dir vax2_dir, ovmx_dir;
	struct vms_dlm_dir_entry vax2_store[16], ovmx_store[16];
	struct vms_dlm_res_ident id;
	const struct vms_dlm_proxy_post *rm;
	const char *name;
	vms_csid_t m = 0;
	enum vms_dlm_dir_outcome o;
	uint32_t mlk;

	printf("-- 3. lab1b68's sequence: adopt, release, VAX1 locks again\n");
	engine_up();
	name = name_directed_at(CSID_VAX2);
	ct_check(name != NULL, "b4d: lab1b68's topology: the name's directory is VAX2");
	if (name == NULL) {
		engine_down();
		return;
	}
	ident_of(name, &id);
	(void)vms_dlm_dir_init(&vax2_dir, vax2_store, 16u);
	(void)vms_dlm_dir_init(&ovmx_dir, ovmx_store, 16u);

	/* Step 1: the hand-over; VAX2's directory names OVMXE (wire-1610
	 * 6667/6668: VAX2 answered VAX1 0xf8 naming OVMXE). */
	mlk = adopt_from_vax1(name);
	ct_check(mlk != 0u, "b4d: step 1, OVMXE adopts the tree VAX1 hands over");
	(void)vms_dlm_dir_register(&vax2_dir, &id, CSID_OVMX);

	/* Step 2: OVMXE's last lock goes, and VAX2 acts on what it is sent. */
	ct_check_eq_u32(vax1_releases(mlk), SS__NORMAL,
			"b4d: step 2, the last lock on the adopted tree is released");
	rm = the_removal();
	if (rm != NULL && rm->dst_csid == CSID_VAX2)
		(void)vms_dlm_dir_remove(&vax2_dir, &id, (vms_csid_t)rm->master_csid);
	ct_check(vms_dlm_dir_find(&vax2_dir, &id) == NULL,
		 "b4d: step 2, VAX2's directory no longer names OVMXE");

	/* Step 3: VAX1's next $ENQ, at VAX2: VAX1 masters it, VAX2 records it. */
	o = vms_dlm_dir_lookup(&vax2_dir, &id, CSID_VAX1, CSID_VAX2, &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_YOU && m == CSID_VAX1,
		 "b4d: step 3, VAX2 answers VAX1 you-master-it, not a redirect to OVMXE");

	/* And had VAX2 still redirected VAX1 here (a directory that missed the
	 * removal), OVMXE -- not the name's directory -- makes no second master. */
	m = 0;
	o = vms_dlm_dir_lookup_directed(&ovmx_dir, &id, CSID_VAX1, CSID_OVMX,
					directed_here(&id), &m);
	ct_check(o != VMS_DLM_DIR_ANSWER_YOU,
		 "b4d: step 3', a redirected op-0x01 at OVMXE is not answered you-master-it");
	ct_check(!vms_lock_dlm_name_mastered_here(name, (uint32_t)strlen(name),
						  RES_GROUP, RES_MODE),
		 "b4d: OVMXE masters nothing once its tree is released");
	engine_down();
}

int main(void)
{
	printf("=== test_dlm_stale_directory (rd vms-b4d, real engine + vector + directory) ===\n");
	adopted_tree_release_tells_its_directory();
	undirected_request_is_never_you();
	two_masters_cannot_form();
	return ct_summary("test_dlm_stale_directory");
}
