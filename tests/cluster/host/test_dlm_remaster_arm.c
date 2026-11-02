// SPDX-License-Identifier: GPL-2.0
/*
 * test_dlm_remaster_arm.c - the NEW MASTER's remaster table, rung R1
 * (rd vms-ed5, under rd vms-50d).
 *
 * WHAT THIS PROVES, and how it is kept honest:
 *
 *   EVERY INBOUND FRAME IS A REAL VAX V7.3 FRAME. The five steps are the
 *   clean-room specimens of ONE REAL REMASTER EPISODE -- rm5-remaster.pcap's
 *   RMT5, old master 0104, new master 309a, holder 1332 -- replayed into the
 *   table in the order the capture shows. Nothing is composed.
 *
 *   THE ENGINE IS A FAKE, AND IT IS MUTATED BETWEEN FRAMES. The handle the
 *   fake "mints" CHANGES on every record, and the test asserts the answer on
 *   the wire follows it. A table that cached a handle, echoed the holder's, or
 *   derived one from a counter goes RED here -- the same discipline
 *   test_dlm_requester.c applies to the proxy LKB (FC-P4.6).
 *
 *   THE TWO TIMING RULES ARE ASSERTED AS RULES. W1: an op-0x14 goes to each
 *   system that sent an op-0x13 and to nobody else. W2: none goes out until
 *   the last one is in -- which is the rule that stops the table closing the
 *   adoption on the OLD MASTER's 0x13 and then refusing four locks a real VAX
 *   still believes it holds (what RA8's interleave would have caused).
 *   tools/cluster/remaster_census.py --selftest pins both on the corpus; this
 *   file pins that the code implements them.
 *
 *   AND THE REFUSALS, which are the point of the whole item: a tree that
 *   cannot be opened is NOT answered; a record whose parent handle this engine
 *   never minted takes the WHOLE tree back; a completed tree this executive
 *   does not master is announced to NOBODY.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_cm.h"
#include "vms_dlm_rm_fsm.h"
#include "conformance_dump.h"

#include <stdio.h>
#include <string.h>

/* The SS$ values the ARM compares against live in the arm; this file's fake
 * engine answers the table's own 1/0 contract (vms_dlm_rm_fsm.h). */
#define RMT_O_CSID  0x00010001u      /* the old master, RMT5's 0104  */
#define RMT_H_CSID  0x00010003u      /* the holder, RMT5's 1332      */
#define RMT_B_CSID  0x00010004u      /* a third member that holds nothing */

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

static const uint8_t *fxbody(const struct vms_fixture *f)
{
	return f->bytes + VMS_OFF_SYSAP_BODY;
}

/* ==========================================================================
 * THE FAKE EXECUTIVE. Every door the table reaches the real one through, and
 * a log of what it was asked -- so a wire byte can be traced to the read that
 * produced it.
 * ========================================================================== */

#define FAKE_MAX_SENT   8
#define FAKE_MAX_LOCKS  8

struct fake {
	/* what the CLUB answers */
	uint32_t members[VMS_DLM_RM_MEMBERS];
	uint32_t n_members;
	uint32_t members_overflow;    /* 1 = answer max+1 (too many to name) */

	/* what the ENGINE does */
	int      open_ok;
	int      lock_ok;
	int      end_ok;
	int      mastered;
	int      hash_known;
	uint32_t hash;
	/* WHO IS THE NAME'S DIRECTORY NODE (rd vms-083): `dir_known` 0 is an
	 * executive that cannot name it, and `dir_csid` 0 is THIS node. */
	int      dir_known;
	uint32_t dir_csid;
	uint32_t dir_len;             /* the COUNT the FSM asked with */
	uint32_t dir_asked;           /* how many times it asked      */
	uint32_t next_lkid;           /* MUTATED: the handle the fake mints */
	uint8_t  residue;
	int      valblk_ok;           /* rd vms-26e: the engine takes the 0x1a */
	uint32_t valblks;             /* ...how many times it was asked       */
	struct vms_dlm_adopt_valblk last_valblk;

	/* what really happened */
	uint32_t opens, ends, aborts, reports;
	/* the name COUNTS the FSM asked the engine with (rd vms-326) */
	uint32_t mastered_len, hash_len;
	struct vms_dlm_adopt_tree  opened;
	struct vms_dlm_adopt_lock_rec last_rec;
	uint32_t n_locks;
	struct { uint32_t lkid; uint32_t csid; } minted[FAKE_MAX_LOCKS];

	/* the parent-handle -> tree map the engine owns in the real executive */
	uint32_t parent_lkid;
	struct vms_dlm_adopt_tree parent_tree;

	/* staged replies and originated frames */
	uint8_t  staged[VMS_CM_BODY_LEN];
	uint32_t n_staged;
	struct { uint32_t csid; uint8_t body[VMS_CM_BODY_LEN]; } sent[FAKE_MAX_SENT];
	uint32_t n_sent;
	int      send_fails;
};

static struct fake g_f;

static uint32_t fk_members(void *ctx, uint32_t *csids, uint32_t max)
{
	struct fake *f = (struct fake *)ctx;
	uint32_t i;

	if (f->members_overflow)
		return max + 1u;
	for (i = 0u; i < f->n_members && i < max; i++)
		csids[i] = f->members[i];
	return f->n_members;
}

static int fk_open(void *ctx, const struct vms_dlm_adopt_tree *t)
{
	struct fake *f = (struct fake *)ctx;

	if (!f->open_ok)
		return 0;
	f->opened = *t;
	f->opens++;
	return 1;
}

static int fk_lock(void *ctx, const struct vms_dlm_adopt_tree *t,
		   const struct vms_dlm_adopt_lock_rec *r,
		   struct vms_dlm_adopt_lock_result *out)
{
	struct fake *f = (struct fake *)ctx;
	uint32_t i;

	(void)t;
	memset(out, 0, sizeof(*out));
	if (!f->lock_ok)
		return 0;
	f->last_rec = *r;
	/*
	 * THE HANDLE THIS "ENGINE" MINTS MOVES EVERY TIME, and the result is
	 * filled from the engine's own state and not from the record -- which
	 * is what makes the assertion below ("the answer carries what the
	 * engine minted") capable of failing.
	 */
	out->master_lkid = f->next_lkid;
	f->next_lkid += 0x11u;
	out->holder_lkid = r->holder_lkid;
	out->granted_mode = r->granted_mode;
	out->valblk_present = r->valblk_present;
	out->valblk_seq = r->valblk_seq;
	for (i = 0u; i < VMS_DLM_VALBLK_LEN; i++)
		out->valblk[i] = r->valblk[i];
	if (f->n_locks < FAKE_MAX_LOCKS) {
		f->minted[f->n_locks].lkid = out->master_lkid;
		f->minted[f->n_locks].csid = r->holder_csid;
	}
	f->n_locks++;
	return 1;
}

static int fk_end(void *ctx, const struct vms_dlm_adopt_tree *t,
		  uint32_t *n_locks)
{
	struct fake *f = (struct fake *)ctx;

	(void)t;
	if (n_locks != NULL)
		*n_locks = f->n_locks;
	if (!f->end_ok)
		return 0;
	f->ends++;
	return 1;
}

static int fk_abort(void *ctx, const struct vms_dlm_adopt_tree *t,
		    uint32_t *n_undone)
{
	struct fake *f = (struct fake *)ctx;

	(void)t;
	if (n_undone != NULL)
		*n_undone = f->n_locks;
	f->aborts++;
	return 1;
}

static int fk_tree_of_lkid(void *ctx, uint32_t lkid,
			   struct vms_dlm_adopt_tree *out)
{
	struct fake *f = (struct fake *)ctx;

	memset(out, 0, sizeof(*out));
	if (lkid == 0u || lkid != f->parent_lkid)
		return 0;
	*out = f->parent_tree;
	return 1;
}

static void fk_reported(void *ctx)
{
	((struct fake *)ctx)->reports++;
}

static int fk_mastered(void *ctx, const char *resnam, uint32_t resnam_len,
		       uint16_t group, uint8_t mode)
{
	struct fake *f = (struct fake *)ctx;

	(void)resnam;
	(void)group;
	(void)mode;
	/* The name's COUNT the FSM asked with (rd vms-326), recorded so the
	 * test can assert the question was about the identity's own length and
	 * not about a C-string prefix of it. */
	f->mastered_len = resnam_len;
	return f->mastered;
}

static int fk_hash(void *ctx, const char *resnam, uint32_t resnam_len,
		   uint16_t group, uint8_t mode, uint32_t *out)
{
	struct fake *f = (struct fake *)ctx;

	(void)resnam;
	(void)group;
	(void)mode;
	f->hash_len = resnam_len;
	if (!f->hash_known)
		return 0;
	*out = f->hash;
	return 1;
}

/* The tree's DIRECTORY NODE, as the real door composes it out of the engine's
 * hash and the club's weight vector (rd vms-083). */
static int fk_dir_node(void *ctx, const char *resnam, uint32_t resnam_len,
		       uint16_t group, uint8_t mode, uint32_t *out)
{
	struct fake *f = (struct fake *)ctx;

	(void)resnam;
	(void)group;
	(void)mode;
	f->dir_len = resnam_len;
	f->dir_asked++;
	if (!f->dir_known)
		return 0;
	*out = f->dir_csid;
	return 1;
}

static int fk_stage(void *ctx, const uint8_t *body, uint32_t len)
{
	struct fake *f = (struct fake *)ctx;

	if (len != VMS_CM_BODY_LEN)
		return -1;
	memcpy(f->staged, body, VMS_CM_BODY_LEN);
	f->n_staged++;
	return 0;
}

static int fk_send(void *ctx, uint32_t csid, const uint8_t *body, uint32_t len)
{
	struct fake *f = (struct fake *)ctx;

	if (f->send_fails || len != VMS_CM_BODY_LEN ||
	    f->n_sent >= FAKE_MAX_SENT)
		return -1;
	f->sent[f->n_sent].csid = csid;
	memcpy(f->sent[f->n_sent].body, body, VMS_CM_BODY_LEN);
	f->n_sent++;
	return 0;
}

static uint8_t fk_residue(void *ctx)
{
	return ((struct fake *)ctx)->residue;
}

static int fk_valblk(void *ctx, const struct vms_dlm_adopt_valblk *v)
{
	struct fake *f = (struct fake *)ctx;

	f->last_valblk = *v;
	f->valblks++;
	return f->valblk_ok;
}

static void fk_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

/* The holder doors are left UNBOUND here: every new-master test above runs on
 * a table whose holder half is off, exactly as before rd vms-33a. The holder
 * tests bind them (section 9). */
static const struct vms_dlm_rm_ops g_ops_template = {
	&g_f, fk_members, fk_open, fk_lock, fk_end, fk_abort, fk_tree_of_lkid,
	fk_reported, fk_mastered, fk_hash, fk_dir_node, fk_stage, fk_send,
	fk_residue, fk_valblk, fk_log, NULL, NULL, NULL,
	/* rd vms-874f: the rebuild doors, unbound here too (test_dlm_rebuild
	 * binds them); rd vms-bcb's rebuild-step door among them. */
	NULL, NULL, NULL, NULL, NULL, NULL
};

static struct vms_dlm_rm_ops g_ops;
static struct vms_dlm_rm g_rm;

/* A fresh executive with three members: the old master, the holder, and a
 * bystander that holds nothing in the tree. */
static void rig_reset(void)
{
	memset(&g_f, 0, sizeof(g_f));
	g_f.members[0] = RMT_O_CSID;
	g_f.members[1] = RMT_H_CSID;
	g_f.n_members = 2u;
	g_f.open_ok = 1;
	g_f.lock_ok = 1;
	g_f.end_ok = 1;
	g_f.mastered = 1;
	g_f.hash_known = 1;
	g_f.hash = 0xabcd1234u;
	/*
	 * THE DEFAULT RIG IS rd vms-083's ARM A -- the arrangement the driven
	 * lab arm measured TWO op-0x13 senders in: the tree's directory node is
	 * the THIRD member (H), not the old master. The owed set is then
	 * {O, H} and every expectation written before the arm holds unchanged.
	 * Arm B (the directory IS the old master, so the third member owes
	 * nothing) is its own case below.
	 */
	g_f.dir_known = 1;
	g_f.dir_csid = RMT_H_CSID;
	g_f.next_lkid = 0x01000401u;
	g_f.valblk_ok = 1;
	g_ops = g_ops_template;
	g_ops.ctx = &g_f;
	vms_dlm_rm_init(&g_rm, &g_ops);
}

/* Replay one specimen's body as if it arrived from `csid`. */
static enum vms_dlm_rm_status step(const char *spec, uint32_t csid)
{
	const struct vms_fixture *f = fixture(spec);
	const uint8_t *b;

	if (f == NULL) {
		printf("  FAIL missing specimen %s\n", spec);
		return VMS_DLM_RM_REFUSED;
	}
	b = fxbody(f);
	return vms_dlm_rm_step(&g_rm, csid, b[8], b[9], b, VMS_CM_BODY_LEN);
}

static uint32_t le32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ==========================================================================
 * 1. THE HAND-OVER OPENS, from the real op-0x0e
 * ========================================================================== */
static void test_open_drives_the_engine(void)
{
	const struct vms_fixture *req = fixture("rm-0e-req");
	const struct vms_fixture *ans = fixture("rm-0e-ans");
	enum vms_dlm_rm_status st;

	printf("-- the real op-0x0e opens a real adoption\n");
	if (req == NULL || ans == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	st = step("rm-0e-req", RMT_O_CSID);
	ct_check(st == VMS_DLM_RM_HANDLED, "  the step is handled");
	ct_check_eq_u32(g_f.opens, 1, "  the ADOPTION ENGINE was opened once");
	ct_check(strcmp(g_f.opened.resnam, "RMT5") == 0,
		 "  ...for the tree the frame names (RMT5)");
	ct_check_eq_u32(g_f.opened.old_master_csid, RMT_O_CSID,
			"  ...handed over by the CM's own sender identity");
	ct_check_eq_u32(g_f.opened.res_group, fxbody(req)[44] |
			((uint32_t)fxbody(req)[45] << 8),
			"  ...with the frame's UIC group");
	ct_check_eq_u32(g_f.opened.res_mode, fxbody(req)[46],
			"  ...and the frame's access mode");
	ct_check(g_f.opened.ident_valid != 0u,
		 "  ...a COMPLETE identity: an adoption is never defaulted");
	ct_check_eq_u32(g_f.n_staged, 1, "  one answer staged");
	ct_check(memcmp(g_f.staged + 4, fxbody(ans) + 4,
			VMS_CM_BODY_LEN - 4) == 0,
		 "  *** and it is byte-identical to the real VAX's 82/15 ***");
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_OPEN), 1,
			"  one tree is open here");
}

/*
 * THE REFUSAL THAT MATTERS MOST: a node that cannot adopt the tree must not
 * answer the 0x0e, because the answer is this node saying it will take it.
 */
static void test_open_refused_is_not_answered(void)
{
	printf("-- NEGATIVE CONTROL: an adoption the engine refuses is NOT "
	       "acknowledged\n");
	rig_reset();
	g_f.open_ok = 0;
	ct_check(step("rm-0e-req", RMT_O_CSID) == VMS_DLM_RM_REFUSED,
		 "  the step is REFUSED");
	ct_check_eq_u32(g_f.n_staged, 0,
			"  *** nothing is answered: the old master is not "
			"told this node will take a tree it cannot hold ***");
	ct_check_eq_u32(g_rm.opens_refused, 1, "  and it is counted");

	printf("-- NEGATIVE CONTROL: a membership this node cannot name\n");
	rig_reset();
	g_f.members_overflow = 1u;
	ct_check(step("rm-0e-req", RMT_O_CSID) == VMS_DLM_RM_REFUSED,
		 "  a cluster wider than the owed set refuses the tree");
	ct_check_eq_u32(g_f.opens, 0, "  the engine was never asked");
	ct_check_eq_u32(g_f.n_staged, 0, "  and nothing was answered");

	printf("-- NEGATIVE CONTROL: a sender the CM could not name\n");
	rig_reset();
	{
		const struct vms_fixture *f = fixture("rm-0e-req");
		const uint8_t *b = f != NULL ? fxbody(f) : NULL;

		ct_check(b != NULL && vms_dlm_rm_step(&g_rm, 0u, b[8], b[9], b,
						      VMS_CM_BODY_LEN) ==
			 VMS_DLM_RM_REFUSED,
			 "  a hand-over from CSID 0 is refused (spec S4(a))");
		ct_check_eq_u32(g_f.opens, 0, "  the engine was never asked");
	}
}

/* ==========================================================================
 * 2. THE op-0x0f ROLE, read off this node's own adoption
 * ========================================================================== */
static void test_role_comes_from_our_own_state(void)
{
	const struct vms_fixture *nm = fixture("rm-0f-newmaster-ans");
	const struct vms_fixture *pm = fixture("rm-0f-member-ans");

	printf("-- the op-0x0f answer's role is an executive read, not a frame "
	       "field\n");
	if (nm == NULL || pm == NULL) {
		ct_check(0, "specimens load");
		return;
	}

	/* With no tree open, the SAME request is answered as a plain member. */
	rig_reset();
	ct_check(step("rm-0f-newmaster-req", RMT_O_CSID) ==
		 VMS_DLM_RM_HANDLED, "  answered with no tree open");
	ct_check(memcmp(g_f.staged + 28, "\0\0\0\0", 4) != 0,
		 "  *** body[28:32] is KEPT: this node is not the new master "
		 "and does not claim to be ***");
	ct_check_eq_u32(g_rm.roles_as_new_master, 0,
			"  no new-master role was asserted");

	/* Open the tree and the same request gets the new master's answer. */
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-0f-newmaster-req", RMT_O_CSID) ==
		 VMS_DLM_RM_HANDLED, "  answered with the tree open");
	ct_check(memcmp(g_f.staged + 4, fxbody(nm) + 4,
			VMS_CM_BODY_LEN - 4) == 0,
		 "  *** and it is byte-identical to the real NEW MASTER's "
		 "answer, body[28:32] cleared ***");
	ct_check_eq_u32(g_rm.roles_as_new_master, 1, "  counted as the role");
}

/* ==========================================================================
 * 3. THE RECORDS: the 82/01 carries the handle THIS engine minted
 * ========================================================================== */
static void test_record_answer_follows_the_engine(void)
{
	const struct vms_fixture *req = fixture("rm-12-root-req");
	uint32_t first;

	printf("-- the op-0x12 record drives the engine and the answer follows "
	       "it\n");
	if (req == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the holder's ROOT record is handled");
	ct_check_eq_u32(g_f.n_locks, 1, "  one lock really adopted");

	/* The record the engine was handed. */
	ct_check_eq_u32(g_f.last_rec.holder_csid, RMT_H_CSID,
			"  holder_csid is the CM's sender identity, not a "
			"body field");
	ct_check_eq_u32(g_f.last_rec.holder_lkid, le32(fxbody(req) + 24),
			"  holder_lkid is body[24:28], the holder's own handle");
	ct_check_eq_u32(g_f.last_rec.granted_mode, fxbody(req)[30],
			"  granted mode is body[30]");
	ct_check_eq_u32(g_f.last_rec.parent_master_lkid, 0,
			"  a ROOT record names no parent");
	ct_check(g_f.last_rec.valblk_present != 0u,
		 "  the record's VALUE BLOCK reaches the engine");
	ct_check(memcmp(g_f.last_rec.valblk, fxbody(req) + 88, 16) == 0,
		 "  *** and it is the block the record carried at "
		 "body[88:104] -- SDA's 'RMLVB-0123456789' ***");
	ct_check_eq_u32(g_f.last_rec.valblk_seq, le32(fxbody(req) + 104),
			"  ...with its sequence from body[104:108]");

	/* The answer. */
	first = g_f.minted[0].lkid;
	ct_check_eq_u32(le32(g_f.staged + 20), first,
			"  *** the answer's body[20:24] is the handle THE "
			"ENGINE minted ***");
	ct_check_eq_u32(le32(g_f.staged + 24), le32(fxbody(req) + 24),
			"  body[24:28] echoes the holder's handle: the "
			"correlation");
	ct_check(memcmp(g_f.staged + 36, fxbody(req) + 88, 16) == 0,
		 "  body[36:52] is the RSB's block, read back");

	/*
	 * MUTATE THE ENGINE AND REPLAY. The same record answered a second time
	 * must carry whatever the engine says NOW -- a table that cached the
	 * first handle fails here.
	 */
	g_f.next_lkid = 0x0200BEEFu;
	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the record is replayed");
	ct_check_eq_u32(le32(g_f.staged + 20), 0x0200BEEFu,
			"  *** the answer follows the MUTATED engine: nothing "
			"here caches a handle ***");
	ct_check(le32(g_f.staged + 20) != first,
		 "  ...and it is not the first answer's handle");
}

static void test_sub_record_is_placed_by_the_engine(void)
{
	const struct vms_fixture *req = fixture("rm-12-sub-req");

	printf("-- a SUB record is placed in its tree by the ENGINE's own "
	       "parent lookup\n");
	if (req == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	/* The engine says this parent handle belongs to the tree it opened. */
	g_f.parent_lkid = le32(fxbody(req) + 40);
	g_f.parent_tree = g_f.opened;

	ct_check(step("rm-12-sub-req", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the sub record is handled");
	ct_check_eq_u32(g_f.n_locks, 1, "  one lock adopted");
	ct_check_eq_u32(g_f.last_rec.parent_master_lkid,
			le32(fxbody(req) + 40),
			"  the parent handle is body[40:44], ours to verify");

	printf("-- NEGATIVE CONTROL: a parent handle this engine never minted\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	g_f.parent_lkid = 0u;        /* the engine knows no such handle */
	ct_check(step("rm-12-sub-req", RMT_H_CSID) == VMS_DLM_RM_REFUSED,
		 "  the record is REFUSED");
	ct_check_eq_u32(g_f.n_locks, 0, "  no lock was created");
	ct_check_eq_u32(g_f.n_staged, 1,
			"  *** and no answer was staged for it (the 0x0e's is "
			"the only one) ***");
}

static void test_a_refused_record_takes_the_whole_tree_back(void)
{
	printf("-- a tree that cannot be adopted WHOLE is adopted NOT AT ALL\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	g_f.lock_ok = 0;
	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_REFUSED,
		 "  the record is refused");
	ct_check_eq_u32(g_f.aborts, 1,
			"  *** the WHOLE tree is given back to its old "
			"master ***");
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_OPEN), 0,
			"  no tree is open here any more");
	ct_check_eq_u32(g_rm.records_refused, 1, "  counted");
	/* And a later record for it finds nothing to join. */
	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_REFUSED,
		 "  a later record for that tree is refused too");
}

/* ==========================================================================
 * 4. RULES W1 AND W2 -- WHEN the op-0x14s go out, and to whom
 * ========================================================================== */
static void test_w2_no_0x14_until_the_last_0x13(void)
{
	printf("-- RULE W2: no op-0x14 until EVERY owed end-of-records is in\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-10-req", RMT_O_CSID);

	/*
	 * RA8's own interleave: the OLD MASTER's 0x13 arrives BEFORE the
	 * holder's records. A table that treated it as the end would close the
	 * adoption here and refuse the four locks that follow.
	 */
	ct_check(step("rm-13-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  the OLD MASTER's end-of-records is accepted");
	ct_check_eq_u32(g_f.ends, 0,
			"  *** the adoption is NOT closed: the holder has not "
			"reported (RA8's interleave) ***");
	ct_check_eq_u32(g_f.n_sent, 0, "  and no op-0x14 has gone out");

	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the holder's record still lands");
	ct_check_eq_u32(g_f.n_locks, 1, "  and is really adopted");

	ct_check(step("rm-13-notif", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the HOLDER's end-of-records closes it");
	ct_check_eq_u32(g_f.ends, 1, "  the adoption is closed exactly once");
	ct_check_eq_u32(g_f.n_sent, 2,
			"  *** two op-0x14s, after the last op-0x13 ***");
	ct_check_eq_u32(g_f.reports, 1,
			"  and the executive is told the completion was "
			"reported");
}

static void test_w1_one_0x14_per_0x13_and_no_others(void)
{
	printf("-- RULE W1: one op-0x14 per op-0x13, to its sender, to nobody "
	       "else\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.ends, 0, "  the DIRECTORY node has not reported");
	ct_check_eq_u32(g_f.n_sent, 0, "  nothing sent");

	ct_check(step("rm-13-notif", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the directory node reports, holding nothing in the tree");
	ct_check_eq_u32(g_f.n_sent, 2, "  two op-0x14s now go out");
	ct_check(g_f.sent[0].csid == RMT_O_CSID &&
		 g_f.sent[1].csid == RMT_H_CSID,
		 "  *** one to each system that sent an op-0x13 ***");
	ct_check_eq_u32(g_rm.ends_admitted, 0, "  both were predicted");

	printf("-- NEGATIVE CONTROL: a system that reported nothing gets "
	       "nothing\n");
	ct_check(g_f.sent[0].csid != RMT_B_CSID &&
		 g_f.sent[1].csid != RMT_B_CSID,
		 "  *** the member with NO ROLE is sent nothing ***");

	printf("-- and an op-0x13 that arrives for a tree ALREADY HELD is "
	       "refused, not re-announced\n");
	ct_check(step("rm-13-notif", RMT_B_CSID) == VMS_DLM_RM_REFUSED,
		 "  the late end-of-records is refused");
	ct_check_eq_u32(g_f.n_sent, 2, "  and no further op-0x14 goes out");
	ct_check_eq_u32(g_f.ends, 1, "  the adoption is still closed once");

	printf("-- an op-0x13 from a system owed nothing is counted, not "
	       "acted on\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-13-notif", 0x000100ffu) == VMS_DLM_RM_REFUSED,
		 "  a stranger's end-of-records is refused");
	ct_check_eq_u32(g_rm.ends_unexpected, 1, "  and counted");
	ct_check_eq_u32(g_f.ends, 0, "  the tree is untouched");
}

/* ==========================================================================
 * rd vms-083: THE OWED SET IS THE ROLES, COMPUTED FROM EXECUTIVE STATE
 *
 * The driven one-variable arm (tests/lab/captures/vms-083-dirnode-20261022,
 * two real OpenVMS VAX V7.3 nodes + one OVMX node, four runs, both VAX role
 * assignments) moved ONE variable -- which node the tree's root name assigns
 * as its directory node -- and measured:
 *
 *   directory is a THIRD member   ->  TWO op-0x13s (old master + directory)
 *   directory IS the old master   ->  ONE op-0x13, and the remaining member
 *                                     got no op-0x0f, no op-0x11 and sent
 *                                     nothing for 180 s.
 *
 * These cases are that measurement, plus the three ways the resolution can
 * fail honestly.
 * ========================================================================== */
static void test_vms083_owed_set_is_the_roles(void)
{
	printf("-- rd vms-083 ARM A: the directory is a THIRD member -- TWO "
	       "systems owe an end-of-records\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	g_f.dir_csid = RMT_H_CSID;
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check_eq_u32(g_rm.owed_dir_named, 1,
			"  this node named the tree's directory node");
	ct_check_eq_u32(g_f.dir_len, 4u,
			"  and asked with the identity's own COUNT");
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.n_sent, 0, "  the directory node still owes one");
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_f.n_sent, 2, "  two op-0x14s, after the last 0x13");

	printf("-- rd vms-083 ARM B: the directory IS the old master -- ONE "
	       "system owes one, and the tree REACHES HELD on it\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	g_f.dir_csid = RMT_O_CSID;      /* the one variable, moved */
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-13-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  the old master's end-of-records is accepted");
	ct_check_eq_u32(g_f.ends, 1,
			"  *** the adoption CLOSES on that one report ***");
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_HELD), 1,
			"  *** the tree reaches HELD: no double grant ***");
	ct_check_eq_u32(g_f.n_sent, 1, "  one op-0x14");
	ct_check_eq_u32(g_f.sent[0].csid, RMT_O_CSID,
			"  to the one system that sent one");
	ct_check_eq_u32(g_rm.held_unsent, 0, "  and none owed and unsent");

	printf("-- a HOLDER joins the owed set when its own record is "
	       "adopted\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	g_f.dir_csid = RMT_O_CSID;      /* arm B: the seed is {O} alone */
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-12-root-req", RMT_H_CSID) == VMS_DLM_RM_HANDLED,
		 "  the holder's record lands and is adopted");
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.ends, 0,
			"  *** the tree does NOT close: the holder owes one ***");
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_f.ends, 1, "  its 0x13 closes the tree");
	ct_check_eq_u32(g_f.n_sent, 2, "  and both reporters are told");

	printf("-- an UNPREDICTED member's op-0x13 is ADMITTED, and acked\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	g_f.dir_known = 0;              /* this node cannot name the directory */
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check_eq_u32(g_rm.owed_dir_unknown, 1,
			"  the unknown directory is COUNTED, not guessed at");
	ct_check(step("rm-13-notif", RMT_B_CSID) == VMS_DLM_RM_HANDLED,
		 "  a selected member the seed did not name reports anyway");
	ct_check_eq_u32(g_rm.ends_admitted, 1, "  counted as ADMITTED");
	ct_check_eq_u32(g_f.n_sent, 0, "  the old master still owes one");
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.n_sent, 2,
			"  *** the admitted sender gets its op-0x14 too ***");

	printf("-- NEGATIVE CONTROL: a directory resolution this node must "
	       "NOT wait for\n");
	rig_reset();
	g_f.dir_csid = 0u;              /* THIS node is the directory */
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check_eq_u32(g_rm.owed_dir_unknown, 1,
			"  this node does not owe ITSELF an end-of-records");
	ct_check_eq_u32(g_f.ends, 0, "  nothing closed yet");
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.ends, 1, "  the old master alone closes it");

	rig_reset();
	g_f.dir_csid = 0x000100ffu;     /* a CSID that is no member */
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check_eq_u32(g_rm.owed_dir_unknown, 1,
			"  a directory CSID the CLUB does not list is skipped");
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.ends, 1, "  and the tree still completes");
	ct_check_eq_u32(g_f.n_sent, 1, "  with one op-0x14");
}

static void test_departure_strikes_the_owed_member(void)
{
	printf("-- a member that LEAVES is struck from the owed set\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.n_sent, 0, "  the holder still owes one");
	vms_dlm_rm_member_departed(&g_rm, RMT_H_CSID);
	ct_check_eq_u32(g_rm.departures_struck, 1, "  it is struck");
	ct_check_eq_u32(g_f.ends, 1,
			"  the tree completes: nobody is left to wait for");
	ct_check_eq_u32(g_f.n_sent, 1,
			"  *** and only the old master -- the one system that "
			"reported -- is told ***");
	ct_check_eq_u32(g_f.sent[0].csid, RMT_O_CSID, "  to the old master");
}

/* ==========================================================================
 * 5. THE op-0x14 ITSELF: two executive reads behind it, and the RA7 knob
 * ========================================================================== */
static void test_the_held_frame(void)
{
	const struct vms_fixture *req = fixture("rm-0e-req");
	const uint8_t *b;
	uint32_t i;
	int residue_ok = 1;

	printf("-- the op-0x14 this node sends\n");
	if (req == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	if (g_f.n_sent < 1u) {
		ct_check(0, "  an op-0x14 went out");
		return;
	}
	b = g_f.sent[0].body;
	ct_check(b[8] == 0x02u && b[9] == 0x14u, "  body[8:10] = 02 14");
	ct_check(memcmp(b + 44, fxbody(req) + 44, 4) == 0,
		 "  body[44:48] is the tree's identity, as the engine holds it");
	ct_check(memcmp(b + 48, "RMT5", 4) == 0, "  body[48..] the root name");
	ct_check_eq_u32(le32(b + 128), g_f.hash,
			"  body[128:132] is the hash THIS EXECUTIVE holds");
	ct_check(memcmp(b + 40, "\0\0\0\0", 4) == 0, "  body[40:44] = 0");

	printf("-- NEGATIVE CONTROL: no hash held, no hash minted\n");
	rig_reset();
	g_f.hash_known = 0;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check(g_f.n_sent > 0u && le32(g_f.sent[0].body + 128) == 0u,
		 "  *** body[128:132] stays ZERO rather than a minted hash "
		 "(Rule 8) ***");

	printf("-- NEGATIVE CONTROL: a tree this executive does not master is "
	       "announced to NOBODY\n");
	rig_reset();
	g_f.mastered = 0;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_f.ends, 1, "  the adoption still completed");
	ct_check_eq_u32(g_f.n_sent, 0,
			"  *** and nothing was sent: the second executive read "
			"says this node does not master the name ***");
	ct_check_eq_u32(g_rm.held_not_mastered, 1, "  counted");
	ct_check_eq_u32(g_f.reports, 0, "  and reported as unreported");

	printf("-- rd vms-6ad1 (RA7): the residue byte comes from the "
	       "CONFIGURATION door\n");
	rig_reset();
	g_f.residue = 0x5au;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	if (g_f.n_sent < 1u) {
		ct_check(0, "  an op-0x14 went out");
		return;
	}
	b = g_f.sent[0].body;
	for (i = 16u; i < 40u; i++) {
		if (b[i] != 0x5au)
			residue_ok = 0;
	}
	ct_check(residue_ok,
		 "  the residue span body[16:40] carries the configured "
		 "pattern");
	ct_check(memcmp(b + 40, "\0\0\0\0", 4) == 0 &&
		 memcmp(b + 48, "RMT5", 4) == 0,
		 "  *** and it did NOT reach a named field ***");

	printf("-- a send that fails is counted, never implied\n");
	rig_reset();
	g_f.send_fails = 1;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_rm.held_unsent, 2, "  both op-0x14s counted unsent");
	ct_check_eq_u32(g_f.reports, 0,
			"  *** and the completion is NOT reported: a frame "
			"that did not go out is not an announcement ***");
}

/* ==========================================================================
 * 6. WHAT THE TABLE LEAVES ALONE
 * ========================================================================== */
static void test_not_mine_and_the_ack(void)
{
	const struct vms_fixture *n11 = fixture("rm-11-notif");
	const struct vms_fixture *ans = fixture("rm-0e-ans");
	const uint8_t *b;

	printf("-- what this table does NOT claim\n");
	if (n11 == NULL || ans == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	b = fxbody(n11);
	/* rd vms-33a: the op-0x11 IS this table's now -- it fires the holder's
	 * half. With the holder doors unbound (this rig) it is counted, and
	 * nothing is answered or sent, which is all this table may claim
	 * without them. */
	ct_check(vms_dlm_rm_step(&g_rm, RMT_O_CSID, b[8], b[9], b,
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_HANDLED &&
		 g_rm.notifications == 1u && g_rm.ho_unbound == 1u &&
		 g_f.n_staged == 0u && g_f.n_sent == 0u,
		 "  an op-0x11 is counted and, with no holder doors, nothing "
		 "is answered or sent");
	b = fxbody(ans);
	ct_check(vms_dlm_rm_step(&g_rm, RMT_O_CSID, b[8], b[9], b,
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_NOT_MINE,
		 "  a cat-0x82 answer is not a step");

	printf("-- the 82/15 answering an op-0x14 of OURS is recognised\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	b = fxbody(ans);   /* a real 82/15 naming RMT5 */
	ct_check(vms_dlm_rm_held_ack(&g_rm, RMT_O_CSID, b[8], b[9], b,
				     VMS_CM_BODY_LEN) == 1,
		 "  matched to the tree this node announced to that system");
	ct_check_eq_u32(g_rm.acks_received, 1, "  counted");
	ct_check(vms_dlm_rm_held_ack(&g_rm, 0x000100ffu, b[8], b[9], b,
				     VMS_CM_BODY_LEN) == 0,
		 "  *** and one from a system we announced nothing to is NOT "
		 "claimed ***");

	printf("-- NEGATIVE CONTROL: no doors, no action\n");
	{
		struct vms_dlm_rm bare;
		const struct vms_fixture *req = fixture("rm-0e-req");

		vms_dlm_rm_init(&bare, NULL);
		b = fxbody(req);
		ct_check(vms_dlm_rm_step(&bare, RMT_O_CSID, b[8], b[9], b,
					 VMS_CM_BODY_LEN) ==
			 VMS_DLM_RM_REFUSED,
			 "  an executive that cannot reach its engine refuses "
			 "every step");
	}
}

/* ==========================================================================
 * 7. MANY TREES AT ONCE, and the honest refusal past the table's width
 * ========================================================================== */
static void test_slots(void)
{
	const struct vms_fixture *req = fixture("rm-0e-req");
	uint8_t body[VMS_CM_BODY_LEN];
	uint32_t i;
	uint32_t opened = 0u;

	printf("-- a tree that moves away and comes back REUSES its own slot\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_HELD), 1,
			"  the tree is held here");
	ct_check(step("rm-0e-req", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  a fresh op-0x0e for the SAME tree is accepted");
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_OPEN), 1,
			"  one slot is open for it");
	ct_check_eq_u32(vms_dlm_rm_trees_in_state(&g_rm, VMS_DLM_RM_ST_HELD), 0,
			"  *** and NO stale HELD slot still names it: two slots "
			"naming one tree would make the op-0x0f role answer off "
			"whichever came first ***");
	{
		const struct vms_fixture *req = fixture("rm-0e-req");
		struct vms_dlm_res_ident id;

		memset(&id, 0, sizeof(id));
		id.group = (uint16_t)(fxbody(req)[44] |
				      ((uint16_t)fxbody(req)[45] << 8));
		id.mode = fxbody(req)[46];
		id.name_len = fxbody(req)[47];
		memcpy(id.name, fxbody(req) + 48, id.name_len);
		ct_check(vms_dlm_rm_is_new_master(&g_rm, &id) == 1,
			 "  and the role readback says NEW MASTER for it");
	}

	printf("-- concurrent hand-overs, and the refusal past the table's "
	       "width\n");
	if (req == NULL) {
		ct_check(0, "specimens load");
		return;
	}
	rig_reset();
	/* The same real frame, renamed: VMS_DLM_RM_TREES + 1 distinct trees. */
	for (i = 0u; i <= VMS_DLM_RM_TREES; i++) {
		memcpy(body, fxbody(req), VMS_CM_BODY_LEN);
		body[48] = (uint8_t)('A' + (i % 26u));
		body[49] = (uint8_t)('0' + (i / 26u));
		if (vms_dlm_rm_step(&g_rm, RMT_O_CSID, body[8], body[9], body,
				    VMS_CM_BODY_LEN) == VMS_DLM_RM_HANDLED)
			opened++;
	}
	ct_check_eq_u32(opened, VMS_DLM_RM_TREES,
			"  every slot takes a tree (the corpus' worst case is "
			"five at once)");
	ct_check_eq_u32(g_rm.no_slot, 1,
			"  *** and the one past the end is REFUSED and "
			"counted, not adopted halfway ***");
	ct_check_eq_u32(g_f.opens, VMS_DLM_RM_TREES,
			"  the engine was opened exactly that many times");
}

/* ==========================================================================
 * rd vms-26e: THE op-0x1a goes to the engine, cell for cell, and is never
 * answered
 * ========================================================================== */
static void test_op1a_reaches_the_engine(void)
{
	const struct vms_fixture *f = fixture("ra7a-02-1a-valblk");
	enum vms_dlm_rm_status st;

	printf("-- rd vms-26e: RA7 fire A's real op-0x1a reaches the engine\n");
	ct_check(f != NULL, "  the ra7a-02-1a-valblk specimen loads");
	if (f == NULL)
		return;
	rig_reset();
	st = step("ra7a-02-1a-valblk", RMT_O_CSID);
	ct_check(st == VMS_DLM_RM_HANDLED, "  the engine took it: HANDLED");
	ct_check_eq_u32(g_f.valblks, 1u, "  the engine was asked once");
	ct_check_eq_u32(g_f.last_valblk.from_csid, RMT_O_CSID,
			"  ...by the CM's own sender identity");
	ct_check_eq_u32(g_f.last_valblk.master_lkid, le32(fxbody(f) + 20),
			"  ...naming the sender's handle, body[20:24]");
	ct_check_eq_u32(g_f.last_valblk.holder_lkid, le32(fxbody(f) + 24),
			"  ...and ours, body[24:28]");
	ct_check(g_f.last_valblk.valblk_present &&
		 memcmp(g_f.last_valblk.valblk, fxbody(f) + 36, 16) == 0 &&
		 g_f.last_valblk.valblk_seq == le32(fxbody(f) + 52),
		 "  *** the block and sequence are the frame's body[36:56] ***");
	ct_check_eq_u32(g_f.n_staged, 0u,
			"  *** a NOTIFICATION: nothing is answered ***");
	ct_check_eq_u32(g_f.n_sent, 0u, "  and nothing is sent");
	ct_check_eq_u32(g_rm.valblks_taken, 1u, "  counted as taken");

	printf("-- NEGATIVE CONTROL: an op-0x1a the engine refuses\n");
	rig_reset();
	g_f.valblk_ok = 0;
	ct_check(step("ra7a-02-1a-valblk", RMT_O_CSID) == VMS_DLM_RM_REFUSED,
		 "  REFUSED");
	ct_check_eq_u32(g_rm.valblks_refused, 1u, "  and counted");
	ct_check_eq_u32(g_f.n_staged, 0u, "  still nothing answered");
	ct_check(vms_dlm_rm_step(&g_rm, 0u, 0x02u, 0x1au, fxbody(f),
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_REFUSED &&
		 g_f.valblks == 1u,
		 "  from a sender the CM cannot name: refused, engine not "
		 "asked");
}


/* ==========================================================================
 * 9. THE HOLDER'S HALF (rd vms-33a): THIS node holds locks in a tree a real
 *    VAX moves to a THIRD node, and reports them.
 *
 * THE EPISODE IS RA5's RMT5, replayed from THIS NODE'S SIDE: this node stands
 * where VAX3 (the holder, 1332) stood. The old master's op-0x0f and op-0x11
 * are VAX1's real frames; the new master's two 82/01 answers are VAX2's real
 * frames. What this node SENDS is built by the real table and codec from a
 * FAKE ENGINE whose proxies carry VAX3's own lock state -- the lock ids, PID,
 * modes, parent link and value block VAX3's SDA had -- so the records it emits
 * can be compared, cell by named cell, with the records VAX3 really sent
 * (rm-12-root-req, rm-12-sub-req, rm-13-notif). A table that copied an inbound
 * frame's bytes into a record would have nothing to copy them FROM here: no
 * inbound frame carries body[88:108] or the PID.
 * ========================================================================== */

#define RMT_N_CSID  0x00010002u   /* VAX2, as the real op-0x0f names it */

/* VAX3's own lock state for RMT5 (SDA and its own op-0x12 records). */
#define HO_ROOT_LKID  0x110000ceu
#define HO_S2_LKID    0x180000a9u
#define HO_EPID       0x20600216u
#define HO_TREE_HASH  0xc54eeaeau

struct ho_lock {
	uint32_t lkid, parent, master, master_lkid;
	uint8_t  grmode, rqmode, has_valblk, hash_known;
	char     name[8];
	uint8_t  name_len;
	uint32_t hash;
};

static struct {
	struct ho_lock lk[4];
	uint32_t n;
	int rehome_ok;
	uint32_t next_asked, rehomes, valblks;
	uint32_t last_new_master, last_old, last_new_lkid;
	struct vms_dlm_adopt_tree last_tree;
} g_ho;

static int fk_ho_next(void *ctx, const struct vms_dlm_adopt_tree *t,
		      uint32_t new_master, struct vms_dlm_handover_rec *out)
{
	uint32_t i, j;

	(void)ctx;
	g_ho.next_asked++;
	g_ho.last_tree = *t;
	g_ho.last_new_master = new_master;
	memset(out, 0, sizeof(*out));
	/* The engine's contract (vms_dlm_adopt.h): a lock still naming the old
	 * master whose parent, if any, already names the new one. */
	for (i = 0; i < g_ho.n; i++) {
		struct ho_lock *l = &g_ho.lk[i];
		uint32_t pm = 0u;

		if (l->master != t->old_master_csid)
			continue;
		if (l->parent != 0u) {
			for (j = 0; j < g_ho.n; j++)
				if (g_ho.lk[j].lkid == l->parent)
					break;
			if (j == g_ho.n || g_ho.lk[j].master != new_master)
				continue;
			pm = g_ho.lk[j].master_lkid;
		}
		memcpy(out->resnam, l->name, l->name_len);
		out->resnam_len = l->name_len;
		out->res_group = 0u;
		out->res_mode = 3u;
		out->holder_lkid = l->lkid;
		out->holder_epid = HO_EPID;
		out->parent_lkid = l->parent;
		out->parent_master_lkid = pm;
		out->granted_mode = l->grmode;
		out->requested_mode = l->rqmode;
		if (l->has_valblk) {
			memcpy(out->valblk, "RMLVB-0123456789", 16);
			out->valblk_seq = 1u;
			out->valblk_present = 1u;
		}
		out->hash_known = l->hash_known;
		out->dir_hash = l->hash;
		return 1;
	}
	return 0;
}

static int fk_ho_rehome(void *ctx, uint32_t lkid, uint32_t old_csid,
			uint32_t new_csid, uint32_t new_lkid)
{
	uint32_t i;

	(void)ctx;
	g_ho.rehomes++;
	g_ho.last_old = old_csid;
	g_ho.last_new_lkid = new_lkid;
	if (!g_ho.rehome_ok)
		return 0;
	for (i = 0; i < g_ho.n; i++) {
		if (g_ho.lk[i].lkid == lkid && g_ho.lk[i].master == old_csid) {
			g_ho.lk[i].master = new_csid;
			g_ho.lk[i].master_lkid = new_lkid;
			return 1;
		}
	}
	return 0;
}

static int fk_ho_valblk(void *ctx, const struct vms_dlm_adopt_valblk *v)
{
	(void)ctx;
	(void)v;
	g_ho.valblks++;
	return 1;
}

/* A holder rig: this node is H; the others are O (VAX1) and N (VAX2). */
static void ho_rig(int with_sub)
{
	rig_reset();
	g_f.members[0] = RMT_O_CSID;
	g_f.members[1] = RMT_N_CSID;
	g_f.n_members = 2u;
	g_f.hash = HO_TREE_HASH;
	g_ops.ho_next = fk_ho_next;
	g_ops.ho_rehome = fk_ho_rehome;
	g_ops.ho_valblk = fk_ho_valblk;
	vms_dlm_rm_init(&g_rm, &g_ops);

	memset(&g_ho, 0, sizeof(g_ho));
	g_ho.rehome_ok = 1;
	g_ho.lk[0].lkid = HO_ROOT_LKID;
	g_ho.lk[0].master = RMT_O_CSID;
	g_ho.lk[0].master_lkid = 0x0400082fu;   /* VAX1's handle for it */
	g_ho.lk[0].grmode = 1u;                 /* CR */
	g_ho.lk[0].rqmode = 1u;
	g_ho.lk[0].has_valblk = 1u;
	g_ho.lk[0].hash_known = 1u;
	g_ho.lk[0].hash = HO_TREE_HASH;
	memcpy(g_ho.lk[0].name, "RMT5", 4);
	g_ho.lk[0].name_len = 4u;
	g_ho.n = 1u;
	if (with_sub) {
		g_ho.lk[1].lkid = HO_S2_LKID;
		g_ho.lk[1].parent = HO_ROOT_LKID;
		g_ho.lk[1].master = RMT_O_CSID;
		g_ho.lk[1].master_lkid = 0x04000830u;
		g_ho.lk[1].grmode = 2u;             /* CW */
		g_ho.lk[1].rqmode = 2u;
		memcpy(g_ho.lk[1].name, "S2", 2);
		g_ho.lk[1].name_len = 2u;
		/* No hash for a sub name: nobody asked a directory about it. */
		g_ho.n = 2u;
	}
}

/* Do the NAMED cells of a record this node built equal the real holder's? */
static int ho_named_cells_match(const uint8_t *ours, const uint8_t *real,
				int with_hash)
{
	static const struct { unsigned off, len; } cells[] = {
		{ 8, 2 },     /* cat, op                               */
		{ 20, 4 },    /* the holder's PID                      */
		{ 24, 4 },    /* the holder's own lock id              */
		{ 30, 2 },    /* granted, requested mode               */
		{ 36, 8 },    /* the parent pair                       */
		{ 44, 4 },    /* group, access mode, name length       */
		{ 88, 20 },   /* the value block and its sequence      */
		{ 110, 1 },   /* const 0x01                            */
		{ 112, 16 },  /* const 00000000 ffffffff 00000000 ffffffff */
	};
	unsigned i;

	for (i = 0; i < sizeof(cells) / sizeof(cells[0]); i++)
		if (memcmp(ours + cells[i].off, real + cells[i].off,
			   cells[i].len) != 0) {
			printf("    cell body[%u:%u] differs\n", cells[i].off,
			       cells[i].off + cells[i].len);
			return 0;
		}
	if (memcmp(ours + 48, real + 48, real[47]) != 0)
		return 0;
	if (with_hash && memcmp(ours + 128, real + 128, 4) != 0)
		return 0;
	return 1;
}

static void ho_dump12(const uint8_t *b)
{
	char st[256];

	snprintf(st, sizeof(st),
		 "lkid=0x%x,epid=0x%x,grmode=%u,rqmode=%u,parent_lkid=0x%x,"
		 "parent_master=0x%x,group=0,acmode=3,namelen=%u,hash=0x%x,"
		 "valblkseq=0x%x",
		 le32(b + 24), le32(b + 20), b[30], b[31], le32(b + 36),
		 le32(b + 40), b[47], le32(b + 128), le32(b + 104));
	conf_dump("dlm_rm_12", VMS_OFF_SYSAP_BODY, st, b, VMS_CM_BODY_LEN);
}

static void ho_dump13(const uint8_t *b)
{
	char st[128];

	snprintf(st, sizeof(st), "group=0,acmode=3,namelen=%u,hash=0x%x",
		 b[47], le32(b + 128));
	conf_dump("dlm_rm_13", VMS_OFF_SYSAP_BODY, st, b, VMS_CM_BODY_LEN);
}

static void test_holder_reports_its_locks_root_first(void)
{
	const struct vms_fixture *root_req = fixture("rm-12-root-req");
	const struct vms_fixture *sub_req = fixture("rm-12-sub-req");
	const struct vms_fixture *root_ans = fixture("rm-12-root-ans");
	const struct vms_fixture *sub_ans = fixture("rm-12-sub-ans");
	const struct vms_fixture *end = fixture("rm-13-notif");
	const uint8_t *b;

	printf("-- rd vms-33a: RMT5 from the HOLDER's side -- the records, root "
	       "first, then the end-of-records\n");
	ct_check(root_req && sub_req && root_ans && sub_ans && end,
		 "  the RA5 holder specimens load");
	if (!(root_req && sub_req && root_ans && sub_ans && end))
		return;
	ho_rig(1);

	/* VAX1's op-0x0f to the holder: answered as a PLAIN member, and it
	 * ARMS the hand-over -- naming VAX2 (body[28:32] = 00010002). */
	ct_check(step("rm-0f-member-req", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  the real op-0x0f is HANDLED");
	ct_check_eq_u32(g_f.n_staged, 1u, "  ...and answered (82/15)");
	ct_check(memcmp(g_f.staged + 28, fxbody(fixture("rm-0f-member-req")) + 28,
			4) == 0,
		 "  ...in the plain-member role: body[28:32] KEPT");
	ct_check_eq_u32(g_rm.ho_armed, 1u, "  the hand-over is ARMED");
	ct_check_eq_u32(g_f.n_sent, 0u,
			"  *** and NOTHING is sent yet: the records follow the "
			"op-0x11 (7 of 7 attributable episodes) ***");

	/* VAX1's op-0x11: FIRE. The engine names the ROOT first. */
	ct_check(step("rm-11-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  the real op-0x11 is HANDLED");
	ct_check_eq_u32(g_f.n_staged, 1u, "  a notification: nothing answered");
	ct_check_eq_u32(g_f.n_sent, 1u, "  ONE record goes out");
	ct_check_eq_u32(g_f.sent[0].csid, RMT_N_CSID,
			"  *** to the NEW master the op-0x0f named, not to the "
			"old master that sent the op-0x11 ***");
	ct_check_eq_u32(g_ho.last_tree.old_master_csid, RMT_O_CSID,
			"  the engine was asked for locks still naming VAX1");
	b = g_f.sent[0].body;
	ho_dump12(b);
	ct_check(b[8] == 0x02 && b[9] == 0x12, "  it is a cat-0x02 op-0x12");
	ct_check(ho_named_cells_match(b, fxbody(root_req), 1),
		 "  *** EVERY NAMED CELL equals VAX3's real ROOT record: PID, "
		 "lock id, modes, no parent, identity, name, the value block "
		 "RMLVB-0123456789 at sequence 1, the tree's hash ***");
	ct_check(le32(b + 4) == 0u && le32(b + 12) == 0u,
		 "  body[4:8] and body[12:16] left for the connection manager");

	/* A second op-0x11 while the root's answer is out changes nothing. */
	ct_check(step("rm-11-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED &&
		 g_f.n_sent == 1u && g_rm.ho_repeats == 1u,
		 "  a repeated op-0x11 mid-hand-over sends nothing more");

	/* An 82/01 from the wrong system is NOT this record's answer. */
	ct_check(vms_dlm_rm_ho_answer(&g_rm, RMT_O_CSID, 0x82u, 0x01u,
				      fxbody(root_ans), VMS_CM_BODY_LEN) == 0,
		 "  the real answer, but from the OLD master: not consumed");

	/* VAX2's real 82/01 for the root: re-home, then the SUB record. */
	ct_check(vms_dlm_rm_ho_answer(&g_rm, RMT_N_CSID, 0x82u, 0x01u,
				      fxbody(root_ans), VMS_CM_BODY_LEN) == 1,
		 "  VAX2's real 82/01 for the root is consumed");
	ct_check_eq_u32(g_ho.last_new_lkid, le32(fxbody(root_ans) + 20),
			"  *** the proxy's master handle is REPLACED with the one "
			"VAX2 minted (0x020004cc) ***");
	ct_check_eq_u32(g_ho.last_old, RMT_O_CSID, "  ...from VAX1's");
	ct_check_eq_u32(g_f.n_sent, 2u, "  and the next record goes out");
	b = g_f.sent[1].body;
	ho_dump12(b);
	ct_check(ho_named_cells_match(b, fxbody(sub_req), 0),
		 "  *** EVERY NAMED CELL equals VAX3's real S2 record -- "
		 "including body[40:44], the parent's NEW-master handle, which "
		 "existed only once VAX2's answer came back ***");
	ct_check(le32(b + 128) == 0u,
		 "  and NO hash for a sub name this node never learned "
		 "(a deviation row, never a minted value)");

	/* VAX2's real 82/01 for S2: nothing left -> the op-0x13. */
	ct_check(vms_dlm_rm_ho_answer(&g_rm, RMT_N_CSID, 0x82u, 0x01u,
				      fxbody(sub_ans), VMS_CM_BODY_LEN) == 1,
		 "  VAX2's real 82/01 for S2 is consumed");
	ct_check_eq_u32(g_f.n_sent, 3u, "  and the end-of-records goes out");
	b = g_f.sent[2].body;
	ho_dump13(b);
	ct_check_eq_u32(g_f.sent[2].csid, RMT_N_CSID, "  to the new master");
	ct_check(b[8] == 0x02 && b[9] == 0x13, "  it is a cat-0x02 op-0x13");
	ct_check(memcmp(b + 44, fxbody(end) + 44, 4 + fxbody(end)[47]) == 0 &&
		 memcmp(b + 128, fxbody(end) + 128, 4) == 0 &&
		 b[28] == fxbody(end)[28] && le32(b + 4) == 0u &&
		 le32(b + 40) == le32(fxbody(end) + 40),
		 "  *** it equals VAX3's real op-0x13 on every named cell: the "
		 "identity, the name, the hash, the HOLDER role byte body[28]=0, "
		 "body[40:44]=0, and no transaction ***");
	ct_check(vms_dlm_rm_step_parse_body(b, VMS_CM_BODY_LEN,
					    &(struct vms_dlm_rm_step){0}) ==
		 VMS_CODEC_OK,
		 "  and the new master's own parser reads it as a step");
	ct_check_eq_u32(g_rm.ho_records_sent, 2u, "  counted: 2 records");
	ct_check_eq_u32(g_rm.ho_answers, 2u, "  ...2 answered and re-homed");
	ct_check_eq_u32(g_rm.ho_ends_sent, 1u, "  ...1 end-of-records");
	ct_check_eq_u32(vms_dlm_rm_ho_in_state(&g_rm, VMS_DLM_HO_ST_ENDED), 1u,
			"  *** the slot waits, ENDED, for the new master's op-0x14 "
			"(lab33a: freeing it here left VAX2's 0x14 unanswered) ***");
	ct_check(vms_dlm_rm_ho_answer(&g_rm, RMT_N_CSID, 0x82u, 0x01u,
				      fxbody(sub_ans), VMS_CM_BODY_LEN) == 0,
		 "  a late duplicate answer is not consumed (the reply path "
		 "counts it as unmatched)");

	/* rd vms-bc51: VAX2's real op-0x14 to the holder, and VAX3's real
	 * 82/15 to it (rm5-remaster.pcap frames 36/37). */
	{
		const struct vms_fixture *h14 = fixture("rm-14-holder-req");
		const struct vms_fixture *h15 = fixture("rm-14-holder-ans");
		uint32_t staged0 = g_f.n_staged;

		ct_check(vms_dlm_rm_step(&g_rm, RMT_O_CSID, 0x02u, 0x14u,
					 fxbody(h14), VMS_CM_BODY_LEN) !=
			 VMS_DLM_RM_HANDLED,
			 "  an op-0x14 for the tree from a system that is NOT "
			 "the new master is not this holder's to answer");
		ct_check_eq_u32(g_f.n_staged, staged0, "    (nothing staged)");
		ct_check(vms_dlm_rm_step(&g_rm, RMT_N_CSID, 0x02u, 0x14u,
					 fxbody(h14), VMS_CM_BODY_LEN) ==
			 VMS_DLM_RM_HANDLED,
			 "*** the NEW MASTER's real op-0x14 is answered ***");
		ct_check_eq_u32(g_f.n_staged, staged0 + 1u,
				"  one 82/15 staged");
		ct_check(memcmp(g_f.staged + 4, fxbody(h15) + 4,
				VMS_CM_BODY_LEN - 4u) == 0,
			 "  *** byte-for-byte VAX3's real 82/15 outside the "
			 "envelope body[0:4] ***");
		ct_check_eq_u32(g_rm.ho_held_acked, 1u, "  counted: 1 acked");
		ct_check_eq_u32(vms_dlm_rm_ho_in_state(&g_rm,
						       VMS_DLM_HO_ST_FREE),
				VMS_DLM_HO_TREES, "  and the slot is free again");
		ct_check(vms_dlm_rm_step(&g_rm, RMT_N_CSID, 0x02u, 0x14u,
					 fxbody(h14), VMS_CM_BODY_LEN) !=
			 VMS_DLM_RM_HANDLED,
			 "  a repeated op-0x14 is not answered twice");
	}
}

static void test_holder_with_nothing_still_ends(void)
{
	printf("-- rd vms-33a H2: a member that holds NOTHING in the tree still "
	       "sends its op-0x13 (15 of 391 real ones)\n");
	ho_rig(0);
	g_ho.n = 0u;
	(void)step("rm-0f-member-req", RMT_O_CSID);
	ct_check(step("rm-11-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  the op-0x11 is HANDLED");
	ct_check_eq_u32(g_f.n_sent, 1u, "  ONE frame goes out");
	ct_check(g_f.sent[0].body[9] == 0x13 &&
		 g_f.sent[0].csid == RMT_N_CSID,
		 "  *** and it is the op-0x13, to the new master ***");
	ho_dump13(g_f.sent[0].body);
	ct_check_eq_u32(g_rm.ho_records_sent, 0u, "  with no record before it");
}

static void test_holder_refusals(void)
{
	uint8_t body[VMS_CM_BODY_LEN];

	printf("-- rd vms-33a: what the holder half will NOT do\n");

	/* 1. An op-0x11 with no op-0x0f: the new master is unknown. */
	ho_rig(1);
	ct_check(step("rm-11-notif", RMT_O_CSID) == VMS_DLM_RM_HANDLED,
		 "  an unarmed op-0x11 is still HANDLED (it is never answered)");
	ct_check(g_f.n_sent == 0u && g_rm.ho_unarmed == 1u,
		 "  *** but NOTHING is sent: no 0x0f named the new master, and "
		 "this node does not guess one ***");

	/* 2. An op-0x0f naming a system that is not a member. */
	ho_rig(1);
	g_f.members[1] = 0x00010009u;
	(void)step("rm-0f-member-req", RMT_O_CSID);
	ct_check(g_rm.ho_armed == 0u && g_rm.ho_unaddressable == 1u,
		 "  an op-0x0f naming a non-member new master does not arm");
	ct_check(g_f.n_staged == 1u,
		 "  ...though it is still answered, as every member answers it");
	(void)step("rm-11-notif", RMT_O_CSID);
	ct_check_eq_u32(g_f.n_sent, 0u, "  ...and the op-0x11 sends nothing");

	/* 3. A refused answer STOPS the hand-over -- no op-0x13. */
	ho_rig(1);
	(void)step("rm-0f-member-req", RMT_O_CSID);
	(void)step("rm-11-notif", RMT_O_CSID);
	memcpy(body, fxbody(fixture("rm-12-root-ans")), sizeof(body));
	body[34] = 0x00u;   /* not a grant */
	ct_check(vms_dlm_rm_ho_answer(&g_rm, RMT_N_CSID, 0x82u, 0x01u, body,
				      sizeof(body)) == 1,
		 "  a non-grant answer to our record is consumed");
	ct_check(g_f.n_sent == 1u && g_rm.ho_answers_refused == 1u &&
		 g_ho.rehomes == 0u,
		 "  *** nothing re-homed and NO op-0x13: telling the new master "
		 "this node holds less than it does is the stale-handle crash "
		 "(rd vms-3ac) ***");

	/* 4. The engine will not re-home (the lock moved): stop too. */
	ho_rig(1);
	g_ho.rehome_ok = 0;
	(void)step("rm-0f-member-req", RMT_O_CSID);
	(void)step("rm-11-notif", RMT_O_CSID);
	(void)vms_dlm_rm_ho_answer(&g_rm, RMT_N_CSID, 0x82u, 0x01u,
				   fxbody(fixture("rm-12-root-ans")),
				   VMS_CM_BODY_LEN);
	ct_check(g_f.n_sent == 1u && g_rm.ho_answers_refused == 1u,
		 "  an answer the engine cannot apply stops the hand-over");

	/* 5. The new master LEAVES mid-hand-over: dropped, counted. */
	ho_rig(1);
	(void)step("rm-0f-member-req", RMT_O_CSID);
	(void)step("rm-11-notif", RMT_O_CSID);
	vms_dlm_rm_member_departed(&g_rm, RMT_N_CSID);
	ct_check(g_rm.ho_departed == 1u &&
		 vms_dlm_rm_ho_in_state(&g_rm, VMS_DLM_HO_ST_AWAIT) == 0u,
		 "  the new master left: the hand-over is dropped");

	/* 6. The send door refuses: stopped, never retried in a loop. */
	ho_rig(1);
	(void)step("rm-0f-member-req", RMT_O_CSID);
	g_f.send_fails = 1;
	(void)step("rm-11-notif", RMT_O_CSID);
	ct_check(g_rm.ho_records_unsent == 1u &&
		 vms_dlm_rm_ho_in_state(&g_rm, VMS_DLM_HO_ST_FREE) ==
		 VMS_DLM_HO_TREES,
		 "  a refused send stops the hand-over, counted");

	/* 7. The holder doors UNBOUND: the op-0x11 is only counted. */
	rig_reset();
	(void)step("rm-0f-member-req", RMT_O_CSID);
	(void)step("rm-11-notif", RMT_O_CSID);
	ct_check(g_f.n_sent == 0u && g_rm.ho_unbound == 1u &&
		 g_rm.notifications == 1u,
		 "  a table without the holder doors sends nothing and says so");
}

static void test_holder_op1a_needs_an_armed_sender(void)
{
	printf("-- rd vms-33a: an op-0x1a to a HOLDER reaches the engine only "
	       "from a system that armed a hand-over here\n");
	ho_rig(1);
	g_f.valblk_ok = 0;   /* this node is adopting nothing */
	ct_check(step("ra7a-02-1a-valblk", RMT_O_CSID) == VMS_DLM_RM_REFUSED &&
		 g_ho.valblks == 0u,
		 "  before any op-0x0f: REFUSED, the holder door not asked");
	(void)step("rm-0f-member-req", RMT_O_CSID);
	ct_check(step("ra7a-02-1a-valblk", RMT_O_CSID) == VMS_DLM_RM_HANDLED &&
		 g_ho.valblks == 1u && g_rm.ho_valblks_taken == 1u,
		 "  after VAX1's op-0x0f armed it: HANDLED through the engine");
	ct_check(step("ra7a-02-1a-valblk", RMT_N_CSID) == VMS_DLM_RM_REFUSED &&
		 g_ho.valblks == 1u,
		 "  from another member: REFUSED");
	ct_check_eq_u32(g_f.n_staged, 1u,
			"  and no op-0x1a is ever answered (only the 0x0f was)");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_dlm_remaster_arm: the new master's remaster table "
	       "(rd vms-ed5)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_open_drives_the_engine();
	test_open_refused_is_not_answered();
	test_role_comes_from_our_own_state();
	test_record_answer_follows_the_engine();
	test_sub_record_is_placed_by_the_engine();
	test_a_refused_record_takes_the_whole_tree_back();
	test_w2_no_0x14_until_the_last_0x13();
	test_w1_one_0x14_per_0x13_and_no_others();
	test_vms083_owed_set_is_the_roles();
	test_departure_strikes_the_owed_member();
	test_the_held_frame();
	test_not_mine_and_the_ack();
	test_slots();
	test_op1a_reaches_the_engine();
	test_holder_reports_its_locks_root_first();
	test_holder_with_nothing_still_ends();
	test_holder_refusals();
	test_holder_op1a_needs_an_armed_sender();

	return ct_summary("test_dlm_remaster_arm");
}
