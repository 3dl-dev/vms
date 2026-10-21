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
	uint32_t next_lkid;           /* MUTATED: the handle the fake mints */
	uint8_t  residue;

	/* what really happened */
	uint32_t opens, ends, aborts, reports;
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

static int fk_mastered(void *ctx, const char *resnam, uint16_t group,
		       uint8_t mode)
{
	(void)resnam;
	(void)group;
	(void)mode;
	return ((struct fake *)ctx)->mastered;
}

static int fk_hash(void *ctx, const char *resnam, uint16_t group, uint8_t mode,
		   uint32_t *out)
{
	struct fake *f = (struct fake *)ctx;

	(void)resnam;
	(void)group;
	(void)mode;
	if (!f->hash_known)
		return 0;
	*out = f->hash;
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

static void fk_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

static const struct vms_dlm_rm_ops g_ops_template = {
	&g_f, fk_members, fk_open, fk_lock, fk_end, fk_abort, fk_tree_of_lkid,
	fk_reported, fk_mastered, fk_hash, fk_stage, fk_send, fk_residue,
	fk_log
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
	g_f.next_lkid = 0x01000401u;
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
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_f.ends, 0,
			"  with three members and two reports the tree is "
			"still waiting");
	ct_check_eq_u32(g_f.n_sent, 0, "  nothing sent");

	ct_check(step("rm-13-notif", RMT_B_CSID) == VMS_DLM_RM_HANDLED,
		 "  the bystander reports too -- it holds nothing and still "
		 "sends one (RA8's CACHE$ episode)");
	ct_check_eq_u32(g_f.n_sent, 3, "  three op-0x14s now go out");
	ct_check(g_f.sent[0].csid == RMT_O_CSID &&
		 g_f.sent[1].csid == RMT_H_CSID &&
		 g_f.sent[2].csid == RMT_B_CSID,
		 "  *** one to each system that sent an op-0x13 ***");

	printf("-- NEGATIVE CONTROL: a system that reported nothing gets "
	       "nothing\n");
	rig_reset();
	g_f.members[2] = RMT_B_CSID;
	g_f.n_members = 3u;
	(void)step("rm-0e-req", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_O_CSID);
	(void)step("rm-13-notif", RMT_H_CSID);
	ct_check_eq_u32(g_f.n_sent, 0,
			"  *** the tree is not announced at all while a member "
			"still owes an end-of-records ***");
	ct_check_eq_u32(g_f.reports, 0,
			"  and the executive still counts the completion as "
			"unreported");

	printf("-- an op-0x13 from a system owed nothing is counted, not "
	       "acted on\n");
	rig_reset();
	(void)step("rm-0e-req", RMT_O_CSID);
	ct_check(step("rm-13-notif", 0x000100ffu) == VMS_DLM_RM_REFUSED,
		 "  a stranger's end-of-records is refused");
	ct_check_eq_u32(g_rm.ends_unexpected, 1, "  and counted");
	ct_check_eq_u32(g_f.ends, 0, "  the tree is untouched");
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
	ct_check(vms_dlm_rm_step(&g_rm, RMT_O_CSID, b[8], b[9], b,
				 VMS_CM_BODY_LEN) == VMS_DLM_RM_NOT_MINE,
		 "  an op-0x11 is not this table's: the arm counts it");
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
	test_departure_strikes_the_owed_member();
	test_the_held_frame();
	test_not_mine_and_the_ack();
	test_slots();

	return ct_summary("test_dlm_remaster_arm");
}
