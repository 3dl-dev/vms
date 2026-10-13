// SPDX-License-Identifier: GPL-2.0
/*
 * test_dlm_adopt.c - rd vms-14fd's R1: THE ENGINE ADOPTS A REMASTERED TREE.
 *
 * ==========================================================================
 * WHAT THIS PROVES, AND WHY IT IS THE GATE ON EVERY LATER RUNG
 * ==========================================================================
 * Measured on real V7.3 VAXes (tests/lab/captures/vms-380-remaster-arms-
 * 20261011/ arms RA2 and RA4, tests/lab/captures/vms-50d-ra5-14cells-20261013/
 * arm RA5, SDA truth on every node before and after): when a lock tree moves to
 * a new master N, each HOLDER sends N one op-0x12 record per lock it holds in
 * the tree -- root first, then its sub-resources -- and N answers each with
 * 82/01 carrying THE HANDLE N ITSELF MINTED (RA2: 010003BA..BD, exactly the
 * master copies SDA then showed on the holder's node).
 *
 * So the handle on the wire is only honest if the lock EXISTS here. That is
 * this file's subject: after the records, does this executive HOLD the tree --
 * real RSBs, real LKBs, the holder's real modes, the real parent links -- and
 * does it then SERVE that holder's next ENQ, CONVERT and DEQ from them?
 *
 * ==========================================================================
 * WHAT IS REAL HERE
 * ==========================================================================
 *   the ENGINE    src/kernel-core/vms_lock.c on FC-P4.9's host backend -- the
 *                 same object the executive runs. Every assertion reads state
 *                 back out through the engine's OWN entry points ($GETLKI,
 *                 GET_RESMASTER, the master seam), never off a struct this
 *                 file set.
 *   the SEAMS     src/kernel-core/vms_dlm_adopt.h (the adoption door under
 *                 test) and src/kernel-core/vms_dlm_master.h (the shipping
 *                 master-side serve the follow-ups go through).
 *
 * SIMULATED: the holders' own lock managers (they are scripted -- the records
 * carry the values a real holder reads out of its own lock database) and the
 * LAN (there is none: this is the engine door, not the wire). The CODEC that
 * parses an 0x12 into one of these records is rd vms-380 and is not here.
 *
 * ==========================================================================
 * AND THE 0x14 IS NOT HERE EITHER, ON PURPOSE (INV-6)
 * ==========================================================================
 * Completing an adoption sends nothing. Two cells of the op-0x14 that tells the
 * old master the tree is held -- body[20:24] and body[28:40] -- are ungrounded
 * (a 4,658-frame census: [20] takes 255 values, [28] 69, [32] 150; RA5's SDA
 * could not name them because the lock slot had been reused). Section 9 asserts
 * the executive COUNTS every adoption it could not report, so the gap is a
 * number and not a silence.
 */
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_dlm_adopt.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"

#include <stdio.h>
#include <string.h>

/* The executive globals vms_lock.c reads, and the AST hook it calls. */
uint32_t vms_local_csid = 0x00010002u;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

/* RA5's three systems. VAX1 was the old master, VAX2 the new one; here OVMX is
 * the new master and the two VAXes are holders -- the topology variation the
 * held-out rule says to make (two holders, not one). */
#define CSID_OLD_MASTER 0x00010001u
#define CSID_LOCAL      0x00010002u
#define CSID_HOLDER     0x00010003u
#define CSID_HOLDER2    0x00010004u

/* The holders' OWN handles. They are THEIR values: this executive stamps its
 * LKBs with them and echoes them where the protocol says the holder's handle
 * goes, and never mints one on their behalf. RA5's VAX3 root lkid was
 * 0x110000CE; these keep that shape. */
#define H_ROOT_LKID  0x110000CEu
#define H_S0_LKID    0x120000A7u
#define H_S1_LKID    0x130000A8u
#define H_S2_LKID    0x180000A9u
#define H_S0B_LKID   0x190000AAu
#define H2_ROOT_LKID 0x210000B1u

#define TREE_NAME  "RMT5"
#define RES_GROUP  0u
#define RES_MODE   0u

/* RA5's value block, written by the root holder before the move. */
static const uint8_t RMLVB[LCK_VALBLK_SIZE] = "RMLVB-012345678";

static struct vms_proc g_delivery;

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

/* ==========================================================================
 * 0. The harness: the tree, the records, and the readbacks
 * ========================================================================== */

static struct vms_dlm_adopt_tree tree_of(const char *name)
{
	struct vms_dlm_adopt_tree t;

	memset(&t, 0, sizeof(t));
	snprintf(t.resnam, sizeof(t.resnam), "%s", name);
	t.old_master_csid = CSID_OLD_MASTER;
	t.res_group = RES_GROUP;
	t.res_mode = RES_MODE;
	t.ident_valid = 1u;
	return t;
}

static struct vms_dlm_adopt_lock_rec rec_of(const char *name, uint32_t csid,
					    uint32_t lkid, uint32_t parent,
					    uint8_t mode)
{
	struct vms_dlm_adopt_lock_rec r;

	memset(&r, 0, sizeof(r));
	snprintf(r.resnam, sizeof(r.resnam), "%s", name);
	r.holder_csid = csid;
	r.holder_lkid = lkid;
	r.parent_master_lkid = parent;
	r.res_group = RES_GROUP;
	r.res_mode = RES_MODE;
	r.ident_valid = 1u;
	r.granted_mode = mode;
	r.requested_mode = mode;
	return r;
}

/* $GETLKI on a handle: the engine's own readback. Returns 0 when the handle
 * names no lock. */
static int getlki(uint32_t lkid, struct vms_getlki_args *out)
{
	memset(out, 0, sizeof(*out));
	out->lkid = lkid;
	vms_ioctl_getlki(&g_delivery, (unsigned long)(void *)out);
	return out->status == SS__NORMAL;
}

static struct vms_resmaster_args resmaster(const char *name)
{
	struct vms_resmaster_args a;

	memset(&a, 0, sizeof(a));
	snprintf(a.resnam, sizeof(a.resnam), "%s", name);
	vms_ioctl_get_resmaster(&g_delivery, (unsigned long)(void *)&a);
	return a;
}

/* One request through the SHIPPING master seam -- the same door the wire arm
 * uses for an inbound op-0x01 / op-0x07 / op-0x03. */
static struct vms_dlm_master_result serve(uint32_t op, uint32_t csid,
					  uint32_t holder_lkid,
					  uint32_t master_lkid, uint8_t mode,
					  const char *name)
{
	struct vms_dlm_master_request r;
	struct vms_dlm_master_result out;

	memset(&r, 0, sizeof(r));
	memset(&out, 0, sizeof(out));
	r.op = op;
	r.req_csid = csid;
	r.req_lkid = holder_lkid;
	r.master_lkid = master_lkid;
	r.lkmode = mode;
	if (name != NULL) {
		snprintf(r.resnam, sizeof(r.resnam), "%s", name);
		r.res_group = RES_GROUP;
		r.res_mode = RES_MODE;
		r.res_ident_valid = 1u;
	}
	(void)vms_lock_dlm_master_serve(&r, &out);
	return out;
}

static void engine_up(void)
{
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0)
		ct_check(0, "vms_lock_init");
	proc_init(&g_delivery);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	vms_lock_dlm_set_local_csid(CSID_LOCAL);
}

static void engine_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
}

/*
 * ADOPT RA5's TREE. Root at CR with the value block, three sub-resources at CW
 * (S2 under S1, so the tree is three deep and not a flat fan), then a SECOND
 * holder's root record. Returns the handles this engine minted.
 */
struct adopted {
	uint32_t root, s0, s1, s2, h2root;
	uint32_t n_locks;
};

static struct adopted adopt_ra5_tree(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	struct adopted a;

	memset(&a, 0, sizeof(a));
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL,
			"adopt_open takes the tree over");

	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	memcpy(r.valblk, RMLVB, LCK_VALBLK_SIZE);
	r.valblk_present = 1u;
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"root record adopted");
	a.root = o.master_lkid;

	r = rec_of("S0", CSID_HOLDER, H_S0_LKID, a.root, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"sub S0 adopted under the root");
	a.s0 = o.master_lkid;

	r = rec_of("S1", CSID_HOLDER, H_S1_LKID, a.root, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"sub S1 adopted under the root");
	a.s1 = o.master_lkid;

	r = rec_of("S2", CSID_HOLDER, H_S2_LKID, a.s1, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"sub S2 adopted under S1 (three deep)");
	a.s2 = o.master_lkid;

	r = rec_of(TREE_NAME, CSID_HOLDER2, H2_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"a SECOND holder's root record adopted");
	a.h2root = o.master_lkid;

	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t, &a.n_locks), SS__NORMAL,
			"adopt_end closes the transaction");
	return a;
}

/* ==========================================================================
 * 1. THE TREE IS HELD FOR REAL
 * ========================================================================== */
static void tree_is_held_for_real(void)
{
	struct adopted a;
	struct vms_getlki_args lki;
	struct vms_resmaster_args rm;

	printf("-- 1. the adopted tree is REAL executive state\n");
	engine_up();
	a = adopt_ra5_tree();

	ct_check_eq_u32(a.n_locks, 5, "five locks really installed");
	ct_check(a.root && a.s0 && a.s1 && a.s2 && a.h2root,
		 "every handle this engine answered is non-zero");
	ct_check(a.root != a.s0 && a.s0 != a.s1 && a.s1 != a.s2 &&
		 a.s2 != a.h2root && a.root != a.h2root,
		 "every handle is distinct -- five locks, not one echoed five times");

	/* Each handle names a lock the engine HOLDS, at the mode the holder
	 * really held, on the resource the record named. */
	ct_check(getlki(a.root, &lki), "the root handle names a real lock");
	ct_check_eq_u32(lki.granted_mode, LCK_K_CRMODE, "root granted CR");
	ct_check(strcmp(lki.resnam, TREE_NAME) == 0, "root lock is on RMT5");
	ct_check_eq_u32(lki.parent_id, 0, "the root lock has no parent");
	ct_check(getlki(a.s2, &lki), "the S2 handle names a real lock");
	ct_check_eq_u32(lki.granted_mode, LCK_K_CWMODE, "S2 granted CW");
	ct_check(strcmp(lki.resnam, "S2") == 0, "S2 lock is on S2");
	ct_check_eq_u32(lki.parent_id, a.s1,
			"S2's parent is OUR handle for S1, not the holder's");

	ct_check(getlki(a.s1, &lki), "the S1 handle names a real lock");
	ct_check_eq_u32(lki.parent_id, a.root, "S1's parent is OUR root handle");

	/* The tree is a TREE: the sub-resources exist UNDER the root, so no
	 * root-level resource of their name was created. A by-name lookup --
	 * which is what the pre-vms-14fd convert path did -- finds nothing. */
	rm = resmaster("S0");
	ct_check_eq_u32(rm.found, 0,
			"no ROOT-LEVEL resource named S0 was created");
	rm = resmaster("S2");
	ct_check_eq_u32(rm.found, 0,
			"no ROOT-LEVEL resource named S2 was created");

	/* The root resource is mastered HERE and holds both holders' locks,
	 * each tagged with the CSID it is held for. */
	rm = resmaster(TREE_NAME);
	ct_check_eq_u32(rm.found, 1, "the root resource exists");
	ct_check_eq_u32(rm.is_local_master, 1, "this node masters RMT5");
	ct_check_eq_u32(rm.n_granted, 2, "both holders' root locks are granted");
	ct_check(rm.remote_holder_csid == CSID_HOLDER ||
		 rm.remote_holder_csid == CSID_HOLDER2,
		 "the root grants are held FOR a remote system, not locally");

	/* The value block the root record carried is on the RESOURCE. */
	ct_check(getlki(a.root, &lki) &&
		 memcmp(lki.valblk, RMLVB, LCK_VALBLK_SIZE) == 0,
		 "the root record's LVB is on the lock the engine built");

	ct_check_eq_u32(vms_lock_dlm_adoptions_completed(), 1,
			"one adoption completed");
	engine_down();
}

/* ==========================================================================
 * 2. ONE NAME UNDER TWO PARENTS IS TWO RESOURCES
 *
 * The sharpest proof that the parent link is real and not decoration: adopt a
 * SECOND lock named S0, under S1 instead of under the root. If the engine had
 * flattened the tree they would be one resource, and converting one to EX
 * would QUEUE behind the other's CW. They are two, so it GRANTS.
 * ========================================================================== */
static void one_name_two_parents(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	struct vms_dlm_master_result m;
	struct adopted a;
	uint32_t s0b;

	printf("-- 2. one name under two parents is two resources\n");
	engine_up();

	/* Re-open the tree so the extra record belongs to a transaction. */
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open");
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	a.root = o.master_lkid;
	r = rec_of("S1", CSID_HOLDER, H_S1_LKID, a.root, LCK_K_CWMODE);
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	a.s1 = o.master_lkid;
	r = rec_of("S0", CSID_HOLDER, H_S0_LKID, a.root, LCK_K_CWMODE);
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	a.s0 = o.master_lkid;
	r = rec_of("S0", CSID_HOLDER, H_S0B_LKID, a.s1, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"a second S0, under S1, is adopted");
	s0b = o.master_lkid;
	ct_check(s0b != a.s0, "the two S0 locks have different handles");
	(void)vms_lock_dlm_adopt_end(&t, NULL);

	m = serve(VMS_DLM_MREQ_CONVERT, CSID_HOLDER, H_S0_LKID, a.s0,
		  LCK_K_EXMODE, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED,
			"converting S0-under-root to EX GRANTS "
			"(it does not contend with S0-under-S1)");
	ct_check_eq_u32(m.master_lkid, a.s0, "and keeps the same handle");
	engine_down();
}

/* ==========================================================================
 * 3. THE HOLDER'S NEXT ENQ / CONVERT / DEQ ARE SERVED
 * ========================================================================== */
static void follow_ups_are_served(void)
{
	struct adopted a;
	struct vms_dlm_master_result m;
	struct vms_getlki_args lki;
	struct vms_resmaster_args rm;

	printf("-- 3. the holders' next ENQ / CONVERT / DEQ are served\n");
	engine_up();
	a = adopt_ra5_tree();

	/* ENQ -- a retransmit of the root request. The master is idempotent:
	 * the SAME handle, never a second lock. */
	m = serve(VMS_DLM_MREQ_ENQ, CSID_HOLDER, H_ROOT_LKID, 0,
		  LCK_K_CRMODE, TREE_NAME);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED, "ENQ served: granted");
	ct_check_eq_u32(m.master_lkid, a.root,
			"and it is the ADOPTED lock, by its adopted handle");
	rm = resmaster(TREE_NAME);
	ct_check_eq_u32(rm.n_granted, 2, "no second lock was minted");

	/* CONVERT of a SUB-RESOURCE lock -- the case the by-name path could not
	 * serve. CW -> EX, on the lock the holder named. */
	m = serve(VMS_DLM_MREQ_CONVERT, CSID_HOLDER, H_S2_LKID, a.s2,
		  LCK_K_EXMODE, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED, "sub CONVERT served");
	ct_check_eq_u32(m.master_lkid, a.s2, "the handle does not change");
	ct_check_eq_u32(m.granted_mode, LCK_K_EXMODE, "and the mode really moved");
	ct_check(getlki(a.s2, &lki) && lki.granted_mode == LCK_K_EXMODE,
		 "$GETLKI agrees: S2 is held at EX");
	ct_check_eq_u32(resmaster("S2").found, 0,
			"NEGATIVE CONTROL: the convert created no root-level S2");

	/* CONVERT back DOWN -- strength given back, on the same lock. */
	m = serve(VMS_DLM_MREQ_CONVERT, CSID_HOLDER, H_S2_LKID, a.s2,
		  LCK_K_CRMODE, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED, "down-convert served");
	ct_check(getlki(a.s2, &lki) && lki.granted_mode == LCK_K_CRMODE,
		 "a DOWN-convert really lowers the granted mode");

	/* DEQ of a sub lock. */
	m = serve(VMS_DLM_MREQ_DEQ, CSID_HOLDER, H_S2_LKID, a.s2, 0, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_RELEASED, "sub DEQ served");
	ct_check(!getlki(a.s2, &lki), "and the lock is gone");

	/* DEQ of the second holder's root lock leaves the first holder's. */
	m = serve(VMS_DLM_MREQ_DEQ, CSID_HOLDER2, H2_ROOT_LKID, a.h2root, 0, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_RELEASED, "holder 2's DEQ served");
	ct_check(getlki(a.root, &lki), "holder 1's root lock is untouched");
	engine_down();
}

/* ==========================================================================
 * 4. A DOWN-CONVERT GRANTS THE SYSTEM WAITING BEHIND IT
 *
 * The evacuation property: a holder giving strength back must release whoever
 * was queued. Before rd vms-14fd a cross-node convert ran the ENQ path, whose
 * retransmit matcher answered "already granted at a mode that covers it" and
 * changed NOTHING -- so the waiter waited forever.
 * ========================================================================== */
static void down_convert_grants_the_waiter(void)
{
	struct adopted a;
	struct vms_dlm_master_result m;
	struct vms_getlki_args lki;

	printf("-- 4. a down-convert grants the system queued behind it\n");
	engine_up();
	a = adopt_ra5_tree();

	/* Take the root to EX for holder 1: nothing else may hold it. */
	m = serve(VMS_DLM_MREQ_DEQ, CSID_HOLDER2, H2_ROOT_LKID, a.h2root, 0, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_RELEASED, "holder 2 stands down");
	m = serve(VMS_DLM_MREQ_CONVERT, CSID_HOLDER, H_ROOT_LKID, a.root,
		  LCK_K_EXMODE, NULL);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED, "holder 1 takes EX");

	/* Holder 2 asks for PW: incompatible, so it QUEUES on a real queue. */
	m = serve(VMS_DLM_MREQ_ENQ, CSID_HOLDER2, H2_ROOT_LKID, 0,
		  LCK_K_PWMODE, TREE_NAME);
	ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_QUEUED, "holder 2 is queued");
	ct_check_eq_u32(m.blocking_csid, CSID_HOLDER,
			"and the master names the holder blocking it");
	{
		uint32_t queued_lkid = m.master_lkid;

		/* Holder 1 gives strength back. */
		m = serve(VMS_DLM_MREQ_CONVERT, CSID_HOLDER, H_ROOT_LKID, a.root,
			  LCK_K_NLMODE, NULL);
		ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED,
				"holder 1 converts down to NL");
		ct_check(getlki(queued_lkid, &lki) &&
			 lki.granted_mode == LCK_K_PWMODE,
			 "THE WAITER IS NOW GRANTED AT PW -- the down-convert "
			 "released it");
	}
	engine_down();
}

/* ==========================================================================
 * 5. NEGATIVE CONTROLS -- every refusal creates NO lock state
 * ========================================================================== */
static void refusals_create_nothing(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_tree other = tree_of("OTHERTREE");
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	struct vms_enq_args enq;
	uint32_t root;

	printf("-- 5. negative controls\n");
	engine_up();

	/* N1: a record with no adoption open creates nothing. */
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__IVLOCKID,
			"N1 a record outside an adoption is refused");
	ct_check_eq_u32(o.master_lkid, 0, "N1 and names no handle");
	ct_check_eq_u32(resmaster(TREE_NAME).found, 0,
			"N1 and created no resource");

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open");
	ct_check_eq_u32(vms_lock_dlm_adopt_is_open(&t), 1, "the adoption is open");

	/* N2: a second open of the same tree. */
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__UNSUPPORTED,
			"N2 a second hand-over of the same tree is refused");

	/* N3: a root record about a DIFFERENT tree. */
	r = rec_of("SOMEOTHER", CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__BADPARAM,
			"N3 a root record naming another resource is refused");
	ct_check_eq_u32(resmaster("SOMEOTHER").found, 0, "N3 created no resource");

	/* N4: a record with no holder identity. */
	r = rec_of(TREE_NAME, 0, H_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__BADPARAM,
			"N4 a record from no system is refused");
	r = rec_of(TREE_NAME, CSID_HOLDER, 0, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__BADPARAM,
			"N4 a record with no holder handle is refused");
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	r.ident_valid = 0u;
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__BADPARAM,
			"N4 a record whose resource domain is unknown is refused");

	/* The real root, so the parent checks have something to point at. */
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL, "root");
	root = o.master_lkid;

	/* N5: a sub naming a parent handle this engine never minted. */
	r = rec_of("S0", CSID_HOLDER, H_S0_LKID, 0xDEADBEEFu, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__IVLOCKID,
			"N5 a sub naming an unknown parent handle is refused");

	/* N6: a sub naming a parent held for ANOTHER holder. */
	r = rec_of(TREE_NAME, CSID_HOLDER2, H2_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"holder 2's root");
	{
		uint32_t h2root = o.master_lkid;

		r = rec_of("S0", CSID_HOLDER, H_S0_LKID, h2root, LCK_K_CWMODE);
		ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__IVLOCKID,
				"N6 a sub naming ANOTHER holder's parent is refused");
	}

	/* N7: a sub naming a LOCAL lock of this node as its parent. */
	memset(&enq, 0, sizeof(enq));
	enq.lkmode = LCK_K_CRMODE;
	snprintf(enq.resnam, sizeof(enq.resnam), "LOCALRES");
	vms_ioctl_enq(&g_delivery, (unsigned long)(void *)&enq);
	ct_check_eq_u32(enq.status, SS__NORMAL, "a local $ENQ for the control");
	r = rec_of("S0", CSID_HOLDER, H_S0_LKID, enq.lkid, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__IVLOCKID,
			"N7 a sub naming a LOCAL lock as its parent is refused");

	/* N8: a retransmitted record gets the SAME handle, not a second lock. */
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__NORMAL,
			"N8 a retransmitted root record is accepted");
	ct_check_eq_u32(o.master_lkid, root, "N8 with the SAME handle");
	ct_check_eq_u32(o.duplicate, 1, "N8 and is reported as a retransmit");
	ct_check_eq_u32(vms_lock_dlm_adopt_installed(&t), 2,
			"N8 installed two locks, not three");

	ct_check(vms_lock_dlm_adoptions_refused() >= 8,
		 "every refusal above was COUNTED");
	(void)vms_lock_dlm_adopt_end(&t, NULL);

	/* N9: an 0x12-shaped record for a tree whose adoption has closed. */
	r = rec_of(TREE_NAME, CSID_HOLDER, H_S1_LKID, root, LCK_K_CWMODE);
	ct_check_eq_u32(vms_lock_dlm_adopt_lock(&t, &r, &o), SS__IVLOCKID,
			"N9 a record after the 0x13 is refused");

	/* N10: an adoption of a tree nobody opened cannot be ended or aborted. */
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&other, NULL), SS__IVLOCKID,
			"N10 ending an adoption that was never opened is refused");
	ct_check_eq_u32(vms_lock_dlm_adopt_abort(&other, NULL), SS__IVLOCKID,
			"N10 aborting one is refused too");
	engine_down();
}

/* ==========================================================================
 * 6. THE EXECUTIVE REFUSES TO ADOPT WHEN IT CANNOT HOLD THE TREE
 * ========================================================================== */
static void refuses_without_the_means(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);

	printf("-- 6. no identity / no delivery proc -> no adoption\n");

	/* No delivery proc: a master-side LKB would be owned by nobody. */
	vms_local_csid = CSID_LOCAL;
	if (vms_lock_init() != 0)
		ct_check(0, "vms_lock_init");
	proc_init(&g_delivery);
	vms_lock_dlm_set_local_csid(CSID_LOCAL);
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__UNSUPPORTED,
			"with no delivery proc the adoption is refused");
	ct_check_eq_u32(resmaster(TREE_NAME).found, 0, "and nothing was created");
	vms_lock_cleanup();

	/* No cluster identity: every LKB would carry a placeholder CSID. */
	vms_local_csid = 0u;
	if (vms_lock_init() != 0)
		ct_check(0, "vms_lock_init");
	proc_init(&g_delivery);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__UNSUPPORTED,
			"a node with no CSID may not assert mastery");
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_cleanup();
	vms_local_csid = CSID_LOCAL;
}

/* ==========================================================================
 * 7. THE VALUE BLOCK IS NEVER ZEROED BY AN ADOPTION
 * ========================================================================== */
static void lvb_absent_leaves_the_block_alone(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	struct vms_getlki_args lki;
	uint32_t root;

	printf("-- 7. a record with no LVB leaves the resource's block alone\n");
	engine_up();
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open");

	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	memcpy(r.valblk, RMLVB, LCK_VALBLK_SIZE);
	r.valblk_present = 1u;
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	root = o.master_lkid;
	ct_check_eq_u32(o.valblk_present, 1, "the record's block is reported back");
	ct_check(memcmp(o.valblk, RMLVB, LCK_VALBLK_SIZE) == 0,
		 "and it is the block that was written");

	/* A SECOND holder's record for the same resource, carrying none. */
	r = rec_of(TREE_NAME, CSID_HOLDER2, H2_ROOT_LKID, 0, LCK_K_CRMODE);
	r.valblk_present = 0u;
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	ct_check(memcmp(o.valblk, RMLVB, LCK_VALBLK_SIZE) == 0,
		 "the resource still holds the real block");
	ct_check(getlki(root, &lki) &&
		 memcmp(lki.valblk, RMLVB, LCK_VALBLK_SIZE) == 0,
		 "sixteen zeros did NOT overwrite it");
	(void)vms_lock_dlm_adopt_end(&t, NULL);
	engine_down();
}

/* ==========================================================================
 * 8. AN ABANDONED ADOPTION UNDOES EXACTLY WHAT IT INSTALLED
 * ========================================================================== */
static void abort_undoes_exactly_its_own(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;
	struct vms_resmaster_args rm;
	struct vms_getlki_args lki;
	struct vms_enq_args enq;
	uint32_t root, s0, n = 0;

	printf("-- 8. an abandoned adoption releases exactly its own locks\n");
	engine_up();

	/* An unrelated local lock of this node, and an unrelated cross-node
	 * lock held for the SAME holder -- neither may be touched. */
	memset(&enq, 0, sizeof(enq));
	enq.lkmode = LCK_K_CRMODE;
	snprintf(enq.resnam, sizeof(enq.resnam), "UNRELATED");
	vms_ioctl_enq(&g_delivery, (unsigned long)(void *)&enq);
	{
		struct vms_dlm_master_result m =
			serve(VMS_DLM_MREQ_ENQ, CSID_HOLDER, 0x7777u, 0,
			      LCK_K_CRMODE, "ELSEWHERE");

		ct_check_eq_u32(m.outcome, VMS_DLM_MASTER_GRANTED,
				"the holder also holds a lock on another tree");
	}

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open");
	/* The root record carries the tree's value block, as RA5's did. That is
	 * also why the resource BLOCK survives the undo below -- a resource with
	 * a real value block is preserved past its last lock (resource_release)
	 * -- so the mastery question can still be asked afterwards. */
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	memcpy(r.valblk, RMLVB, LCK_VALBLK_SIZE);
	r.valblk_present = 1u;
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	root = o.master_lkid;
	r = rec_of("S0", CSID_HOLDER, H_S0_LKID, root, LCK_K_CWMODE);
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	s0 = o.master_lkid;

	ct_check_eq_u32(vms_lock_dlm_adopt_abort(&t, &n), SS__NORMAL, "abort");
	ct_check_eq_u32(n, 2, "it released exactly the two locks it installed");
	ct_check(!getlki(root, &lki), "the adopted root lock is gone");
	ct_check(!getlki(s0, &lki), "the adopted sub lock is gone");
	ct_check(getlki(enq.lkid, &lki), "this node's own local lock is untouched");
	ct_check_eq_u32(resmaster("ELSEWHERE").n_granted, 1,
			"the holder's lock on another tree is untouched");

	rm = resmaster(TREE_NAME);
	ct_check_eq_u32(rm.found, 1,
			"the root resource block survives (it holds a real LVB)");
	ct_check_eq_u32(rm.master_csid, 0,
			"and mastery of the tree went back where it was");
	ct_check_eq_u32(rm.is_local_master, 0,
			"this node no longer claims to master it");
	ct_check_eq_u32(vms_lock_dlm_adopt_is_open(&t), 0, "the adoption is closed");
	engine_down();
}

/* ==========================================================================
 * 9. THE 0x14 THIS NODE DOES NOT SEND IS COUNTED, NOT HIDDEN (INV-6)
 * ========================================================================== */
static void the_unreported_gap_is_a_number(void)
{
	struct vms_dlm_adopt_tree t = tree_of(TREE_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_dlm_adopt_lock_result o;

	printf("-- 9. every completed adoption this node could not report is counted\n");
	engine_up();
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(), 0,
			"a fresh executive has reported nothing and owes nothing");

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open");
	r = rec_of(TREE_NAME, CSID_HOLDER, H_ROOT_LKID, 0, LCK_K_CRMODE);
	(void)vms_lock_dlm_adopt_lock(&t, &r, &o);
	(void)vms_lock_dlm_adopt_end(&t, NULL);

	ct_check_eq_u32(vms_lock_dlm_adoptions_opened(), 1, "one opened");
	ct_check_eq_u32(vms_lock_dlm_adoptions_completed(), 1, "one completed");
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(), 1,
			"and ONE adoption went unreported to the old master -- "
			"body[20:24] and body[28:40] of the op-0x14 are not "
			"grounded, so no frame is sent");
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(),
			vms_lock_dlm_adoptions_completed(),
			"the gap is exactly 'all of them', stated as a number");

	/* An ABORTED adoption is not an unreported one: nothing was adopted. */
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "open again");
	(void)vms_lock_dlm_adopt_abort(&t, NULL);
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(), 1,
			"an abandoned adoption owes the old master nothing");
	engine_down();
}

int main(void)
{
	printf("=== test_dlm_adopt (rd vms-14fd, real engine, R1 host unit) ===\n");
	tree_is_held_for_real();
	one_name_two_parents();
	follow_ups_are_served();
	down_convert_grants_the_waiter();
	refusals_create_nothing();
	refuses_without_the_means();
	lvb_absent_leaves_the_block_alone();
	abort_undoes_exactly_its_own();
	the_unreported_gap_is_a_number();
	return ct_summary("test_dlm_adopt");
}
