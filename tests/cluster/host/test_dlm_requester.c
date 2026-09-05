/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_requester.c - FC-P4.6's R1: the DLM REQUESTER arm
 * (src/kernel-core/vms_dlm_scs_fsm.c), every transition of its table, against
 * a fake lock engine and a fake connection manager.
 *
 * ==========================================================================
 * THE ASSERTION THIS FILE EXISTS FOR
 * ==========================================================================
 * "Every outbound field is read from the proxy LKB" is the plan row's own
 * done-condition, and it is a claim a test can either prove or decorate. This
 * file proves it, by the only method that can:
 *
 *   THE FAKE ENGINE'S LKB IS MUTATED BETWEEN FRAMES, AND THE WIRE MUST FOLLOW.
 *
 * A requester that cached ANY field -- the mode, the resource name, the
 * master's handle, the hash -- would keep sending the OLD value and this file
 * would go red. The single most important case is `master_lkid` on the
 * completion, because a completion carrying a lock id that came off a frame
 * instead of out of the lock database is literally what bugchecked a real VAX
 * with INVLOCKID and took the cluster down (commit fc8540ae): the test sets the
 * fake LKB's master handle to a value DIFFERENT from the one the grant frame
 * carried, and then asserts the completion carries the LKB's.
 *
 * Every captured frame also records a SNAPSHOT of the fake LKB as it stood at
 * send time, and `check_frame_traces_to_lkb()` re-derives all six wire fields
 * from that snapshot. So the proof is not "the fields look plausible", it is
 * "there is a function from the lock database to the frame and the frame is its
 * image".
 *
 * WHAT IS FAKE HERE AND WHY IT IS HONEST. The lock ENGINE is fake (FC-P4.9's
 * host backend runs the real one, and the R2 leg
 * tests/cluster/sim/scenarios/dlm_requester.c drives the REAL vms_lock.c
 * through this same FSM). What is REAL here is the object under test -- the
 * shipping src/kernel-core/vms_dlm_scs_fsm.c -- and every byte it emits or
 * consumes, which goes through the shipping FC-P4.5 codec. Nothing in this file
 * reimplements a protocol decision.
 */
#include "cluster_test.h"
#include "vms_frame_compose.h"
#include "vms_dlm_scs_fsm.h"

#include <stdio.h>
#include <string.h>

#define CSID_SELF   0x00010001u
#define CSID_DIR    0x00010007u
#define CSID_DIR2   0x00010008u
#define CSID_MASTER 0x00010009u

/* ==========================================================================
 * The fake lock engine: ONE proxy LKB, and every door vms_dlm_scs_fsm.h asks
 * for. It is deliberately a plain struct -- the point of the test is that the
 * FSM reads it every time, so the struct being trivially mutable is the whole
 * mechanism.
 * ========================================================================== */
struct fake_lkb {
	int      exists;
	uint32_t lkid;
	uint32_t master_csid;   /* 0 == unmastered, which is what makes a post
				 * a DIRECTORY lookup (dlm_proxy_fill_post)   */
	uint32_t master_lkid;
	uint32_t lkmode;
	uint32_t flags;
	char     resnam[32];
	uint8_t  valblk[VMS_DLM_VALBLK_LEN];
	uint16_t dir_hash;
	uint8_t  hash_known;
};

struct sent_frame {
	vms_csid_t dst;
	uint8_t    body[DLM_REQ_BODY_LEN];
	uint32_t   len;
	struct fake_lkb lkb_at_send;   /* the snapshot the frame must match */
};

#define MAX_SENT 32

struct fake_engine {
	struct fake_lkb lkb;

	struct sent_frame sent[MAX_SENT];
	uint32_t n_sent;
	int      send_fails;

	/* the directory vector */
	vms_csid_t dir_answer;
	uint32_t   dir_generation;
	int        dir_refuse;
	uint32_t   dir_calls;
	uint16_t   dir_last_hash;

	/* what the FSM asked the engine to do */
	uint32_t refills;
	uint32_t record_master_calls;
	char     record_master_name[40];
	vms_csid_t record_master_csid;
	uint32_t assume_calls;
	char     assume_name[40];
	uint32_t grant_calls;
	struct vms_dlm_proxy_grant last_grant;
	int      grant_refuse;
	uint32_t blkast_calls;
	uint32_t blkast_lkid;
	int      blkast_refuse;
	uint32_t learn_calls;
	char     learn_name[40];
	uint16_t learn_hash;
	uint32_t fail_calls;
	uint32_t fail_lkid;
	enum dlm_req_fail_reason fail_why;

	uint32_t now_ms;
	uint32_t logs;
};

static struct fake_engine g;
static struct dlm_req_fsm g_fsm;

/* ---- the doors -------------------------------------------------------- */

static int fe_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	struct fake_engine *e = ctx;
	struct sent_frame *s;

	if (e->send_fails)
		return -1;
	if (e->n_sent >= MAX_SENT)
		return -1;
	s = &e->sent[e->n_sent++];
	memset(s, 0, sizeof(*s));
	s->dst = dst;
	s->len = len > DLM_REQ_BODY_LEN ? DLM_REQ_BODY_LEN : len;
	memcpy(s->body, body, s->len);
	s->lkb_at_send = e->lkb;
	return 0;
}

/*
 * THE ONE FUNCTION THAT MATTERS. A faithful stand-in for vms_lock.c's
 * dlm_proxy_fill_post(): every field out of the LKB/RSB, `to_directory`
 * derived exactly as the engine derives it (no master known), and the hash
 * taken from the resource block's LEARNED value, never computed.
 */
static int fe_refill(void *ctx, uint32_t req_lkid, uint32_t op,
		     vms_csid_t dst_csid, struct vms_dlm_proxy_post *out)
{
	struct fake_engine *e = ctx;

	e->refills++;
	if (!e->lkb.exists || e->lkb.lkid != req_lkid)
		return -1;

	memset(out, 0, sizeof(*out));
	out->op          = op;
	out->dst_csid    = dst_csid;
	out->req_csid    = CSID_SELF;
	out->req_lkid    = e->lkb.lkid;
	out->master_csid = e->lkb.master_csid;
	out->master_lkid = e->lkb.master_lkid;
	out->lkmode      = e->lkb.lkmode;
	out->flags       = e->lkb.flags;
	memcpy(out->resnam, e->lkb.resnam, sizeof(out->resnam));
	memcpy(out->valblk, e->lkb.valblk, VMS_DLM_VALBLK_LEN);
	out->dir_hash       = e->lkb.dir_hash;
	out->dir_hash_known = e->lkb.hash_known;
	out->to_directory   = (e->lkb.master_csid == 0u) ? 1u : 0u;
	return 0;
}

static int fe_dir_resolve(void *ctx, uint16_t hash16, vms_csid_t *out_csid)
{
	struct fake_engine *e = ctx;

	e->dir_calls++;
	e->dir_last_hash = hash16;
	if (e->dir_refuse)
		return -1;
	*out_csid = e->dir_answer;
	return 0;
}

static uint32_t fe_dir_generation(void *ctx)
{
	return ((struct fake_engine *)ctx)->dir_generation;
}

static int fe_record_master(void *ctx, const char *resnam, uint32_t req_lkid,
			    vms_csid_t master_csid)
{
	struct fake_engine *e = ctx;

	if (!e->lkb.exists || e->lkb.lkid != req_lkid)
		return -1;
	e->record_master_calls++;
	snprintf(e->record_master_name, sizeof(e->record_master_name), "%s",
		 resnam);
	e->record_master_csid = master_csid;
	/* what vms_lock_dlm_record_master really does */
	e->lkb.master_csid = master_csid;
	return 0;
}

static int fe_assume(void *ctx, const char *resnam, uint32_t req_lkid)
{
	struct fake_engine *e = ctx;

	if (!e->lkb.exists || e->lkb.lkid != req_lkid)
		return -1;
	e->assume_calls++;
	snprintf(e->assume_name, sizeof(e->assume_name), "%s", resnam);
	e->lkb.master_csid = CSID_SELF;   /* promoted: we master it now */
	return 0;
}

static int fe_grant(void *ctx, const struct vms_dlm_proxy_grant *gr)
{
	struct fake_engine *e = ctx;

	if (e->grant_refuse)
		return -1;
	if (!e->lkb.exists || e->lkb.lkid != gr->req_lkid)
		return -1;
	e->grant_calls++;
	e->last_grant = *gr;
	/* what vms_lock_dlm_xnode_grant_recv really does */
	e->lkb.master_lkid = gr->master_lkid;
	e->lkb.master_csid = gr->master_csid;
	if (gr->granted_mode > 0u)
		e->lkb.lkmode = gr->granted_mode;
	if (gr->valblk_present)
		memcpy(e->lkb.valblk, gr->valblk, VMS_DLM_VALBLK_LEN);
	return 0;
}

static int fe_blkast(void *ctx, uint32_t req_lkid)
{
	struct fake_engine *e = ctx;

	e->blkast_calls++;
	e->blkast_lkid = req_lkid;
	if (e->blkast_refuse)
		return -1;
	if (!e->lkb.exists || e->lkb.lkid != req_lkid)
		return -1;
	return 0;
}

static int fe_learn(void *ctx, const char *resnam, uint16_t hash16)
{
	struct fake_engine *e = ctx;

	e->learn_calls++;
	snprintf(e->learn_name, sizeof(e->learn_name), "%s", resnam);
	e->learn_hash = hash16;
	return 0;
}

static void fe_fail(void *ctx, uint32_t req_lkid, enum dlm_req_fail_reason why)
{
	struct fake_engine *e = ctx;

	e->fail_calls++;
	e->fail_lkid = req_lkid;
	e->fail_why = why;
}

static uint32_t fe_now(void *ctx)
{
	return ((struct fake_engine *)ctx)->now_ms;
}

static void fe_log(void *ctx, const char *msg)
{
	(void)msg;
	((struct fake_engine *)ctx)->logs++;
}

static struct dlm_req_ops g_ops;

static void fe_reset(const char *resnam, uint32_t lkmode, uint16_t hash,
		     int hash_known, uint32_t master_csid)
{
	memset(&g, 0, sizeof(g));
	g.lkb.exists = 1;
	g.lkb.lkid = 0x2001u;
	g.lkb.lkmode = lkmode;
	g.lkb.master_csid = master_csid;
	g.lkb.dir_hash = hash;
	g.lkb.hash_known = (uint8_t)(hash_known ? 1 : 0);
	snprintf(g.lkb.resnam, sizeof(g.lkb.resnam), "%s", resnam);
	g.dir_answer = CSID_DIR;
	g.dir_generation = 1u;
	g.now_ms = 1000u;

	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.send = fe_send;
	g_ops.refill_post = fe_refill;
	g_ops.dir_resolve = fe_dir_resolve;
	g_ops.dir_generation = fe_dir_generation;
	g_ops.record_master = fe_record_master;
	g_ops.assume_mastery = fe_assume;
	g_ops.grant_recv = fe_grant;
	g_ops.blkast_deliver = fe_blkast;
	g_ops.learn_dir_hash = fe_learn;
	g_ops.fail = fe_fail;
	g_ops.now_ms = fe_now;
	g_ops.log = fe_log;
	g_ops.ctx = &g;

	dlm_req_fsm_init(&g_fsm, &g_ops);
}

/* Build the post the ENGINE would hand us for the current LKB. */
static void post_from_lkb(struct vms_dlm_proxy_post *p, uint32_t op,
			  vms_csid_t dst)
{
	(void)fe_refill(&g, g.lkb.lkid, op, dst, p);
	g.refills--;   /* the harness's own read is not one of the FSM's */
}

/* ==========================================================================
 * Frame inspection -- through the SHIPPING codec, never by byte arithmetic
 * ========================================================================== */

/* A captured body, spliced back under a link so the codec can classify it.
 * The link bytes are a TEST FIXTURE (vms_frame_compose.h) and are never
 * asserted about; only the body span the FSM wrote is inspected. */
static uint32_t splice(const struct sent_frame *s, uint8_t *frame)
{
	struct vms_cm_link link;
	uint32_t written = 0;

	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	memcpy(frame + VMS_OFF_SYSAP_BODY, s->body, s->len);
	return VMS_CM_FRAME_LEN;
}

static int parse_request(const struct sent_frame *s, uint8_t *opcode_out,
			 struct vms_dlm_enq_request *out)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_frame_info fi;
	uint32_t len = splice(s, frame);

	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_dlm_enq_request_parse(frame, len, &fi, opcode_out, out) !=
	    VMS_CODEC_OK)
		return -1;
	return 0;
}

/* The completion/commit pair has no parser in the codec (it is PROVISIONAL and
 * carries no round trip), so this reads its three declared fields through the
 * codec's OWN published offsets rather than open-coded numbers. */
static int read_completion(const struct sent_frame *s, uint8_t *op_out,
			   uint32_t *master_lkid, uint32_t *req_lkid)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	vms_wire_view_t v;
	uint32_t len = splice(s, frame);

	vms_wire_view_init(&v, frame, len);
	*op_out = vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
	*master_lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_COMPLETE_MASTER_LKID);
	*req_lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_COMPLETE_REQ_LKID);
	return vms_wire_view_ok(&v) ? 0 : -1;
}

/* Is the 16-bit directory hash physically present at body[10:12]? A frame that
 * carries none must carry a ZERO there -- not because zero is a hash, but
 * because the builder left the span untouched over a zeroed frame. */
static uint16_t read_dir_hash(const struct sent_frame *s)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	vms_wire_view_t v;
	uint32_t len = splice(s, frame);

	vms_wire_view_init(&v, frame, len);
	return vms_wire_get_le16(&v, VMS_OFF_DLM_DIR_HASH);
}

/*
 * *** THE PROOF. *** Re-derive every field of an outbound ENQ/CONVERT from the
 * LKB snapshot taken at send time, and require the frame to be its image.
 */
static void check_frame_traces_to_lkb(const struct sent_frame *s,
				      const char *label)
{
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;
	char what[160];
	const struct fake_lkb *l = &s->lkb_at_send;
	size_t namelen = strlen(l->resnam);

	snprintf(what, sizeof(what), "%s: parses as a cat-02 request", label);
	if (parse_request(s, &opcode, &req) != 0) {
		ct_check(0, what);
		return;
	}
	ct_check(1, what);

	snprintf(what, sizeof(what), "%s: body[20:24] == the LKB's own lock id",
		 label);
	ct_check_eq_u32(req.req_pid_or_lkid, l->lkid, what);

	snprintf(what, sizeof(what), "%s: body[30] == the LKB's mode", label);
	ct_check_eq_u32(req.mode, l->lkmode, what);

	snprintf(what, sizeof(what),
		 "%s: body[24:28] == the LKB's master handle", label);
	ct_check_eq_u32(req.master_lkid, l->master_lkid, what);

	snprintf(what, sizeof(what), "%s: body[47..] == the RSB's name", label);
	ct_check(req.name_len == (uint8_t)namelen &&
		 memcmp(req.name, l->resnam, namelen) == 0, what);

	snprintf(what, sizeof(what),
		 "%s: body[10:12] == the RSB's LEARNED hash (or absent)", label);
	ct_check(read_dir_hash(s) == (l->hash_known ? l->dir_hash : 0u), what);

	snprintf(what, sizeof(what),
		 "%s: NO byte of the LKB's value block is on the wire", label);
	{
		int leaked = 0;
		uint32_t i;
		int nonzero = 0;

		for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
			if (l->valblk[i] != 0u)
				nonzero = 1;
		if (nonzero) {
			for (i = 0; i + VMS_DLM_VALBLK_LEN <= s->len; i++) {
				if (memcmp(s->body + i, l->valblk,
					   VMS_DLM_VALBLK_LEN) == 0)
					leaked = 1;
			}
		}
		ct_check(!leaked, what);
	}
}

/* ==========================================================================
 * Reply frames, built with the SHIPPING codec's own response builders
 * ========================================================================== */
static uint32_t make_grant(uint8_t *frame, uint32_t req_lkid,
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
	/* the response bit -- the builder writes cat 0x02; a reply is 0x82 */
	frame[VMS_OFF_DLM_CAT] = (uint8_t)(VMS_DLM_CAT_REQUEST | 0x80u);
	return VMS_CM_FRAME_LEN;
}

static uint32_t make_deny(uint8_t *frame, uint32_t pid_echo,
			  uint32_t master_lkid, const char *name)
{
	struct vms_cm_link link;
	uint32_t written = 0;
	uint8_t nm[VMS_DLM_NAME_MAX];
	uint8_t n = (uint8_t)strlen(name);

	memset(nm, 0, sizeof(nm));
	memcpy(nm, name, n);
	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	(void)vms_dlm_enq_response_build_deny(pid_echo, master_lkid, n, nm,
					      frame, VMS_CM_FRAME_LEN, &written);
	frame[VMS_OFF_DLM_CAT] = (uint8_t)(VMS_DLM_CAT_REQUEST | 0x80u);
	return VMS_CM_FRAME_LEN;
}

/* ==========================================================================
 * 1. The full path: lookup -> grant -> completion + commit
 * ========================================================================== */
static void test_full_path(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;
	const struct dlm_req *r;
	uint8_t op = 0;
	uint32_t mlk = 0, rlk = 0;

	printf("-- full path: directory lookup -> grant -> completion/commit\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_PW, 0x1234u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK, "the post is taken");
	ct_check_eq_u32(g.n_sent, 1u, "exactly ONE frame went out");
	ct_check_eq_u32(g.sent[0].dst, CSID_DIR,
			"it went to the DIRECTORY node the engine named");
	ct_check_eq_u32(g_fsm.lookups_sent, 1u, "counted as a LOOKUP");
	check_frame_traces_to_lkb(&g.sent[0], "lookup");

	r = dlm_req_fsm_find(&g_fsm, g.lkb.lkid);
	ct_check(r != NULL && r->state == (uint8_t)DLM_REQ_ST_LOOKUP,
		 "the request block is in ST_LOOKUP");

	/* The master answers with a grant, from a node that IS the directory
	 * (Davis p. 6-31 outcome 1 -- the common case). */
	len = make_grant(frame, g.lkb.lkid, 0x00ABCDEFu, VMS_LCK_PW);
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_DIR, 0u, frame, len) ==
		 DLM_REQ_OK, "the grant is accepted");
	ct_check_eq_u32(g.grant_calls, 1u, "the ENGINE was told about it");
	ct_check_eq_u32(g.last_grant.master_lkid, 0x00ABCDEFu,
			"  the master's handle came off body[24:28]");
	ct_check_eq_u32(g.last_grant.master_csid, CSID_DIR,
			"  the master's CSID is the frame's OWN SCA source");
	ct_check_eq_u32(g.last_grant.req_lkid, g.lkb.lkid,
			"  the request handle is OURS, not the frame's");
	ct_check_eq_u32(g.last_grant.valblk_present, 0u,
			"  valblk_present is 0: no grounded LVB field, so the "
			"engine keeps the proxy's own block");

	ct_check_eq_u32(g_fsm.completions_sent, 1u,
			"a completion/commit PAIR went out");
	ct_check_eq_u32(g.n_sent, 3u, "three frames total (lookup + 2)");

	ct_check(read_completion(&g.sent[1], &op, &mlk, &rlk) == 0 &&
		 op == VMS_DLM_WIREOP_COMPLETE_PROVISIONAL,
		 "frame 2 is the op-0x04 completion");
	ct_check_eq_u32(mlk, 0x00ABCDEFu,
			"  its master handle is the LKB's (which the grant set)");
	ct_check_eq_u32(rlk, g.lkb.lkid, "  its req handle is the LKB's");

	ct_check(read_completion(&g.sent[2], &op, &mlk, &rlk) == 0 &&
		 op == VMS_DLM_WIREOP_COMMIT_PROVISIONAL,
		 "frame 3 is the op-0x03 commit");

	r = dlm_req_fsm_find(&g_fsm, g.lkb.lkid);
	ct_check(r != NULL && r->state == (uint8_t)DLM_REQ_ST_GRANTED,
		 "the request block is now ST_GRANTED");
}

/* ==========================================================================
 * 2. *** THE ANTI-LARP ASSERTION ***
 *
 * The completion is built from a FRESH read of the lock database, not from the
 * grant frame. Proved by making the two DIFFER: the fake engine's grant handler
 * records the master's handle and then the test changes it, so a completion
 * that carried the FRAME's value would be visibly wrong.
 * ========================================================================== */
static void test_completion_reads_the_lkb_not_the_frame(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len, refills_before;
	uint8_t op = 0;
	uint32_t mlk = 0, rlk = 0;

	printf("-- the completion's master handle comes from the LKB "
	       "(fc8540ae)\n");
	fe_reset("LNM$CWLOGICALS", VMS_LCK_EX, 0x4321u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g_fsm.requests_sent, 1u,
			"a known master gets a REQUEST, not a lookup");

	/*
	 * The grant frame says 0x11111111. The engine (our fake) records it,
	 * and then a REMASTER changes the executive's own record to
	 * 0x22222222 before the completion is built. Only a build that RE-READS
	 * can carry 0x22222222.
	 */
	len = make_grant(frame, g.lkb.lkid, 0x11111111u, VMS_LCK_EX);
	g.send_fails = 1;   /* make the completion fail so we control the retry */
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	g.send_fails = 0;
	ct_check_eq_u32(g.lkb.master_lkid, 0x11111111u,
			"the engine recorded the grant's handle");

	g.lkb.master_lkid = 0x22222222u;   /* the executive's truth moves */
	refills_before = g.refills;
	g.now_ms += DLM_REQ_RETRY_MS + 1u;
	(void)dlm_req_fsm_tick(&g_fsm);

	ct_check(g.refills > refills_before,
		 "the retry RE-READ the lock database");
	ct_check(g.n_sent >= 3u, "the completion pair went out on the retry");
	ct_check(read_completion(&g.sent[g.n_sent - 2u], &op, &mlk, &rlk) == 0,
		 "the completion parses");
	ct_check_eq_u32(mlk, 0x22222222u,
			"*** it carries the LKB's CURRENT handle, NOT the "
			"grant frame's ***");
}

/* ==========================================================================
 * 3. A novel root name: no wire-learned hash -> refuse, send NOTHING
 * ========================================================================== */
static void test_hash_unknown_refuses(void)
{
	struct vms_dlm_proxy_post p;

	printf("-- a root name with no WIRE-LEARNED hash is refused, "
	       "silently\n");
	fe_reset("OVMX$PRIVATE_VOL", VMS_LCK_EX, 0u, 0 /* not known */, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_E_NOHASH,
		 "the post is REFUSED");
	ct_check_eq_u32(g.n_sent, 0u, "*** NOTHING went on the wire ***");
	ct_check_eq_u32(g_fsm.hash_unknown_refused, 1u, "counted");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"no phantom request block was left behind");
	ct_check_eq_u32(g.dir_calls, 0u,
			"the directory resolver was never even called");
}

/* ==========================================================================
 * 4. REDIRECT (outcome 2): retry at the master the DIRECTORY named
 * ========================================================================== */
static void test_redirect(void)
{
	struct vms_dlm_proxy_post p;
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;

	printf("-- outcome 2: the directory names the master, we retry there\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_CR, 0x0055u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g.n_sent, 1u, "the lookup went to the directory");

	ct_check(dlm_req_fsm_redirect(&g_fsm, g.lkb.lkid, CSID_MASTER) ==
		 DLM_REQ_OK, "the redirect is taken");
	ct_check_eq_u32(g.record_master_calls, 1u,
			"the answer went into the LOCK DATABASE first");
	ct_check(strcmp(g.record_master_name, "F11B$aSYSDSK1") == 0,
		 "  and the NAME it was recorded against came from the LKB, "
		 "not the frame");
	ct_check_eq_u32(g.record_master_csid, CSID_MASTER,
			"  with the CSID the directory named");

	ct_check_eq_u32(g.n_sent, 2u, "one retry frame went out");
	ct_check_eq_u32(g.sent[1].dst, CSID_MASTER,
			"*** addressed to the MASTER, from the re-read ***");
	ct_check_eq_u32(g_fsm.redirects_followed, 1u, "counted");
	check_frame_traces_to_lkb(&g.sent[1], "redirect retry");

	ct_check(parse_request(&g.sent[1], &opcode, &req) == 0 &&
		 opcode == VMS_DLM_WIREOP_ENQ,
		 "the retry is still an op-0x01 ENQ");
	ct_check(dlm_req_fsm_find(&g_fsm, g.lkb.lkid)->state ==
		 (uint8_t)DLM_REQ_ST_ENQ,
		 "the block moved to ST_ENQ (outstanding at the master)");
}

/* ==========================================================================
 * 5. ASSUME (outcome 3): "you master it" -- promote, send nothing
 * ========================================================================== */
static void test_assume_mastery(void)
{
	struct vms_dlm_proxy_post p;

	printf("-- outcome 3: no master exists, so the engine promotes ours\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0077u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	ct_check(dlm_req_fsm_assume_mastery(&g_fsm, g.lkb.lkid) == DLM_REQ_OK,
		 "the answer is taken");
	ct_check_eq_u32(g.assume_calls, 1u, "the ENGINE promoted the proxy");
	ct_check(strcmp(g.assume_name, "F11B$aSYSDSK1") == 0,
		 "  against the name READ FROM THE LKB");
	ct_check_eq_u32(g.n_sent, 1u,
		"*** nothing further was sent: there is nobody to send to ***");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"the wire record is gone -- it is a LOCAL lock now");
	ct_check_eq_u32(g_fsm.masteries_assumed, 1u, "counted");
}

/* ==========================================================================
 * 6. DECLINE: re-resolve through the CURRENT vector, and STOP when it does
 *    not move. This is the grant storm's cure.
 * ========================================================================== */
static void test_decline_reresolve_then_stop(void)
{
	struct vms_dlm_proxy_post p;

	printf("-- a decline re-resolves once, then refuses to storm\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0099u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g.n_sent, 1u, "the lookup went to VECTOR[hash] = DIR");

	/* Phase 1 of a transition rebuilt the vector; it now names another. */
	g.dir_answer = CSID_DIR2;
	g.dir_generation = 2u;
	ct_check(dlm_req_fsm_decline(&g_fsm, g.lkb.lkid) == DLM_REQ_OK,
		 "the decline is taken");
	ct_check_eq_u32(g.dir_last_hash, 0x0099u,
			"the resolver was asked with the LEARNED hash");
	ct_check_eq_u32(g.n_sent, 2u, "one retry went out");
	ct_check_eq_u32(g.sent[1].dst, CSID_DIR2, "  to the NEW directory node");
	ct_check_eq_u32(g_fsm.declines_reresolved, 1u, "counted");

	/* Now the vector keeps naming the same node. One more frame would be
	 * the first of the storm. */
	ct_check(dlm_req_fsm_decline(&g_fsm, g.lkb.lkid) != DLM_REQ_OK,
		 "the second decline does NOT retry");
	ct_check_eq_u32(g.n_sent, 2u, "*** no further frame went out ***");
	ct_check_eq_u32(g.fail_calls, 1u, "the waiter was told, honestly");
	ct_check(g.fail_why == DLM_REQ_FAIL_UNROUTABLE,
		 "  with UNROUTABLE, not a fabricated success");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"the request block is gone");
}

/* ==========================================================================
 * 7. DENY at the MASTER is an ANSWER (SS$_NOTQUEUED), not a routing problem
 * ========================================================================== */
static void test_deny_at_master(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;

	printf("-- a deny from the MASTER is delivered, not retried\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00aau, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	len = make_deny(frame, g.lkb.lkid, 0x1234u, "F11B$aSYSDSK1");
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len) ==
		 DLM_REQ_OK, "the deny is accepted");
	ct_check_eq_u32(g_fsm.denies_rx, 1u, "counted");
	ct_check_eq_u32(g.n_sent, 1u, "no retry frame went out");
	ct_check_eq_u32(g.fail_calls, 1u, "the waiter was told");
	ct_check(g.fail_why == DLM_REQ_FAIL_NOTQUEUED,
		 "  with NOTQUEUED -- the master's own answer");
}

/* ==========================================================================
 * 8. CONVERT on a granted lock: op 0x07, carrying the LKB's NEW mode
 * ========================================================================== */
static void test_convert(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;
	uint32_t len, n;

	printf("-- convert: op 0x07 with the mode READ OFF the LKB\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_CR, 0x00bbu, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	len = make_grant(frame, g.lkb.lkid, 0x0777u, VMS_LCK_CR);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	n = g.n_sent;

	/* The $ENQ convert raised the requested mode in the LKB first. */
	g.lkb.lkmode = VMS_LCK_EX;
	post_from_lkb(&p, VMS_DLM_POST_CONVERT, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK,
		 "the convert post is taken");
	ct_check_eq_u32(g.n_sent, n + 1u, "one convert frame went out");
	ct_check(parse_request(&g.sent[n], &opcode, &req) == 0 &&
		 opcode == VMS_DLM_WIREOP_CONVERT, "it is op 0x07");
	ct_check_eq_u32(req.mode, VMS_LCK_EX, "carrying the LKB's NEW mode");
	ct_check_eq_u32(req.master_lkid, 0x0777u,
			"and the master handle the LKB holds");
	check_frame_traces_to_lkb(&g.sent[n], "convert");
	ct_check(dlm_req_fsm_find(&g_fsm, g.lkb.lkid)->state ==
		 (uint8_t)DLM_REQ_ST_ENQ, "back to ST_ENQ until it is answered");
}

/* ==========================================================================
 * 9. The RELEASE: no grounded opcode, so nothing is sent and it is COUNTED
 * ========================================================================== */
static void test_release_is_honestly_unsent(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len, n;

	printf("-- a cross-node release has NO grounded opcode (E6's open "
	       "half)\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00ccu, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	len = make_grant(frame, g.lkb.lkid, 0x0888u, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	n = g.n_sent;

	post_from_lkb(&p, VMS_DLM_POST_DEQ, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_E_NOWIREOP,
		 "the release is REFUSED, not guessed at opcode 0x03");
	ct_check_eq_u32(g.n_sent, n, "*** nothing went on the wire ***");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 1u,
			"counted -- a measured gap, not a silent one");
	ct_check_eq_u32(g.logs > 0u ? 1u : 0u, 1u, "and said out loud");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"the wire record is dropped: we no longer hold it");
}

/* ==========================================================================
 * 10. BLKAST -> a REAL local AST, through the engine
 * ========================================================================== */
static void test_blkast(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;

	printf("-- an inbound BLKAST fires the holder's REAL local AST\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00ddu, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	/* Before the grant, this lock is not a holder: an empty table cell. */
	ct_check(dlm_req_fsm_blkast(&g_fsm, g.lkb.lkid) == DLM_REQ_E_STATE,
		 "a BLKAST for a request that is not yet granted is IGNORED");
	ct_check_eq_u32(g_fsm.ignored_events, 1u, "  and COUNTED, not guessed");
	ct_check_eq_u32(g.blkast_calls, 0u, "  the engine was not called");

	len = make_grant(frame, g.lkb.lkid, 0x0999u, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);

	ct_check(dlm_req_fsm_blkast(&g_fsm, g.lkb.lkid) == DLM_REQ_OK,
		 "a BLKAST on the granted lock is delivered");
	ct_check_eq_u32(g.blkast_calls, 1u, "the engine fired it");
	ct_check_eq_u32(g.blkast_lkid, g.lkb.lkid,
			"  named by OUR OWN handle");
	ct_check_eq_u32(g_fsm.blkasts_delivered, 1u, "counted as delivered");

	/* The holder registered no blocking AST: the engine declines. */
	g.blkast_refuse = 1;
	ct_check(dlm_req_fsm_blkast(&g_fsm, g.lkb.lkid) != DLM_REQ_OK,
		 "a BLKAST the engine cannot deliver is not faked");
	ct_check_eq_u32(g_fsm.blkasts_undeliverable, 1u, "counted honestly");

	/* A handle we hold no request for. */
	g.blkast_refuse = 0;
	ct_check(dlm_req_fsm_blkast(&g_fsm, 0x9999u) == DLM_REQ_E_NOLOCK,
		 "a BLKAST naming no request of ours is refused");
}

/* ==========================================================================
 * 11. The value block: carried IN the post, never ON the wire
 * ========================================================================== */
static void test_lvb(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t i, len;

	printf("-- the LVB write crossing is an HONEST OMISSION, and counted\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00eeu, 1, CSID_MASTER);
	for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
		g.lkb.valblk[i] = (uint8_t)(0xA0u + i);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	ct_check(p.valblk[0] == 0xA0u,
		 "the post carries the LKB's real value block");
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g_fsm.lvb_write_no_wire_field, 1u,
			"the unsent write crossing is COUNTED");
	check_frame_traces_to_lkb(&g.sent[0], "enq with a value block");

	/* And an inbound grant must not zero the proxy's block. */
	len = make_grant(frame, g.lkb.lkid, 0x0aaau, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g.last_grant.valblk_present, 0u,
			"the grant is handed to the engine with "
			"valblk_present = 0");
	ct_check(g.lkb.valblk[0] == 0xA0u,
		 "*** so the proxy's own value block SURVIVED the grant ***");
}

/* ==========================================================================
 * 12. Retransmit idempotency, keyed on (req_csid, req_lkid)
 * ========================================================================== */
static void test_retransmit_idempotency(void)
{
	struct vms_dlm_proxy_post p;
	struct vms_dlm_enq_request a, b;
	uint8_t oa = 0, ob = 0;
	uint32_t refills_before;

	printf("-- a re-post and a retransmit reuse the SAME request\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00ffu, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	(void)dlm_req_fsm_post(&g_fsm, &p);   /* the engine posted again */

	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"*** ONE request block, not two ***");
	ct_check_eq_u32(g.n_sent, 2u, "two frames went out");
	ct_check(parse_request(&g.sent[0], &oa, &a) == 0 &&
		 parse_request(&g.sent[1], &ob, &b) == 0,
		 "both parse");
	ct_check(a.req_pid_or_lkid == b.req_pid_or_lkid &&
		 a.req_pid_or_lkid == g.lkb.lkid,
		 "both carry the SAME (req_csid, req_lkid) key");
	ct_check_eq_u32(g_fsm.retransmits, 1u, "the second is counted a retransmit");

	/* The beat's retransmit RE-READS. */
	refills_before = g.refills;
	g.now_ms += DLM_REQ_RETRY_MS + 1u;
	ct_check_eq_u32(dlm_req_fsm_tick(&g_fsm), 1u, "the beat retransmits");
	ct_check(g.refills > refills_before,
		 "*** and it did so from a FRESH executive read ***");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"still ONE request block");

	/* Ladder exhaustion is an honest terminal status, not a hang. */
	while (dlm_req_fsm_outstanding(&g_fsm) > 0u && g.n_sent < MAX_SENT) {
		g.now_ms += DLM_REQ_RETRY_MS + 1u;
		(void)dlm_req_fsm_tick(&g_fsm);
	}
	ct_check_eq_u32(g.fail_calls, 1u, "the ladder ended in a real failure");
	ct_check(g.fail_why == DLM_REQ_FAIL_TIMEOUT, "  with TIMEOUT");
	ct_check_eq_u32(g_fsm.timeouts_failed, 1u, "counted");
}

/* ==========================================================================
 * 13. A duplicate GRANT: re-applied, re-answered, never re-minted
 * ========================================================================== */
static void test_duplicate_grant(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;

	printf("-- a retransmitted grant is answered again, not doubled\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0111u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	len = make_grant(frame, g.lkb.lkid, 0x0bbbu, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g_fsm.completions_sent, 1u, "the first pair went out");

	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g_fsm.grants_duplicate, 1u, "the duplicate is counted");
	ct_check_eq_u32(g_fsm.completions_resent, 1u,
			"and answered again -- ONE reply per received frame");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"*** still ONE request block ***");
}

/* ==========================================================================
 * 14. A member left
 * ========================================================================== */
static void test_peer_gone(void)
{
	struct vms_dlm_proxy_post p;

	printf("-- a request outstanding at a departed member fails honestly\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0122u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	ct_check_eq_u32(dlm_req_fsm_peer_gone(&g_fsm, CSID_DIR), 0u,
			"an unrelated departure touches nothing");
	ct_check_eq_u32(dlm_req_fsm_peer_gone(&g_fsm, CSID_MASTER), 1u,
			"the master's departure ends the request");
	ct_check_eq_u32(g.fail_calls, 1u, "the waiter was told");
	ct_check(g.fail_why == DLM_REQ_FAIL_PATHLOST, "  with PATHLOST");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u, "block released");
}

/* ==========================================================================
 * 15. E49: the hash is LEARNED from a frame that carries both halves
 * ========================================================================== */
static void test_observe_learns_the_hash(void)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_cm_link link;
	struct vms_dlm_enq_request req;
	uint32_t written = 0, len;

	printf("-- E49: body[10:12] + the root name, learned off the wire\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0u, 0, 0u);

	/* An inbound REQUEST from another system, carrying ITS hash. */
	memset(&req, 0, sizeof(req));
	req.mode = VMS_LCK_PR;
	req.req_pid_or_lkid = 0x5150u;
	req.dir_hash = 0xBEEFu;
	req.dir_hash_valid = 1u;
	req.name_len = (uint8_t)strlen("F11B$aSYSDSK1");
	memcpy(req.name, "F11B$aSYSDSK1", req.name_len);

	memset(&link, 0, sizeof(link));
	memset(frame, 0, sizeof(frame));
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	(void)vms_dlm_enq_request_build(&req, VMS_DLM_WIREOP_ENQ, frame,
					VMS_CM_FRAME_LEN, &written);
	len = VMS_CM_FRAME_LEN;

	ct_check_eq_u32(dlm_req_fsm_observe(&g_fsm, frame, len), 1u,
			"one hash learned from the frame");
	ct_check_eq_u32(g.learn_calls, 1u, "the ENGINE was told");
	ct_check_eq_u32(g.learn_hash, 0xBEEFu,
			"  the value the SENDER put on the wire");
	ct_check(strcmp(g.learn_name, "F11B$aSYSDSK1") == 0,
		 "  against the name from the SAME frame");
	ct_check_eq_u32(g_fsm.hashes_learned, 1u, "counted");

	/* A GRANT echoes no name (spec 4(f).1), so it teaches nothing -- a
	 * hash learned against the wrong name is worse than no hash. */
	len = make_grant(frame, 0x2001u, 0x0ccc, VMS_LCK_EX);
	ct_check_eq_u32(dlm_req_fsm_observe(&g_fsm, frame, len), 0u,
			"a GRANT (which echoes no name) teaches nothing");
	ct_check_eq_u32(g.learn_calls, 1u, "the engine was not called again");
}

/* ==========================================================================
 * 16. Table hygiene: unmatched replies, missing ops, empty cells
 * ========================================================================== */
static void test_refusals(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct dlm_req_fsm bare;
	struct dlm_req_ops half;
	uint32_t len;

	printf("-- the refusals: unmatched, unparsed, and an FSM with no ops\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0133u, 1, CSID_MASTER);

	len = make_grant(frame, 0x7777u, 0x0dddu, VMS_LCK_EX);
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len) ==
		 DLM_REQ_E_NOLOCK,
		 "a reply naming no request of ours is dropped");
	ct_check_eq_u32(g_fsm.replies_unmatched, 1u, "counted");
	ct_check_eq_u32(g.grant_calls, 0u,
			"and NOT applied to some other request");

	memset(frame, 0, sizeof(frame));
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame,
				   VMS_CM_FRAME_LEN) != DLM_REQ_OK,
		 "an unparsable frame is refused");
	ct_check(g_fsm.replies_unparsed > 0u, "counted");

	/* An arm whose doors are not all wired refuses to act. */
	memset(&half, 0, sizeof(half));
	half.send = fe_send;
	half.ctx = &g;
	dlm_req_fsm_init(&bare, &half);
	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&bare, &p) == DLM_REQ_E_INVAL,
		 "an FSM with an unwired engine door does nothing");

	/* A post with no lock id is the engine's own refusal, mirrored. */
	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	p.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_E_INVAL,
		 "a post with no lock id is refused (the fc8540ae sentinel)");
}

/* ==========================================================================
 * 17. The transmission is ABANDONED when the lock went away under it
 * ========================================================================== */
static void test_lock_gone(void)
{
	struct vms_dlm_proxy_post p;
	uint32_t n;

	printf("-- a retransmit for a lock that no longer exists is dropped\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0144u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	n = g.n_sent;

	g.lkb.exists = 0;   /* $DEQ or rundown took it */
	g.now_ms += DLM_REQ_RETRY_MS + 1u;
	(void)dlm_req_fsm_tick(&g_fsm);

	ct_check_eq_u32(g.n_sent, n,
		"*** no frame about a lock that no longer exists ***");
	ct_check_eq_u32(g_fsm.lock_gone, 1u, "counted");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"and the request block is released");
}

/* ==========================================================================
 * 18. Reply correlation prefers the CM's transaction envelope
 * ========================================================================== */
static void test_correlation(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;

	printf("-- the CM's transaction correlation wins over body[20]\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0155u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	/* A master that rewrote body[20] to something of its own. The envelope
	 * still says which request this answers. */
	len = make_grant(frame, 0xDEADu, 0x0eeeu, VMS_LCK_EX);
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, g.lkb.lkid, frame,
				   len) == DLM_REQ_OK,
		 "the reply is matched by the transaction envelope");
	ct_check_eq_u32(g.grant_calls, 1u, "the engine got it");
	ct_check_eq_u32(g.last_grant.req_lkid, g.lkb.lkid,
			"*** with OUR handle, not the frame's 0xDEAD ***");
}

/* ==========================================================================
 * 19. A re-post adopts the ENGINE's fresh routing decision
 *
 * The engine re-resolved when it filled the new post. If the master has been
 * learned since the first transmission, re-sending to the old DIRECTORY node
 * would be a lookup for a tree whose master the executive already knows.
 * ========================================================================== */
static void test_repost_adopts_new_routing(void)
{
	struct vms_dlm_proxy_post p;

	printf("-- a re-post follows the engine's NEW routing, not the old\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0166u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g.sent[0].dst, CSID_DIR, "the first frame went to the "
					       "directory");

	/* The cluster named the master in between (a grant on a sibling lock,
	 * a rebuild record -- the engine records it on the RSB either way). */
	g.lkb.master_csid = CSID_MASTER;
	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK, "the re-post goes");

	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"still ONE request block");
	ct_check_eq_u32(g.sent[1].dst, CSID_MASTER,
			"*** and it went to the MASTER the engine now names ***");
	ct_check(dlm_req_fsm_find(&g_fsm, g.lkb.lkid)->to_directory == 0u,
		 "the block is no longer addressed at a directory");
	ct_check_eq_u32(g_fsm.requests_sent, 1u, "counted as a request");
}

/* ==========================================================================
 * 20. A request that can NEVER be transmitted still walks off the ladder
 *
 * The beat must not retry forever on a request that is refused before a frame
 * is even built. A bounded wrong answer beats an unbounded silent one: the
 * $ENQW's caller has to be told something.
 * ========================================================================== */
static void test_untransmittable_request_terminates(void)
{
	struct vms_dlm_proxy_post p;
	uint32_t n, beats = 0;

	printf("-- a request that can never be sent FAILS instead of "
	       "spinning\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x0177u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	n = g.n_sent;

	/* The resource block lost its wire-learned hash (a transition discarded
	 * the directory information), so every retransmit is refused. */
	g.lkb.hash_known = 0u;

	while (dlm_req_fsm_outstanding(&g_fsm) > 0u && beats < 64u) {
		g.now_ms += DLM_REQ_RETRY_MS + 1u;
		(void)dlm_req_fsm_tick(&g_fsm);
		beats++;
	}
	ct_check(beats < 64u, "the ladder terminated");
	ct_check(beats <= (uint32_t)DLM_REQ_MAX_TRIES + 1u,
		 "  within the declared retry budget");
	ct_check_eq_u32(g.n_sent, n, "and not one further frame went out");
	ct_check_eq_u32(g.fail_calls, 1u, "the waiter was told");
	ct_check(g.fail_why == DLM_REQ_FAIL_TIMEOUT, "  with a real status");
	ct_check(g_fsm.hash_unknown_refused > 0u,
		 "every refused attempt was counted");
}

int main(void)
{
	printf("== FC-P4.6 R1: the DLM requester FSM ==\n");
	test_full_path();
	test_completion_reads_the_lkb_not_the_frame();
	test_hash_unknown_refuses();
	test_redirect();
	test_assume_mastery();
	test_decline_reresolve_then_stop();
	test_deny_at_master();
	test_convert();
	test_release_is_honestly_unsent();
	test_blkast();
	test_lvb();
	test_retransmit_idempotency();
	test_duplicate_grant();
	test_peer_gone();
	test_observe_learns_the_hash();
	test_refusals();
	test_lock_gone();
	test_correlation();
	test_repost_adopts_new_routing();
	test_untransmittable_request_terminates();
	return ct_summary("test_dlm_requester");
}
