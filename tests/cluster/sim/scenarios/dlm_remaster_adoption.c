/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/dlm_remaster_adoption.c - rd vms-14fd's R2 leg: FOUR SYSTEMS, TWO
 * LOCK TREES HANDED OVER AT ONCE, AND THE HOLDERS' NEXT FRAMES SERVED OFF THE
 * TREES THIS EXECUTIVE NOW HOLDS.
 *
 * ==========================================================================
 * WHAT MAKES THIS R2 AND NOT A SECOND R1
 * ==========================================================================
 * tests/cluster/host/test_dlm_adopt.c drives one adoption to the bottom on one
 * node. Three things cannot be seen there, and each of them is a way a real
 * remaster differs from one transaction in isolation:
 *
 *   1. CONCURRENCY. A real shutdown remaster moves HUNDREDS of trees and their
 *      frames interleave on the wire -- 278 trees in lab run ci6-evac-16, and
 *      the E-captures show the per-tree sequences overlapping. Two adoptions
 *      open at once must stay two: each tree ends up holding exactly its own
 *      locks, and an abort of one must not touch the other.
 *   2. SEVERAL HOLDERS, IN TIME. RA2 measured the holders sending their own
 *      op-0x12 records to the new master independently of the old master's
 *      hand-over. Here three systems do that, interleaved, on the virtual
 *      clock.
 *   3. THE FOLLOW-UP TRAFFIC IS REAL FRAMES. After the hand-over, each
 *      holder's next request is BUILT by the shipping cat-0x02 builder and
 *      READ by the shipping parser -- not a struct this file fills in -- and
 *      served through the shipping master seam. That is the only way to see
 *      that the handles this executive answered with are handles the protocol
 *      can actually name a lock by.
 *
 * NOTHING IS MODELLED except the three VMS systems' own lock managers (they
 * are scripted: the 0x12 records carry the values a real holder reads out of
 * its own lock database) and the LAN (a function call). The ENGINE
 * (src/kernel-core/vms_lock.c), the ADOPTION DOOR (vms_dlm_adopt.h), the
 * MASTER SEAM (vms_dlm_master.h) and the CODEC
 * (src/kernel-core/vms_cluster_codec_dlm.c, both ways) are the shipping
 * objects. The codec that turns a real op-0x12 into one of these records is rd
 * vms-380 and is deliberately NOT here: this file is about what the engine
 * does with the records, which is what makes the 82/01 answer honest.
 *
 * AND NO op-0x14 IS SENT, ON PURPOSE (INV-6). Two of its cells, body[20:24]
 * and body[28:40], are ungrounded against real captures. Section 3 asserts
 * that across the whole run this node put NOTHING on the wire toward the old
 * master, and that it COUNTS every adoption it therefore could not report.
 */
#include <stdio.h>
#include <string.h>

/* lock_shim/ is FIRST on this target's include path, so these two resolve to
 * FC-P4.9's host backend -- the same recipe scenarios/dlm_requester.c uses. */
#include "vms_internal.h"
#include "exec_kbackend.h"

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster_codec.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_adopt.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"

/* ==========================================================================
 * 0. The four systems and the virtual clock
 * ========================================================================== */

uint32_t vms_local_csid = 0x00010002u;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_OLD   0x00010001u   /* VAX1: the master handing the trees over */
#define CSID_OVMX  0x00010002u   /* this node: the chosen new master        */
#define CSID_H1    0x00010003u   /* VAX2: a holder in both trees            */
#define CSID_H2    0x00010004u   /* VAX3: a holder in the second tree       */

#define T1_NAME "RMT5"
#define T2_NAME "F11B$aSYSDSK1"
#define RES_GROUP 0u
#define RES_MODE  3u

/* The holders' OWN handles -- their values, never minted here. */
#define H1_T1_ROOT 0x110000CEu
#define H1_T1_S0   0x120000A7u
#define H1_T2_ROOT 0x140000B2u
#define H2_T2_ROOT 0x210000B1u
#define H2_T2_S0   0x220000B3u

static struct sim_clock g_clock;
static struct vms_proc g_delivery;

static void clock_advance(uint32_t ms)
{
	g_clock.now_ms += (uint64_t)ms;
}

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

/*
 * THE WIRE THIS NODE IS WATCHED ON. Nothing in the adoption path may emit, so
 * the counter below is the whole "sent" model: any send op this file installs
 * bumps it, and section 3 asserts it never moved.
 */
static uint32_t g_frames_sent;

static uint32_t sim_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	(void)p;
	g_frames_sent++;
	return SS__NORMAL;
}

static void ovmx_up(void)
{
	static struct vms_dlm_requester_ops ops;

	vms_local_csid = CSID_OVMX;
	g_frames_sent = 0u;
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return;
	}
	memset(&ops, 0, sizeof(ops));
	ops.post = sim_post;
	vms_lock_dlm_set_requester_ops(&ops);
	proc_init(&g_delivery);
	g_delivery.current_mode = PSL_C_USER;
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	vms_lock_dlm_set_local_csid(CSID_OVMX);
}

static void ovmx_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ==========================================================================
 * 1. The scripted hand-over: a tree and its op-0x12 records
 * ========================================================================== */

static struct vms_dlm_adopt_tree tree_of(const char *name)
{
	struct vms_dlm_adopt_tree t;

	memset(&t, 0, sizeof(t));
	snprintf(t.resnam, sizeof(t.resnam), "%s", name);
	t.old_master_csid = CSID_OLD;
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

/* One record, delivered at the current virtual time. Returns the handle this
 * engine minted, or 0 when the engine refused. */
static uint32_t deliver(const struct vms_dlm_adopt_tree *t,
			const struct vms_dlm_adopt_lock_rec *r)
{
	struct vms_dlm_adopt_lock_result o;

	if (vms_lock_dlm_adopt_lock(t, r, &o) != SS__NORMAL)
		return 0u;
	return o.master_lkid;
}

/* ==========================================================================
 * 2. The scripted holders' REAL follow-up frames
 * ========================================================================== */

static const uint8_t *body_of(const uint8_t *f) { return f + VMS_OFF_SYSAP_BODY; }

/* An op-0x01 / op-0x07 built by the SHIPPING builder, as the holder's own
 * connection manager would put it on the wire. */
static int holder_enq_frame(uint8_t *frame, uint8_t opcode, uint32_t holder_lkid,
			    uint32_t master_lkid, uint8_t mode,
			    const char *name)
{
	struct vms_dlm_enq_request r;
	uint32_t written = 0;

	memset(&r, 0, sizeof(r));
	r.mode = mode;
	r.req_lkid = holder_lkid;
	r.master_lkid = master_lkid;
	if (name != NULL) {
		r.name_len = (uint8_t)strlen(name);
		memcpy(r.name, name, r.name_len);
	}
	/*
	 * Every cat-0x02 request this builder writes carries the identity span
	 * (rd vms-b5b0, and the builder refuses without it). What makes a
	 * CONVERT different is on the RECEIVE side: the PARSER does not report
	 * res_ident_valid for an op-0x07, because on a real VAX frame the name
	 * and the identity bytes beside it are STALE BUFFER
	 * (docs/design-dlm-name-hash.md SS4a measured two such frames whose
	 * readable name belonged to another resource entirely). So the master
	 * must take the resource from the lock the convert NAMES -- the path
	 * rd vms-14fd added -- and `name` here is whatever the scripted holder
	 * left in that span, real or stale.
	 */
	r.res_group = RES_GROUP;
	r.res_acmode = RES_MODE;
	r.res_ident_valid = 1u;
	/* body[20:24]: the scripted VMS node's REQUESTING PID until a master
	 * names a handle (rd vms-c55) */
	r.req_pid = 0x20200204u;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_request_build(&r, opcode, frame, VMS_CM_FRAME_LEN,
					 &written) == VMS_CODEC_OK ? 0 : -1;
}

/* THE MASTER ROLE (the arm's dlm_arm_serve_enq_frame), restated: parse the
 * frame with the shipping parser and hand it to the shipping master seam. */
static void served_enq(const uint8_t *frame, uint32_t from,
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
	mr.req_csid = from;
	mr.req_lkid = e.req_lkid;
	mr.master_lkid = e.master_lkid;
	mr.lkmode = e.mode;
	mr.res_group = e.res_group;
	mr.res_mode = e.res_acmode;
	mr.res_ident_valid = e.res_ident_valid;
	for (i = 0u; i < e.name_len && i < sizeof(mr.resnam) - 1u; i++)
		mr.resnam[i] = (char)e.name[i];
	mr.resnam[i] = '\0';
	(void)vms_lock_dlm_master_serve(&mr, out);
}

/* The same, for an op-0x03 release. */
static void served_deq(uint32_t holder_lkid, uint32_t master_lkid,
		       uint32_t from, struct vms_dlm_master_result *out)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_deq d, parsed;
	struct vms_dlm_master_request mr;
	uint32_t written = 0;

	memset(out, 0, sizeof(*out));
	out->outcome = (uint8_t)VMS_DLM_MASTER_REFUSED;
	memset(&d, 0, sizeof(d));
	d.req_lkid = holder_lkid;
	d.master_lkid = master_lkid;
	memset(frame, 0, sizeof(frame));
	if (vms_dlm_deq_build(&d, frame, (uint32_t)sizeof(frame), &written) !=
	    VMS_CODEC_OK)
		return;
	if (vms_dlm_deq_parse_body(body_of(frame), VMS_CM_BODY_LEN, &parsed) !=
	    VMS_CODEC_OK)
		return;
	memset(&mr, 0, sizeof(mr));
	mr.op = VMS_DLM_MREQ_DEQ;
	mr.req_csid = from;
	mr.req_lkid = parsed.req_lkid;
	mr.master_lkid = parsed.master_lkid;
	(void)vms_lock_dlm_master_serve(&mr, out);
}

static struct vms_resmaster_args resmaster(const char *name)
{
	struct vms_resmaster_args a;

	memset(&a, 0, sizeof(a));
	snprintf(a.resnam, sizeof(a.resnam), "%s", name);
	vms_ioctl_get_resmaster(&g_delivery, (unsigned long)(void *)&a);
	return a;
}

static int getlki(uint32_t lkid, struct vms_getlki_args *out)
{
	memset(out, 0, sizeof(*out));
	out->lkid = lkid;
	vms_ioctl_getlki(&g_delivery, (unsigned long)(void *)out);
	return out->status == SS__NORMAL;
}

/* The handles two interleaved hand-overs produced. */
struct handover {
	uint32_t t1_root, t1_s0;
	uint32_t t2_root_h1, t2_root_h2, t2_s0_h2;
	uint32_t t1_locks, t2_locks;
};

/*
 * THE INTERLEAVED HAND-OVER, on the virtual clock. VAX1 relieves itself of both
 * trees; VAX2 holds locks in both and VAX3 in the second, and their records
 * arrive mixed together, exactly as the E-captures show concurrent trees
 * interleaving.
 */
static struct handover interleaved_handover(void)
{
	struct vms_dlm_adopt_tree t1 = tree_of(T1_NAME);
	struct vms_dlm_adopt_tree t2 = tree_of(T2_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct handover h;

	memset(&h, 0, sizeof(h));

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t1), SS__NORMAL,
			"t=0    VAX1 hands RMT5 over");
	clock_advance(3u);
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t2), SS__NORMAL,
			"t=3    ...and F11B$aSYSDSK1, before RMT5's records are in");
	ct_check_eq_u32(vms_lock_dlm_adopt_is_open(&t1), 1,
			"       both adoptions are open at once");
	ct_check_eq_u32(vms_lock_dlm_adopt_is_open(&t2), 1, "       (the second)");

	clock_advance(5u);
	r = rec_of(T1_NAME, CSID_H1, H1_T1_ROOT, 0, LCK_K_CRMODE);
	h.t1_root = deliver(&t1, &r);
	ct_check(h.t1_root != 0u, "t=8    VAX2's RMT5 root record");

	clock_advance(2u);
	r = rec_of(T2_NAME, CSID_H2, H2_T2_ROOT, 0, LCK_K_CWMODE);
	h.t2_root_h2 = deliver(&t2, &r);
	ct_check(h.t2_root_h2 != 0u, "t=10   VAX3's SYSDSK1 root record, interleaved");

	clock_advance(1u);
	r = rec_of("S0", CSID_H1, H1_T1_S0, h.t1_root, LCK_K_CWMODE);
	h.t1_s0 = deliver(&t1, &r);
	ct_check(h.t1_s0 != 0u, "t=11   VAX2's RMT5 sub record");

	clock_advance(4u);
	r = rec_of("S0", CSID_H2, H2_T2_S0, h.t2_root_h2, LCK_K_CWMODE);
	h.t2_s0_h2 = deliver(&t2, &r);
	ct_check(h.t2_s0_h2 != 0u, "t=15   VAX3's SYSDSK1 sub record");

	clock_advance(2u);
	r = rec_of(T2_NAME, CSID_H1, H1_T2_ROOT, 0, LCK_K_CRMODE);
	h.t2_root_h1 = deliver(&t2, &r);
	ct_check(h.t2_root_h1 != 0u, "t=17   VAX2 holds SYSDSK1 too, and says so");

	clock_advance(6u);
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t1, &h.t1_locks), SS__NORMAL,
			"t=23   RMT5's end-of-records");
	clock_advance(9u);
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t2, &h.t2_locks), SS__NORMAL,
			"t=32   SYSDSK1's end-of-records");
	return h;
}

/* ==========================================================================
 * 3. TWO TREES HANDED OVER AT ONCE STAY TWO TREES
 * ========================================================================== */
static void two_trees_at_once_stay_two_trees(void)
{
	struct handover h;
	struct vms_resmaster_args rm;
	struct vms_getlki_args lki;

	printf("-- two interleaved hand-overs, four systems --\n");
	sim_clock_init(&g_clock, 5000u);
	ovmx_up();
	h = interleaved_handover();

	ct_check_eq_u32(h.t1_locks, 2, "RMT5 came over with exactly two locks");
	ct_check_eq_u32(h.t2_locks, 3,
			"SYSDSK1 came over with exactly three, from two holders");

	rm = resmaster(T1_NAME);
	ct_check_eq_u32(rm.is_local_master, 1, "this node masters RMT5");
	ct_check_eq_u32(rm.n_granted, 1, "  with VAX2's root lock on it");
	rm = resmaster(T2_NAME);
	ct_check_eq_u32(rm.is_local_master, 1, "this node masters SYSDSK1");
	ct_check_eq_u32(rm.n_granted, 2, "  with BOTH holders' root locks on it");

	/* The two sub-resources are both named S0 and belong to different
	 * trees. Neither is a root-level resource, and they are not each
	 * other: the second tree's S0 hangs off ITS root. */
	ct_check_eq_u32(resmaster("S0").found, 0,
			"no root-level resource named S0 exists");
	ct_check(h.t1_s0 != h.t2_s0_h2,
		 "the two trees' S0 locks are different locks");
	ct_check(getlki(h.t1_s0, &lki) && lki.parent_id == h.t1_root,
		 "RMT5's S0 hangs under RMT5's root");
	ct_check(getlki(h.t2_s0_h2, &lki) && lki.parent_id == h.t2_root_h2,
		 "SYSDSK1's S0 hangs under SYSDSK1's root");
	ovmx_down();
}

/* ==========================================================================
 * 4. THE HOLDERS' NEXT FRAMES ARE SERVED OFF THOSE TREES
 *
 * Real cat-0x02 frames, built by the shipping builder and read by the shipping
 * parser -- which is what makes "the handle we answered with" a handle the
 * protocol can name a lock by.
 * ========================================================================== */
static void the_holders_next_frames_are_served(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;
	struct vms_getlki_args lki;
	struct handover h;

	printf("-- the holders' next frames, over the shipping codec --\n");
	sim_clock_init(&g_clock, 6000u);
	ovmx_up();
	h = interleaved_handover();

	/* (a) an op-0x01 RETRANSMIT of VAX2's RMT5 root request. */
	clock_advance(120u);
	ct_check(holder_enq_frame(frame, VMS_DLM_WIREOP_ENQ, H1_T1_ROOT, 0u,
				  (uint8_t)LCK_K_CRMODE, T1_NAME) == 0,
		 "VAX2's op-0x01 is built by the shipping builder");
	served_enq(frame, CSID_H1, &res);
	ct_check_eq_u32(res.outcome, VMS_DLM_MASTER_GRANTED,
			"  and is GRANTED off the adopted tree");
	ct_check_eq_u32(res.master_lkid, h.t1_root,
			"*** by the very handle the hand-over minted ***");
	ct_check_eq_u32(res.req_lkid, H1_T1_ROOT,
			"  with VAX2's own handle read back off the LKB");

	/* (b) an op-0x07 CONVERT of a SUB-RESOURCE lock, with the stale
	 * identity span a real VAX sends. */
	clock_advance(40u);
	/* The name span carries the OTHER tree's root name -- stale buffer, as
	 * a real VAX's op-0x07 does. A master that read it would convert the
	 * wrong resource; this one reads the LOCK the frame names. */
	ct_check(holder_enq_frame(frame, VMS_DLM_WIREOP_CONVERT, H1_T1_S0,
				  h.t1_s0, (uint8_t)LCK_K_EXMODE, T2_NAME) == 0,
		 "VAX2's op-0x07 on the SUB lock is built, with a STALE name span");
	served_enq(frame, CSID_H1, &res);
	ct_check_eq_u32(res.outcome, VMS_DLM_MASTER_GRANTED,
			"  the sub-resource convert is served");
	ct_check_eq_u32(res.master_lkid, h.t1_s0, "  on the SAME lock");
	ct_check_eq_u32(res.granted_mode, LCK_K_EXMODE, "  at the new mode");
	ct_check(getlki(h.t1_s0, &lki) && strcmp(lki.resnam, "S0") == 0 &&
		 lki.granted_mode == LCK_K_EXMODE,
		 "*** the lock that moved is RMT5's S0, not the resource the "
		 "stale name span pointed at ***");
	ct_check_eq_u32(resmaster("S0").found, 0,
			"  and still no root-level S0 was conjured up");

	/* (c) an op-0x03 RELEASE of VAX3's SYSDSK1 sub lock. */
	clock_advance(70u);
	served_deq(H2_T2_S0, h.t2_s0_h2, CSID_H2, &res);
	ct_check_eq_u32(res.outcome, VMS_DLM_MASTER_RELEASED,
			"VAX3's op-0x03 releases its SYSDSK1 sub lock");
	ct_check(!getlki(h.t2_s0_h2, &lki), "  and the lock is really gone");
	ct_check(getlki(h.t2_root_h1, &lki),
		 "  while VAX2's lock on the same tree is untouched");

	/* (d) a release naming a lock held for ANOTHER system is refused. */
	served_deq(H2_T2_ROOT, h.t2_root_h1, CSID_H2, &res);
	ct_check_eq_u32(res.outcome, VMS_DLM_MASTER_REFUSED,
			"NEGATIVE CONTROL: VAX3 may not release VAX2's lock");
	ct_check(getlki(h.t2_root_h1, &lki), "  and it is still held");
	ovmx_down();
}

/* ==========================================================================
 * 5. A REFUSED RECORD MID-TREE LEAVES NO HALF TREE -- AND DOES NOT TOUCH THE
 *    OTHER ADOPTION
 * ========================================================================== */
static void a_refused_record_leaves_no_half_tree(void)
{
	struct vms_dlm_adopt_tree t1 = tree_of(T1_NAME);
	struct vms_dlm_adopt_tree t2 = tree_of(T2_NAME);
	struct vms_dlm_adopt_lock_rec r;
	struct vms_getlki_args lki;
	uint32_t t1_root, t2_root, t2_sub, undone = 0, n2 = 0;

	printf("-- a record refused mid-tree, with another adoption open --\n");
	sim_clock_init(&g_clock, 8000u);
	ovmx_up();

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t1), SS__NORMAL, "RMT5 opens");
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t2), SS__NORMAL, "SYSDSK1 opens");

	r = rec_of(T1_NAME, CSID_H1, H1_T1_ROOT, 0, LCK_K_CRMODE);
	t1_root = deliver(&t1, &r);
	ct_check(t1_root != 0u, "RMT5's root arrives");

	clock_advance(4u);
	r = rec_of(T2_NAME, CSID_H2, H2_T2_ROOT, 0, LCK_K_CWMODE);
	t2_root = deliver(&t2, &r);
	ct_check(t2_root != 0u, "SYSDSK1's root arrives");
	r = rec_of("S0", CSID_H2, H2_T2_S0, t2_root, LCK_K_CWMODE);
	t2_sub = deliver(&t2, &r);
	ct_check(t2_sub != 0u, "SYSDSK1's sub arrives");

	/* RMT5's next record names the OTHER tree's root as its parent --
	 * a lock of a different adoption and a different holder. */
	clock_advance(2u);
	r = rec_of("S0", CSID_H1, H1_T1_S0, t2_root, LCK_K_CWMODE);
	ct_check_eq_u32(deliver(&t1, &r), 0u,
			"*** a record pointing into the OTHER tree is refused ***");

	/* So the arm abandons RMT5's adoption. */
	ct_check_eq_u32(vms_lock_dlm_adopt_abort(&t1, &undone), SS__NORMAL,
			"the arm abandons RMT5");
	ct_check_eq_u32(undone, 1, "  releasing the one lock it had installed");
	ct_check(!getlki(t1_root, &lki), "  RMT5's root lock is gone");

	/* ...and the other adoption is entirely unaffected. */
	ct_check_eq_u32(vms_lock_dlm_adopt_is_open(&t2), 1,
			"SYSDSK1's adoption is still open");
	ct_check_eq_u32(vms_lock_dlm_adopt_installed(&t2), 2,
			"  still holding its two locks");
	ct_check(getlki(t2_root, &lki), "  its root lock is untouched");
	ct_check(getlki(t2_sub, &lki), "  its sub lock is untouched");
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t2, &n2), SS__NORMAL,
			"and SYSDSK1 completes normally");
	ct_check_eq_u32(n2, 2, "  with two locks");
	ovmx_down();
}

/* ==========================================================================
 * 6. THE OLD MASTER IS TOLD NOTHING THIS NODE CANNOT BUILD (INV-6)
 * ========================================================================== */
static void nothing_ungrounded_leaves_this_node(void)
{
	struct handover h;

	printf("-- what this node put on the wire during all of it --\n");
	sim_clock_init(&g_clock, 9000u);
	ovmx_up();
	h = interleaved_handover();
	(void)h;

	ct_check_eq_u32(g_frames_sent, 0,
			"*** NOT ONE frame left this node for the whole "
			"hand-over: the op-0x14's body[20:24] and body[28:40] "
			"are ungrounded, so none is built ***");
	ct_check_eq_u32(vms_lock_dlm_adoptions_completed(), 2,
			"two adoptions completed");
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(), 2,
			"and BOTH are counted as unreported to the old master");
	ct_check_eq_u32(vms_lock_dlm_adopt_unreported(),
			vms_lock_dlm_adoptions_completed(),
			"the gap is published as a number, not left silent");
	ovmx_down();
}

int main(void)
{
	printf("=== sim/dlm_remaster_adoption (rd vms-14fd: two trees handed "
	       "over at once, four systems, R2) ===\n");

	two_trees_at_once_stay_two_trees();
	the_holders_next_frames_are_served();
	a_refused_record_leaves_no_half_tree();
	nothing_ungrounded_leaves_this_node();

	return ct_summary("sim/dlm_remaster_adoption");
}
