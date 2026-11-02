/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_handover.c - rd vms-33a: THE HOLDER'S HALF OF A REMASTER, on the
 * REAL lock engine.
 *
 * THIS NODE holds locks in a tree a real VMS node masters, and that old master
 * moves the tree to a THIRD node. test_dlm_remaster_arm drives the wire table
 * against a fake engine; this file drives the engine entry points the table's
 * holder doors are bound to (vms_dlm_adopt.h, vms_lock_dlm_handover_*), on
 * src/kernel-core/vms_lock.c over the host backend, with locks that exist
 * because the shipping $ENQ and grant-receive doors made them:
 *
 *   1. a ROOT proxy (RMT5 at CR) granted by the old master O, and a SUB proxy
 *      (S2 at CW, PARID = the root) granted by O too -- rd vms-b67a's route;
 *   2. O's op-0x1a block for the root, which the record must relay;
 *   3. _next names the ROOT first, with every record field read off the LKB
 *      and RSB, and will NOT name the sub-lock until the root names the new
 *      master -- because the sub's record carries that master's handle;
 *   4. _rehome replaces the root proxy's master and handle with the new
 *      master's; THEN the sub is named, carrying that very handle;
 *   5. once both are re-homed, _next names nothing: the op-0x13 is due;
 *   6. and the point of the whole exercise (rd vms-3ac): releasing the
 *      re-homed lock is posted to the NEW master with the NEW handle, not to
 *      the old master with a handle it no longer resolves (the INVLOCKID that
 *      bugchecked a real V7.3 node).
 *
 * The handles O and N "minted" are RA5's real ones (rm5-remaster.pcap): N's
 * 82/01 for VAX3's root was 0x020004cc and for its S2 0x020004cb.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_adopt.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"

#include <stdio.h>
#include <string.h>

uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_LOCAL  1u   /* this node: the HOLDER                        */
#define CSID_O      2u   /* the old master, and this node's directory    */
#define CSID_N      3u   /* the new master the old one chose             */

#define O_ROOT_LKID 0x0400082fu   /* O's handle for our root (its grant)   */
#define O_S2_LKID   0x04000830u   /* ...and for our S2                     */
#define N_ROOT_LKID 0x020004ccu   /* RA5: VAX2's 82/01 for the root        */
#define N_S2_LKID   0x020004cbu   /* RA5: VAX2's 82/01 for S2              */

static struct vms_dlm_proxy_post g_last_post;
static uint32_t g_posts;

static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	g_last_post = *p;
	g_posts++;
	return SS__NORMAL;
}

static uint32_t eng_dir_resolve(void *ctx, uint32_t hash16, uint32_t *out)
{
	(void)ctx;
	(void)hash16;
	*out = CSID_O;
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return 1u;
}

static struct vms_dlm_requester_ops eng_ops;
static struct vms_proc g_proc, g_delivery;

static void proc_init(struct vms_proc *p)
{
	static uint32_t next_vms_pid = 0x20600216u;   /* RA5 VAX3's PID form */
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	p->vms_pid = next_vms_pid++;
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

static int engine_up(void)
{
	vms_local_csid = CSID_LOCAL;
	g_posts = 0u;
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return 0;
	}
	memset(&eng_ops, 0, sizeof(eng_ops));
	eng_ops.post = eng_post;
	eng_ops.dir_resolve = eng_dir_resolve;
	eng_ops.dir_generation = eng_dir_generation;
	vms_lock_dlm_set_requester_ops(&eng_ops);
	proc_init(&g_proc);
	g_proc.current_mode = 3u;          /* user mode, UIC group 0 */
	proc_init(&g_delivery);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	/* The cluster's hash for RMT5, as RA5's frames carry it. */
	(void)vms_lock_dlm_learn_dir_hash("RMT5", 4u, 0u, 3u, 0xc54eeaeau);
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

static uint32_t o_grants(uint32_t lkid, uint32_t o_lkid)
{
	struct vms_dlm_proxy_grant g;

	memset(&g, 0, sizeof(g));
	g.req_lkid = lkid;
	g.master_lkid = o_lkid;
	g.master_csid = CSID_O;
	return vms_lock_dlm_proxy_grant_recv(&g);
}

static struct vms_dlm_adopt_tree rmt5_from_o(void)
{
	struct vms_dlm_adopt_tree t;

	memset(&t, 0, sizeof(t));
	memcpy(t.resnam, "RMT5", 4);
	t.resnam_len = 4u;
	t.res_group = 0u;
	t.res_mode = 3u;
	t.ident_valid = 1u;
	t.old_master_csid = CSID_O;
	return t;
}

static void test_the_holder_hands_its_tree_over(void)
{
	struct vms_dlm_adopt_tree t = rmt5_from_o();
	struct vms_dlm_handover_rec r;
	struct vms_dlm_adopt_valblk v;
	struct vms_deq_args d;
	uint32_t root, s2, pending;

	printf("-- rd vms-33a: this node, a HOLDER, hands RMT5 to a new "
	       "master -- on the real engine\n");
	if (!engine_up())
		return;

	root = enq("RMT5", LCK_K_CRMODE, 0u);
	ct_check(root != 0u, "the root $ENQ makes a real proxy");
	ct_check_eq_u32(o_grants(root, O_ROOT_LKID), SS__NORMAL,
			"the old master grants it");
	s2 = enq("S2", LCK_K_CWMODE, root);
	ct_check(s2 != 0u, "the sub-lock $ENQ under it makes a proxy too");
	ct_check_eq_u32(g_last_post.dst_csid, CSID_O,
			"  (posted to the parent's master, rd vms-b67a)");
	ct_check_eq_u32(o_grants(s2, O_S2_LKID), SS__NORMAL,
			"the old master grants the sub-lock");
	pending = enq("RMT5", LCK_K_EXMODE, 0u);
	ct_check(pending != 0u, "a third $ENQ is outstanding, never granted");

	/* Before the old master's op-0x1a: no block rides. */
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 1,
		 "_next names a lock");
	ct_check_eq_u32(r.holder_lkid, root, "*** the ROOT first ***");
	ct_check_eq_u32(r.valblk_present, 0u,
			"  and no value block: the old master has not sent one");

	/* The old master's op-0x1a for the root. */
	memset(&v, 0, sizeof(v));
	v.from_csid = CSID_N;
	v.master_lkid = O_ROOT_LKID;
	v.holder_lkid = root;
	memcpy(v.valblk, "RMLVB-0123456789", 16);
	v.valblk_seq = 1u;
	v.valblk_present = 1u;
	ct_check_eq_u32(vms_lock_dlm_handover_valblk(&v), SS__IVLOCKID,
			"an op-0x1a from a system that is NOT the lock's master "
			"writes nothing");
	v.from_csid = CSID_O;
	v.master_lkid = O_ROOT_LKID + 1u;
	ct_check_eq_u32(vms_lock_dlm_handover_valblk(&v), SS__IVLOCKID,
			"...nor one naming a handle the master never gave it");
	v.master_lkid = O_ROOT_LKID;
	ct_check_eq_u32(vms_lock_dlm_handover_valblk(&v), SS__NORMAL,
			"the old master's own op-0x1a is taken");
	ct_check_eq_u32(vms_lock_dlm_handover_valblks(), 1u, "  and counted");

	/* The root's record, every field off the LKB and RSB. */
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 1 &&
		 r.holder_lkid == root, "_next still names the ROOT");
	ct_check(r.resnam_len == 4u && memcmp(r.resnam, "RMT5", 4) == 0 &&
		 r.res_group == 0u && r.res_mode == 3u,
		 "  its identity is the RSB's: RMT5, group 0, user mode");
	ct_check_eq_u32(r.holder_epid, g_proc.vms_pid,
			"  body[20:24] is the owning process's PID, off its PCB");
	ct_check(r.granted_mode == LCK_K_CRMODE &&
		 r.requested_mode == LCK_K_CRMODE, "  held at CR");
	ct_check(r.parent_lkid == 0u && r.parent_master_lkid == 0u,
		 "  no parent: a ROOT record");
	ct_check(r.valblk_present && r.valblk_seq == 1u &&
		 memcmp(r.valblk, "RMLVB-0123456789", 16) == 0,
		 "  *** and the block the old master's op-0x1a wrote onto the "
		 "RSB, with its sequence ***");
	ct_check(r.hash_known && r.dir_hash == 0xc54eeaeau,
		 "  and the tree hash this executive holds");

	/* Re-home with the wrong old master: nothing. */
	ct_check_eq_u32(vms_lock_dlm_handover_rehome(root, CSID_N, CSID_O,
						     N_ROOT_LKID),
			SS__IVLOCKID,
			"a re-home naming the wrong old master changes nothing");
	ct_check_eq_u32(vms_lock_dlm_handover_rehome(root, CSID_O, CSID_N, 0u),
			SS__BADPARAM, "a re-home to handle 0 is refused");

	/* THE ROOT'S 82/01: re-home it. */
	ct_check_eq_u32(vms_lock_dlm_handover_rehome(root, CSID_O, CSID_N,
						     N_ROOT_LKID),
			SS__NORMAL, "the new master's answer re-homes the root");
	ct_check_eq_u32(vms_lock_dlm_handover_rehome(root, CSID_O, CSID_N,
						     N_ROOT_LKID),
			SS__IVLOCKID, "  ...once: a second answer changes nothing");

	/* NOW the sub-lock is ready, carrying the new handle. */
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 1,
		 "_next names another lock");
	ct_check_eq_u32(r.holder_lkid, s2, "*** the SUB-lock, now ***");
	ct_check_eq_u32(r.parent_lkid, root,
			"  body[36:40]: our own handle for the parent");
	ct_check_eq_u32(r.parent_master_lkid, N_ROOT_LKID,
			"  *** body[40:44]: the handle the NEW master minted for "
			"the parent -- read off the re-homed parent LKB ***");
	ct_check(r.granted_mode == LCK_K_CWMODE && r.resnam_len == 2u &&
		 memcmp(r.resnam, "S2", 2) == 0, "  S2 held at CW");
	ct_check_eq_u32(r.hash_known, 0u,
			"  no hash for a sub name nobody asked a directory about");

	ct_check_eq_u32(vms_lock_dlm_handover_rehome(s2, CSID_O, CSID_N,
						     N_S2_LKID),
			SS__NORMAL, "the sub-lock's answer re-homes it");
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 0,
		 "*** nothing of ours names the old master now: the op-0x13 is "
		 "due -- and the PENDING request was never described ***");
	ct_check_eq_u32(vms_lock_dlm_handover_rehomed(), 2u,
			"two proxies re-homed, counted");

	/* rd vms-3ac: the release goes to the NEW master with ITS handle. */
	memset(&d, 0, sizeof(d));
	d.lkid = s2;
	vms_ioctl_deq(&g_proc, (unsigned long)(void *)&d);
	ct_check_eq_u32(d.status, SS__NORMAL, "the sub-lock is released");
	ct_check_eq_u32(g_last_post.dst_csid, CSID_N,
			"*** the release is posted to the NEW master ***");
	ct_check_eq_u32(g_last_post.master_lkid, N_S2_LKID,
			"*** naming the handle IT minted -- not the old master's "
			"0x04000830, which no longer resolves anywhere (the "
			"INVLOCKID bugcheck, rd vms-3ac) ***");
	engine_down();
}

static void test_a_tree_this_node_holds_nothing_in(void)
{
	struct vms_dlm_adopt_tree t = rmt5_from_o();
	struct vms_dlm_handover_rec r;

	printf("-- rd vms-33a: a tree this node holds nothing in names no "
	       "lock -- the op-0x13 is due at once\n");
	if (!engine_up())
		return;
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 0,
		 "no lock is named");
	t.old_master_csid = 0u;
	ct_check(vms_lock_dlm_handover_next(&t, CSID_N, &r) == 0,
		 "a tree with no old master is no hand-over");
	t = rmt5_from_o();
	ct_check(vms_lock_dlm_handover_next(&t, CSID_O, &r) == 0,
		 "a new master equal to the old one is refused");
	engine_down();
}

int main(void)
{
	printf("test_dlm_handover: the holder's half of a remaster, on the "
	       "real lock engine (rd vms-33a)\n");
	test_the_holder_hands_its_tree_over();
	test_a_tree_this_node_holds_nothing_in();
	return ct_summary("test_dlm_handover");
}
