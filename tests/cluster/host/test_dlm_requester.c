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
 * would go red. The single most important case is `master_lkid` on a
 * POST-GRANT frame, because a frame carrying a lock id that came off another
 * frame instead of out of the lock database is literally what bugchecked a
 * real VAX with INVLOCKID and took the cluster down (commit fc8540ae): the
 * test sets the fake LKB's master handle to a value DIFFERENT from the one the
 * grant frame carried, and then asserts the next frame carries the LKB's.
 *
 * THE SUPERSESSION (vms-c03, rd vms-fa7). This file used to interrogate a
 * post-grant "completion 0x04 + commit 0x03" pair. A real 2-node OpenVMS VAX
 * 7.3 capture showed that pair does not exist -- 0x03 is $DEQ, 0x04 is BLKAST,
 * and a real requester answers a grant with NO frame at all -- so the arm no
 * longer emits it. The assertions moved rather than weakened: the fresh-read
 * proof now runs on the post-grant CONVERT (a frame that really exists), and
 * `check_only_grounded_opcodes_were_sent()` is run over EVERY scenario so a
 * reintroduced phantom frame reddens this file wherever it comes back.
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
	uint8_t  write_valblk;   /* engine marks a demote-from-write convert as an
				  * op-0x06 value-block write (vms-727)         */
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

	/*
	 * THE ALL-OVMX GATE, as the connection manager answers it
	 * (vms_ldwv_all_ovmx). 1 == every member is proven to run this
	 * implementation, which is the only configuration a frame shape OVMX
	 * has never been watched to emit may be addressed at.
	 */
	int all_ovmx;

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
	out->write_valblk   = e->lkb.write_valblk;
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

static int fe_all_ovmx(void *ctx)
{
	return ((struct fake_engine *)ctx)->all_ovmx;
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
	g.all_ovmx = 1;   /* the scenarios below are an OVMX-only cluster */

	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.send = fe_send;
	g_ops.refill_post = fe_refill;
	g_ops.dir_resolve = fe_dir_resolve;
	g_ops.dir_generation = fe_dir_generation;
	g_ops.all_ovmx = fe_all_ovmx;
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

/* A captured frame, parsed as an op-0x03 $DEQ through the SHIPPING codec --
 * which is also the gate that says it IS one (cat 0x02, op 0x03, neither lock
 * id unset). Never by byte arithmetic. */
static int parse_deq(const struct sent_frame *s, struct vms_dlm_deq *out)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_frame_info fi;
	uint32_t len = splice(s, frame);

	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_dlm_deq_parse(frame, len, &fi, out) != VMS_CODEC_OK)
		return -1;
	return 0;
}

/* The opcode of a captured frame, read through the codec's own published
 * offset. Used to assert what this arm did NOT emit as well as what it did. */
static uint8_t sent_opcode(const struct sent_frame *s)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	vms_wire_view_t v;
	uint32_t len = splice(s, frame);

	vms_wire_view_init(&v, frame, len);
	return vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
}

/*
 * *** THE SUPERSESSION GUARD. ***
 *
 * Not one frame this arm emitted may be anything but an ENQ (0x01), a CONVERT
 * (0x07) or a $DEQ (0x03) -- the three opcodes it is BOTH grounded for and
 * cleared to transmit. That is stronger than "the completion emit was deleted":
 * it catches a reintroduction anywhere -- a new handler, a new timeout path, a
 * merge that resurrected the phantom pair -- and it catches this arm
 * originating a shape that belongs to the MASTER (an op-0x04 BLKAST) or one
 * whose builder deliberately does not exist (op-0x06's value block). It is
 * checked after every scenario below rather than in one place, because a
 * phantom frame that only appears on the retry ladder is the kind nobody looks
 * for.
 */
static void check_only_grounded_opcodes_were_sent(const char *label)
{
	uint32_t i;
	char what[160];

	for (i = 0; i < g.n_sent; i++) {
		uint8_t op = sent_opcode(&g.sent[i]);

		if (op != VMS_DLM_WIREOP_ENQ && op != VMS_DLM_WIREOP_CONVERT &&
		    op != VMS_DLM_WIREOP_DEQ) {
			snprintf(what, sizeof(what),
				 "%s: frame %u carries opcode 0x%02x -- this "
				 "arm emits ONLY op 0x01 / 0x07 / 0x03",
				 label, (unsigned)i, (unsigned)op);
			ct_check(0, what);
			return;
		}
	}
	snprintf(what, sizeof(what),
		 "%s: all %u emitted frame(s) are op 0x01 / 0x07 / 0x03 -- no "
		 "completion, no commit, no BLKAST, nothing else",
		 label, (unsigned)g.n_sent);
	ct_check(1, what);
}

/*
 * A request block reached its TERMINAL state: ST_GRANTED, settled, with no
 * ladder running. This is the whole post-grant contract in one predicate.
 */
static void check_settled_terminal(uint32_t lkid, const char *label)
{
	const struct dlm_req *r = dlm_req_fsm_find(&g_fsm, lkid);
	char what[160];

	snprintf(what, sizeof(what), "%s: the block is ST_GRANTED", label);
	ct_check(r != NULL && r->state == (uint8_t)DLM_REQ_ST_GRANTED, what);
	if (r == NULL)
		return;

	snprintf(what, sizeof(what),
		 "%s: *** settled -- nothing is outstanding on the wire ***",
		 label);
	ct_check(r->settled == 1u, what);

	snprintf(what, sizeof(what),
		 "%s: no retry ladder is running (tries == 0)", label);
	ct_check_eq_u32(r->tries, 0u, what);
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

/*
 * *** THE RELEASE'S OWN TRACE-TO-THE-LKB PROOF (rd vms-d7a3). ***
 *
 * Same method as check_frame_traces_to_lkb, on the three fields a $DEQ has:
 * re-derive each from the LKB snapshot taken at send time and require the frame
 * to be its image. And then the NEGATIVE half, which is what makes it a field
 * map rather than a wish: the resource NAME must NOT be on the frame (a real
 * DEQ names its lock by lock-id; the reference frame's body[46] is
 * uninitialised), and the directory HASH must not be either.
 */
static void check_deq_traces_to_lkb(const struct sent_frame *s,
				    const char *label)
{
	struct vms_dlm_deq d;
	const struct fake_lkb *l = &s->lkb_at_send;
	size_t namelen = strlen(l->resnam);
	char what[160];
	uint32_t i;
	int name_on_wire = 0;

	snprintf(what, sizeof(what), "%s: parses as a grounded op-0x03 $DEQ",
		 label);
	if (parse_deq(s, &d) != 0) {
		ct_check(0, what);
		return;
	}
	ct_check(1, what);

	snprintf(what, sizeof(what), "%s: body[20:24] == the LKB's own lock id",
		 label);
	ct_check_eq_u32(d.req_lkid, l->lkid, what);

	snprintf(what, sizeof(what),
		 "%s: body[24:28] == the LKB's MASTER handle (what the master's "
		 "own grant recorded -- never a placeholder)", label);
	ct_check_eq_u32(d.master_lkid, l->master_lkid, what);

	snprintf(what, sizeof(what),
		 "%s: body[30] == the mode the LKB holds as it is released",
		 label);
	ct_check_eq_u32(d.mode, l->lkmode, what);

	snprintf(what, sizeof(what),
		 "%s: the resource NAME is NOT on the wire -- a DEQ names its "
		 "lock by lock-id and by nothing else", label);
	if (namelen > 0u) {
		for (i = 0; i + namelen <= s->len; i++) {
			if (memcmp(s->body + i, l->resnam, namelen) == 0)
				name_on_wire = 1;
		}
	}
	ct_check(!name_on_wire, what);

	snprintf(what, sizeof(what),
		 "%s: no directory hash rides a release (it is addressed to "
		 "the MASTER the lock database names)", label);
	ct_check_eq_u32(read_dir_hash(s), 0u, what);
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

/* A GRANT that RETURNS THE MASTER'S VALUE BLOCK (vms-727, the LVB READ
 * crossing) -- built by the SHIPPING grant-with-valblk builder. */
static uint32_t make_grant_valblk(uint8_t *frame, uint32_t req_lkid,
				  uint32_t master_lkid, uint8_t mode,
				  const uint8_t *valblk)
{
	struct vms_cm_link link;
	uint32_t written = 0;

	memset(&link, 0, sizeof(link));
	memset(frame, 0, VMS_CM_FRAME_LEN);
	(void)vms_frame_compose_link(&link, frame, VMS_CM_FRAME_LEN, &written);
	(void)vms_dlm_enq_response_build_grant_valblk(req_lkid, master_lkid, mode,
						      valblk, frame,
						      VMS_CM_FRAME_LEN, &written);
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
 * 1. The full path: directory lookup -> grant -> SETTLED, and that is all
 *
 * The grant is the terminal settle. Nothing follows it on the wire, because
 * the vms-c03 capture of a real OpenVMS VAX 7.3 cluster shows a real
 * requester sends nothing after one (the "completion 0x04 + commit 0x03" pair
 * this arm used to emit was a phantom).
 * ========================================================================== */
static void test_full_path(void)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;
	const struct dlm_req *r;

	printf("-- full path: directory lookup -> grant -> SETTLED (nothing "
	       "follows a grant)\n");
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

	/* *** THE SETTLE-ON-GRANT ASSERTION. *** */
	ct_check_eq_u32(g.n_sent, 1u,
			"*** ONE frame total: the lookup. NOTHING was emitted "
			"in answer to the grant ***");
	check_only_grounded_opcodes_were_sent("full path");
	ct_check_eq_u32(g_fsm.grants_settled, 1u,
			"the grant reached the terminal settled state");
	check_settled_terminal(g.lkb.lkid, "full path");

	/*
	 * NO DANGLING STATE WAITING ON AN ACK. A settled block is skipped by
	 * the beat, so a hundred beats produce no frame and no failure -- the
	 * property the removed completion ladder violated by construction (it
	 * retransmitted a frame no master was ever going to answer).
	 */
	{
		uint32_t beats, ticks_that_sent = 0;

		for (beats = 0; beats < 100u; beats++) {
			g.now_ms += DLM_REQ_RETRY_MS + 1u;
			ticks_that_sent += dlm_req_fsm_tick(&g_fsm);
		}
		ct_check_eq_u32(ticks_that_sent, 0u,
				"100 beats: the beat never had anything to do");
		ct_check_eq_u32(g.n_sent, 1u,
				"*** still ONE frame -- no retransmit ladder, "
				"no completion retry, no timeout ***");
		ct_check_eq_u32(g.fail_calls, 0u,
				"and the waiter was never failed");
	}
	check_settled_terminal(g.lkb.lkid, "full path after 100 beats");

	/* NO LEAKED SLOT: exactly one block, recording the master it heard
	 * from. */
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"exactly ONE request block is held");
	r = dlm_req_fsm_find(&g_fsm, g.lkb.lkid);
	ct_check(r != NULL && r->dst_csid == CSID_DIR,
		 "and it records the MASTER the grant came from");
}

/* ==========================================================================
 * 1b. A GRANTED LOCK IS USABLE AND RELEASABLE -- with no completion round
 *     trip anywhere in sight.
 *
 * "The emit is gone" is not the claim. The claim is that the post-grant path
 * is COMPLETE: the lock the grant produced can be CONVERTED (a real op-0x07
 * goes out, every field built from a fresh executive read), and it can be
 * RELEASED (the $DEQ post is taken on the granted path and the block returns
 * to IDLE with no slot leaked). Neither is blocked behind an acknowledgement,
 * because there is no acknowledgement.
 * ========================================================================== */
static void test_granted_lock_is_usable_and_releasable(void)
{
	struct vms_dlm_proxy_post p;
	struct vms_dlm_enq_request req;
	struct vms_dlm_deq d;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t opcode = 0;
	uint32_t len, n;

	printf("-- a lock granted with NO completion round trip is usable and "
	       "releasable\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_CR, 0x0155u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	len = make_grant(frame, g.lkb.lkid, 0x0C0FFEE0u, VMS_LCK_CR);
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len) ==
		 DLM_REQ_OK, "the grant is accepted");
	ct_check_eq_u32(g.n_sent, 1u,
			"the ENQ is the only frame: no completion followed");
	check_settled_terminal(g.lkb.lkid, "granted");

	/* ---- USABLE: a CONVERT on the settled lock really transmits ---- */
	n = g.n_sent;
	g.lkb.lkmode = VMS_LCK_EX;   /* the $ENQ raised the requested mode */
	post_from_lkb(&p, VMS_DLM_POST_CONVERT, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK,
		 "*** a CONVERT on the settled lock is taken ***");
	ct_check_eq_u32(g.n_sent, n + 1u, "and one frame went out");
	ct_check(parse_request(&g.sent[n], &opcode, &req) == 0 &&
		 opcode == VMS_DLM_WIREOP_CONVERT, "  it is op 0x07");
	ct_check_eq_u32(req.master_lkid, 0x0C0FFEE0u,
			"  carrying the master handle the GRANT put in the LKB "
			"-- read back out of the executive, not remembered");
	check_frame_traces_to_lkb(&g.sent[n], "post-grant convert");

	/* The master refuses the convert: the lock stays real, at its old mode,
	 * and the block SETTLES again rather than waiting on anything. */
	len = make_deny(frame, g.lkb.lkid, 0x0C0FFEE0u, "F11B$aSYSDSK1");
	ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len) ==
		 DLM_REQ_OK, "a refused convert is an ANSWER, not a hang");
	check_settled_terminal(g.lkb.lkid, "after a refused convert");
	n = g.n_sent;
	{
		uint32_t beats;

		for (beats = 0; beats < 16u; beats++) {
			g.now_ms += DLM_REQ_RETRY_MS + 1u;
			(void)dlm_req_fsm_tick(&g_fsm);
		}
	}
	ct_check_eq_u32(g.n_sent, n,
			"  and the beat leaves the re-settled block alone");

	/* ---- RELEASABLE: the $DEQ is EMITTED, and the block freed ---- */
	n = g.n_sent;
	g.lkb.lkmode = VMS_LCK_NL;   /* the mode this lock holds as it goes */
	post_from_lkb(&p, VMS_DLM_POST_DEQ, CSID_MASTER);
	ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK,
		 "*** the $DEQ is TRANSMITTED (rd vms-d7a3) ***");
	ct_check_eq_u32(g.n_sent, n + 1u, "  one frame went out");
	ct_check_eq_u32(sent_opcode(&g.sent[n]), VMS_DLM_WIREOP_DEQ,
			"  and it is a grounded op-0x03 $DEQ");
	ct_check_eq_u32(g.sent[n].dst, CSID_MASTER,
			"  addressed to the MASTER, never to a directory node");
	check_deq_traces_to_lkb(&g.sent[n], "release");
	ct_check_eq_u32(g_fsm.releases_sent, 1u, "  counted as sent");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 0u,
			"  and NOT counted as a gap");

	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"*** the $DEQ released the block: NO leaked req slot ***");
	ct_check(dlm_req_fsm_find(&g_fsm, g.lkb.lkid) == NULL,
		 "  and the handle finds nothing");
	check_only_grounded_opcodes_were_sent("usable-and-releasable");

	/*
	 * THE CODEC-LEVEL REFUSAL IS STILL THE FLOOR UNDER THE EMIT. The same
	 * builder the FSM just used refuses the placeholder that bugchecked a
	 * real VAX with INVLOCKID (fc8540ae) -- so "the arm emits a release
	 * now" can never become "the arm emits a release naming lock 0".
	 */
	memset(&d, 0, sizeof(d));
	d.req_lkid = g.lkb.lkid;
	d.master_lkid = 0x0C0FFEE0u;
	d.mode = VMS_LCK_NL;
	ct_check(vms_dlm_deq_build(&d, frame, sizeof(frame), &len) ==
		 VMS_CODEC_OK,
		 "the codec builds a grounded op-0x03 for real handles");
	d.master_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_deq_build(&d, frame, sizeof(frame), &len) ==
		 VMS_CODEC_E_INVAL,
		 "  while still refusing the fc8540ae placeholder");
}

/* ==========================================================================
 * 2. *** THE ANTI-LARP ASSERTION ***, after the supersession
 *
 * The rule is unchanged: a master handle on the wire comes from a FRESH read
 * of the lock database, never from the grant frame that arrived a microsecond
 * earlier. What changed is which frame carries it. The completion this test
 * used to interrogate does not exist on a real wire (vms-c03), so the proof
 * moved to the frame that DOES follow a grant when the executive has more to
 * say: the post-grant CONVERT.
 *
 * The method is identical and it is the only one that can prove the claim:
 * MAKE THE TWO VALUES DIFFER. The grant frame says 0x11111111, the engine
 * records it, and then a REMASTER moves the executive's own record to
 * 0x22222222 before the next frame is built. A requester that cached the
 * grant's value is visibly wrong here.
 * ========================================================================== */
static void test_post_grant_frame_reads_the_lkb_not_the_frame(void)
{
	struct vms_dlm_proxy_post p;
	struct vms_dlm_enq_request req;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len, refills_before, n;
	uint8_t opcode = 0;

	printf("-- a post-grant frame's master handle comes from the LKB, not "
	       "from the grant (fc8540ae)\n");
	fe_reset("LNM$CWLOGICALS", VMS_LCK_EX, 0x4321u, 1, CSID_MASTER);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g_fsm.requests_sent, 1u,
			"a known master gets a REQUEST, not a lookup");

	len = make_grant(frame, g.lkb.lkid, 0x11111111u, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g.lkb.master_lkid, 0x11111111u,
			"the engine recorded the grant's handle");
	ct_check_eq_u32(g.n_sent, 1u,
			"and answered it with NO frame (the grant settles)");

	/* ---- part 1: the POST path ---- */
	g.lkb.master_lkid = 0x22222222u;   /* a remaster: the truth moves */
	n = g.n_sent;

	post_from_lkb(&p, VMS_DLM_POST_CONVERT, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	ct_check_eq_u32(g.n_sent, n + 1u, "one convert frame went out");
	ct_check(parse_request(&g.sent[n], &opcode, &req) == 0 &&
		 opcode == VMS_DLM_WIREOP_CONVERT, "it parses as op 0x07");
	ct_check_eq_u32(req.master_lkid, 0x22222222u,
			"*** it carries the LKB's CURRENT handle, NOT the "
			"0x11111111 the grant frame carried ***");
	check_frame_traces_to_lkb(&g.sent[n], "post-remaster convert");

	/*
	 * ---- part 2: the RETRANSMIT path, where the FSM does its OWN read ----
	 *
	 * Part 1 proves the FSM did not reach back for the grant frame's value;
	 * the post it built from was filled by the engine. This part closes the
	 * other half: when the FSM retransmits on its own initiative, it calls
	 * `refill_post` and builds from THAT. Move the executive's truth a
	 * second time with no post in sight, and require the retransmitted
	 * frame to follow.
	 */
	g.lkb.master_lkid = 0x33333333u;
	refills_before = g.refills;
	n = g.n_sent;

	g.now_ms += DLM_REQ_RETRY_MS + 1u;
	ct_check_eq_u32(dlm_req_fsm_tick(&g_fsm), 1u,
			"the unanswered convert is retransmitted by the beat");
	ct_check(g.refills > refills_before,
		 "*** and the retransmit RE-READ the lock database ***");
	ct_check_eq_u32(g.n_sent, n + 1u, "one more frame went out");
	ct_check(parse_request(&g.sent[n], &opcode, &req) == 0 &&
		 opcode == VMS_DLM_WIREOP_CONVERT, "it is op 0x07 again");
	ct_check_eq_u32(req.master_lkid, 0x33333333u,
			"*** carrying the handle the executive holds NOW ***");

	check_only_grounded_opcodes_were_sent("anti-LARP");
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
 * 4b. THE REDIRECT BUDGET TERMINATES (rd vms-b96's other half)
 *
 * Now that a mis-addressed inbound request is ANSWERED with a master CSID
 * (vms_lock.c's enq_inbound_not_master) rather than declined blind, the
 * termination argument has two halves and this is the second one.
 *
 * A redirect is a REPLY, not a forward, so no chain of nodes can form on the
 * wire -- but two systems whose weight vectors disagree could still ping-pong
 * one request between them for ever. The bound that stops that lives HERE, in
 * the requester: DLM_REQ_MAX_REDIRECTS answers followed and no more, after
 * which the waiter is failed honestly instead of frame N+1 leaving this node.
 * Assert the COUNT and the SILENCE, not the intent -- an unbounded build
 * passes every other assertion in this file.
 * ========================================================================== */
static void test_redirect_budget_terminates(void)
{
	struct vms_dlm_proxy_post p;
	uint32_t i, sent_before;

	printf("-- an endless ping-pong of redirects TERMINATES at the "
	       "budget\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_CR, 0x00A5u, 1, 0u);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_DIR);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	/* Every answer names a DIFFERENT master, so nothing but the budget can
	 * stop this: a build without one sends a frame per answer, for ever. */
	for (i = 0; i < (uint32_t)DLM_REQ_MAX_REDIRECTS; i++)
		ct_check(dlm_req_fsm_redirect(&g_fsm, g.lkb.lkid,
					      (vms_csid_t)(CSID_MASTER + i)) ==
			 DLM_REQ_OK,
			 "a redirect WITHIN the budget is followed");
	ct_check_eq_u32(g_fsm.redirects_followed,
			(uint32_t)DLM_REQ_MAX_REDIRECTS,
			"the budget's worth, and they were counted");

	sent_before = g.n_sent;
	ct_check(dlm_req_fsm_redirect(&g_fsm, g.lkb.lkid,
				      (vms_csid_t)(CSID_MASTER +
						   DLM_REQ_MAX_REDIRECTS)) !=
		 DLM_REQ_OK,
		 "*** the redirect PAST the budget is refused ***");
	ct_check_eq_u32(g.n_sent, sent_before,
			"*** and NOT ONE further frame went on the wire ***");
	ct_check_eq_u32(g.fail_calls, 1u,
			"the waiter was failed instead -- honestly");
	ct_check(g.fail_why == DLM_REQ_FAIL_UNROUTABLE,
		 "  with UNROUTABLE, never a fabricated success");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u,
			"and no request block was left outstanding");
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
 * 9. THE RELEASE'S GATES -- the NEGATIVE CONTROLS (rd vms-d7a3)
 *
 * The positive case (a real op-0x03 built from the LKB and sent to the master)
 * is proved in test_granted_lock_is_usable_and_releasable. THIS is the half
 * that has teeth: every way a release must NOT reach a wire. Each one is
 * checked by the pair (nothing sent, the gap counted) -- because a gate that
 * only stops the frame, without saying so, is indistinguishable from a bug.
 *
 * A regression that widens any gate reddens this function.
 * ========================================================================== */

/* Drive one granted cross-node lock to the point of release. Returns the
 * frame count at which the release is about to be posted. */
static uint32_t release_setup(uint32_t master_lkid)
{
	struct vms_dlm_proxy_post p;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t len;

	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00ccu, 1, CSID_MASTER);
	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);
	len = make_grant(frame, g.lkb.lkid, master_lkid, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	return g.n_sent;
}

/* Post the release and assert that NOTHING went out and the gap was counted. */
static void expect_release_refused(uint32_t n_before, const char *label)
{
	struct vms_dlm_proxy_post p;
	char what[192];

	post_from_lkb(&p, VMS_DLM_POST_DEQ, CSID_MASTER);
	(void)dlm_req_fsm_post(&g_fsm, &p);

	snprintf(what, sizeof(what), "%s: *** NOTHING went on the wire ***",
		 label);
	ct_check_eq_u32(g.n_sent, n_before, what);

	snprintf(what, sizeof(what), "%s: the refusal is COUNTED", label);
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 1u, what);

	snprintf(what, sizeof(what), "%s: and nothing is counted as sent",
		 label);
	ct_check_eq_u32(g_fsm.releases_sent, 0u, what);

	snprintf(what, sizeof(what),
		 "%s: the wire record is dropped: we no longer hold the lock",
		 label);
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 0u, what);
}

static void test_release_gates(void)
{
	uint32_t n;

	printf("-- the release's NEGATIVE CONTROLS: every way a $DEQ must not "
	       "reach a wire\n");

	/*
	 * (a) RULE C. The connection manager refuses to transmit DLM traffic to
	 * a system whose advertised software version is not byte-identical to
	 * ours (cnxman_dlm_peer_proven / csb->peer_is_ours), and a refused
	 * `send` is exactly how that reaches this object. The gate itself is
	 * driven against the real derivation in test_dlm_scs_arm.c; what is
	 * proved HERE is that the FSM takes the refusal as a refusal -- no
	 * retry, no second frame, nothing recorded as sent.
	 */
	n = release_setup(0x0888u);
	g.send_fails = 1;                 /* what an unproven peer looks like */
	expect_release_refused(n, "RULE C (the peer is not proven ours)");
	ct_check_eq_u32(g_fsm.send_failures, 1u,
			"  the connection manager's refusal is counted too");

	/*
	 * (b) THE ALL-OVMX GATE. op 0x03 is grounded but has never been watched
	 * to leave OVMX for a real peer, so it may not be addressed at a
	 * cluster that is not all-proven-OVMX -- even one whose members each
	 * hold an open connection.
	 */
	n = release_setup(0x0888u);
	g.all_ovmx = 0;
	expect_release_refused(n, "the all-OVMX gate (a mixed cluster)");
	ct_check_eq_u32(g.logs > 0u ? 1u : 0u, 1u, "  and said out loud");

	/*
	 * (b') THE GATE IS FAIL-CLOSED. An arm wired with no `all_ovmx` op at
	 * all holds no proof that every member is ours, and "nobody told us"
	 * may not read as "go ahead".
	 */
	n = release_setup(0x0888u);
	g_ops.all_ovmx = NULL;
	dlm_req_fsm_init(&g_fsm, &g_ops);   /* rebind: same ops, gate absent */
	{
		struct vms_dlm_proxy_post p;

		/* the block went with the re-init, so this is the untracked
		 * release path -- which must be gated identically */
		post_from_lkb(&p, VMS_DLM_POST_DEQ, CSID_MASTER);
		(void)dlm_req_fsm_post(&g_fsm, &p);
	}
	ct_check_eq_u32(g.n_sent, n,
			"an ABSENT all-OVMX op is CLOSED, not permissive "
			"(untracked release path)");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 1u, "  and counted");
	g_ops.all_ovmx = fe_all_ovmx;

	/*
	 * (c) THE CODEC's LOCK-ID REFUSAL, reached through the FSM. A lock the
	 * master never named has master_lkid 0 in the lock database, and a
	 * release naming lock 0 is not a release. This is the fc8540ae
	 * placeholder path: the value that bugchecked a real VAX with
	 * INVLOCKID cannot be reached even by a caller that wants it, because
	 * the only source for the field is the LKB and the codec refuses the
	 * one value the LKB uses for "not a real lock yet".
	 */
	n = release_setup(0x0888u);
	g.lkb.master_lkid = 0u;           /* the master never named one */
	expect_release_refused(n, "a release naming lock 0 (fc8540ae)");
	ct_check_eq_u32(g_fsm.codec_failures, 1u,
			"  the codec refused to BUILD it -- counted");

	/*
	 * (d) NO ROUTE. The lock database names no destination, so there is
	 * nobody to release it at.
	 */
	n = release_setup(0x0888u);
	{
		struct vms_dlm_proxy_post p;

		post_from_lkb(&p, VMS_DLM_POST_DEQ, 0u);
		p.dst_csid = 0u;
		(void)dlm_req_fsm_post(&g_fsm, &p);
	}
	ct_check_eq_u32(g.n_sent, n, "no route: *** nothing went on the wire ***");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 1u, "  counted");
	ct_check_eq_u32(g_fsm.dir_unresolved, 1u,
			"  and the REASON is counted where it was decided");

	/*
	 * AND THE POSITIVE CONTROL, same setup, gates open: the negative cases
	 * above must be the GATES talking, not a release path that never works.
	 */
	n = release_setup(0x0888u);
	{
		struct vms_dlm_proxy_post p;

		post_from_lkb(&p, VMS_DLM_POST_DEQ, CSID_MASTER);
		ct_check(dlm_req_fsm_post(&g_fsm, &p) == DLM_REQ_OK,
			 "*** gates open: the same release IS transmitted ***");
	}
	ct_check_eq_u32(g.n_sent, n + 1u, "  one op-0x03 frame went out");
	check_deq_traces_to_lkb(&g.sent[n], "gates-open release");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 0u, "  and no gap counted");
	check_only_grounded_opcodes_were_sent("release gates");
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

	/*
	 * CASE A: an ENQ that carries a value block does NOT write it on the
	 * wire and is NOT an unsent "write crossing" -- an ENQ (and any
	 * non-demoting request) READS the block on grant; only a demote from a
	 * write mode WRITES it (vms-727). So the block rides in the post, never
	 * on the frame, and no counter moves.
	 */
	printf("-- an ENQ reads the LVB (block in the post, never on the wire)\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00eeu, 1, CSID_MASTER);
	for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
		g.lkb.valblk[i] = (uint8_t)(0xA0u + i);

	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	ct_check(p.valblk[0] == 0xA0u,
		 "the post carries the LKB's real value block");
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g_fsm.lvb_write_no_wire_field, 0u,
			"an ENQ is not a write crossing -- nothing unsent");
	ct_check_eq_u32(g_fsm.lvb_writes_sent, 0u,
			"and nothing was written to the wire");
	check_frame_traces_to_lkb(&g.sent[0], "enq with a value block");

	/* And an inbound grant must not zero the proxy's block. */
	len = make_grant(frame, g.lkb.lkid, 0x0aaau, VMS_LCK_EX);
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g.last_grant.valblk_present, 0u,
			"the grant is handed to the engine with "
			"valblk_present = 0");
	ct_check(g.lkb.valblk[0] == 0xA0u,
		 "*** so the proxy's own value block SURVIVED the grant ***");

	/*
	 * CASE B: a CONVERT the engine marked as a value-block WRITE (a demote
	 * from a write mode with LCK$M_VALBLK) goes out as a grounded op-0x06
	 * CONVERT-with-VALBLK, and THE BLOCK IS ON THE WIRE, verbatim -- the
	 * write crossing that used to be dropped now crosses (vms-727).
	 */
	printf("-- a demote-from-write CONVERT writes the LVB (op-0x06 on the wire)\n");
	fe_reset("OVMXLV01", VMS_LCK_NL, 0x00eeu, 1, CSID_MASTER);
	g.lkb.master_lkid = 0x04000669u;   /* the master named it */
	g.lkb.lkmode = VMS_LCK_NL;          /* converting DOWN to NL */
	g.lkb.write_valblk = 1u;            /* the engine's demote-from-write mark */
	for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
		g.lkb.valblk[i] = (uint8_t)(0xB0u + i);

	post_from_lkb(&p, VMS_DLM_POST_CONVERT, CSID_MASTER);
	ct_check(p.write_valblk == 1u, "the post is marked a value-block write");
	(void)dlm_req_fsm_post(&g_fsm, &p);
	ct_check_eq_u32(g_fsm.lvb_writes_sent, 1u,
			"*** the op-0x06 value-block write was EMITTED ***");
	ct_check_eq_u32(g_fsm.lvb_write_no_wire_field, 0u,
			"and nothing was dropped");

	{
		struct vms_frame_info fi;
		struct vms_dlm_valblk_convert c;

		len = splice(&g.sent[g.n_sent - 1u], frame);
		ct_check(vms_frame_classify(frame, len, &fi) == VMS_CODEC_OK &&
			 vms_dlm_valblk_convert_parse(frame, len, &fi, &c) ==
				 VMS_CODEC_OK,
			 "the emitted frame parses as an op-0x06 value-block CONVERT");
		ct_check_eq_u32(c.master_lkid, 0x04000669u,
				"  body[24:28] == the LKB's master handle");
		ct_check_eq_u32(c.mode, VMS_LCK_NL,
				"  body[30] == the mode converted TO (NL)");
		ct_check(memcmp(c.valblk, g.lkb.valblk, VMS_DLM_VALBLK_LEN) == 0,
			 "*** body[36:52] IS the LKB's value block, on the wire ***");
	}

	/*
	 * CASE C: THE LVB READ CROSSING (vms-727). A grant that RETURNS the
	 * master's value block is recognised by the codec, and h_grant hands it to
	 * the engine as valblk_present=1 -- so the engine records the master's
	 * block on the proxy. The proof is a STATE DELTA: the proxy's block was one
	 * value before the grant and is the MASTER'S after it.
	 */
	printf("-- a grant that carries the master's LVB applies it (the READ crossing)\n");
	{
		static const uint8_t master_block[VMS_DLM_VALBLK_LEN] =
			{ 'W','R','O','T','E','B','Y','V','A','X','1','X','X','X','X','X' };

		fe_reset("OVMXLV01", VMS_LCK_EX, 0x00efu, 1, CSID_MASTER);
		/* the proxy starts with a DIFFERENT (stale) block, so an apply is
		 * observable as a change, not a coincidence. */
		for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
			g.lkb.valblk[i] = 0x11u;

		post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
		(void)dlm_req_fsm_post(&g_fsm, &p);

		len = make_grant_valblk(frame, g.lkb.lkid, 0x0abcu, VMS_LCK_EX,
					master_block);
		ct_check(dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len) ==
			 DLM_REQ_OK, "the grant-with-valblk is accepted");
		ct_check_eq_u32(g.last_grant.valblk_present, 1u,
				"*** the grant is handed to the engine with "
				"valblk_present = 1 ***");
		ct_check(memcmp(g.last_grant.valblk, master_block,
				VMS_DLM_VALBLK_LEN) == 0,
			 "  the block handed over IS the master's, byte for byte");
		ct_check(memcmp(g.lkb.valblk, master_block, VMS_DLM_VALBLK_LEN) == 0,
			 "*** STATE DELTA: the proxy's value block is now the "
			 "MASTER'S (0x11.. -> 'WROTEBYVAX1XXXXX') ***");

		/*
		 * THE NEVER-CORRUPT GATE. When the engine holds no proxy this grant
		 * can belong to (grant_recv refuses -- the real path's SS$_IVLOCKID),
		 * the FSM records NOTHING: it counts the reply UNMATCHED and the proxy
		 * block is left exactly as it was, never overwritten by a block for a
		 * lock this node does not hold.
		 */
		{
			uint32_t unmatched0;

			fe_reset("OVMXLV01", VMS_LCK_EX, 0x00f0u, 1, CSID_MASTER);
			for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
				g.lkb.valblk[i] = 0x22u;
			post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
			(void)dlm_req_fsm_post(&g_fsm, &p);
			unmatched0 = g_fsm.replies_unmatched;
			g.grant_refuse = 1;   /* the engine owns no such lock */

			len = make_grant_valblk(frame, g.lkb.lkid, 0x0abcu, VMS_LCK_EX,
						master_block);
			(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
			ct_check_eq_u32(g_fsm.replies_unmatched, unmatched0 + 1u,
					"a grant the engine refuses is UNMATCHED "
					"(the never-corrupt gate)");
			ct_check(g.lkb.valblk[0] == 0x22u,
				 "*** the proxy's block was NOT overwritten by a grant "
				 "the engine refused ***");
			g.grant_refuse = 0;
		}
	}
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
	ct_check_eq_u32(g_fsm.grants_settled, 1u, "the first grant settled");
	ct_check_eq_u32(g.n_sent, 1u, "one frame: the ENQ");

	/* The master retransmits. The executive's record moves first, so the
	 * re-apply can be seen to be a real re-apply and not a no-op. */
	g.lkb.master_lkid = 0u;
	(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame, len);
	ct_check_eq_u32(g_fsm.grants_duplicate, 1u, "the duplicate is counted");
	ct_check_eq_u32(g.grant_calls, 2u,
			"and RE-APPLIED to the engine (idempotent on the key)");
	ct_check_eq_u32(g.lkb.master_lkid, 0x0bbbu,
			"  which put the handle THIS frame carried back in the "
			"lock database");
	ct_check_eq_u32(g_fsm.grants_settled, 2u, "and settled again");

	/* *** THE STORM THAT CANNOT START. *** A retransmitting master used to
	 * pump one completion PAIR out of this arm per received frame. Now a
	 * duplicate grant costs zero frames, however many arrive. */
	ct_check_eq_u32(g.n_sent, 1u,
			"*** the duplicate drew NO frame: a retransmitting "
			"master cannot pump this node ***");
	{
		uint32_t i;

		for (i = 0; i < 20u; i++)
			(void)dlm_req_fsm_reply(&g_fsm, CSID_MASTER, 0u, frame,
						len);
	}
	ct_check_eq_u32(g.n_sent, 1u, "  nor did twenty more of them");
	ct_check_eq_u32(dlm_req_fsm_outstanding(&g_fsm), 1u,
			"*** still ONE request block ***");
	check_settled_terminal(g.lkb.lkid, "after 21 duplicate grants");
	check_only_grounded_opcodes_were_sent("duplicate grant");
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


/* ==========================================================================
 * 23. THE RELEASE QUEUE (rd vms-49f8) -- the object that gives the op-0x03
 *     emit a reachable caller
 *
 * Section 9 above proves the emit and its gates by handing the FSM a release
 * post. THE BUG was that production never could: a $DEQ destroys the proxy LKB
 * it would be rebuilt from, so the fork thread's `refill_post` answered "no such
 * lock" and nothing was ever sent (`releases_sent=0`, `posts_lock_gone=2` on the
 * live 2-node rig). `struct dlm_relq` is the thread crossing that fixes it: the
 * release is SNAPSHOTTED in the releaser's own context, out of the post the
 * engine just read from the live LKB, and the fork thread emits from that.
 *
 * This section is the queue's own R1. The end-to-end proof -- a REAL $ENQ, a
 * REAL grant, a REAL $DEQ through vms_lock.c -- is test_dlm_deq_reachable.c.
 * ========================================================================== */

/* What the engine's post looks like for a release of the current fake LKB. */
static void release_post(struct vms_dlm_proxy_post *p, vms_csid_t dst)
{
	post_from_lkb(p, VMS_DLM_POST_DEQ, dst);
}

static void test_relq_snapshot_is_the_lkb_read(void)
{
	struct vms_dlm_proxy_post p, out;
	struct dlm_relq q;
	uint32_t slot = 0u, seq = 0u;

	printf("-- the release queue: a snapshot of the LKB read, and NOTHING "
	       "a release does not carry\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_PW, 0x00abu, 1, CSID_MASTER);
	g.lkb.master_lkid = 0x0ABCDEF0u;
	dlm_relq_init(&q);

	release_post(&p, CSID_MASTER);
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_OK,
		 "a release post is staged");
	ct_check(seq != 0u, "  under a non-zero generation (0 is 'free')");
	ct_check_eq_u32(dlm_relq_pending(&q), 1u, "  and is pending");
	ct_check_eq_u32(q.staged, 1u, "  counted");

	ct_check(dlm_relq_claim(&q, slot, seq, &out) == DLM_REQ_OK,
		 "the fork thread claims it");
	ct_check_eq_u32(out.op, VMS_DLM_POST_DEQ, "  as a RELEASE");
	ct_check_eq_u32(out.req_lkid, g.lkb.lkid,
			"  carrying the proxy LKB's own handle");
	ct_check_eq_u32(out.master_lkid, 0x0ABCDEF0u,
			"  the master's handle as the LKB held it");
	ct_check_eq_u32(out.lkmode, g.lkb.lkmode,
			"  the mode the LKB was released at");
	ct_check_eq_u32(out.dst_csid, CSID_MASTER, "  addressed to the master");

	/*
	 * *** THE STRUCTURAL OMISSION. *** A release carries no resource name,
	 * no value block, no directory index -- those are fields of a REQUEST.
	 * They are not in the record, so they cannot be in the post, so no later
	 * edit can put a stale one on a wire.
	 */
	ct_check(out.resnam[0] == '\0',
		 "*** the claimed post carries NO resource name ***");
	ct_check_eq_u32(out.dir_hash_known, 0u, "  no directory hash");
	ct_check_eq_u32(out.to_directory, 0u,
			"  and it is not a directory lookup");
	{
		uint32_t i, nz = 0u;

		for (i = 0; i < VMS_DLM_VALBLK_LEN; i++)
			nz += out.valblk[i] != 0u ? 1u : 0u;
		ct_check_eq_u32(nz, 0u, "  and no value block");
	}

	ct_check_eq_u32(dlm_relq_pending(&q), 0u, "the slot is given back");
	ct_check_eq_u32(q.claimed, 1u, "the claim is counted");
}

static void test_relq_refusals(void)
{
	struct vms_dlm_proxy_post p, out;
	struct dlm_relq q;
	uint32_t slot = 0u, seq = 0u, i;

	printf("-- the release queue's REFUSALS: not a release, no room, "
	       "claimed twice\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_EX, 0x00acu, 1, CSID_MASTER);
	dlm_relq_init(&q);

	/* Only a release belongs here: everything else refills. */
	post_from_lkb(&p, VMS_DLM_POST_ENQ, CSID_MASTER);
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_E_INVAL,
		 "an ENQ post is REFUSED: only a release is staged");

	/* The engine's own lock-id rule, mirrored. */
	release_post(&p, CSID_MASTER);
	p.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_E_INVAL,
		 "a release with no handle of ours is REFUSED (never lock 0)");
	ct_check_eq_u32(dlm_relq_pending(&q), 0u, "nothing was staged");

	/* FULL is a counted refusal, never an eviction: an evicted release is a
	 * lock the master still believes we hold. */
	for (i = 0; i < DLM_RELQ_SLOTS; i++) {
		release_post(&p, CSID_MASTER);
		p.req_lkid = 0x3000u + i;
		ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_OK,
			 i == 0u ? "the queue fills with real releases" :
				   "  (another slot taken)");
	}
	release_post(&p, CSID_MASTER);
	p.req_lkid = 0x4000u;
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_E_NOSLOT,
		 "*** one past capacity is REFUSED ***");
	ct_check_eq_u32(q.full_refused, 1u, "  counted");
	ct_check_eq_u32(dlm_relq_pending(&q), DLM_RELQ_SLOTS,
			"  and nothing already staged was evicted");

	/* A staging is claimed ONCE. */
	ct_check(dlm_relq_claim(&q, 0u, q.slot[0].seq, &out) == DLM_REQ_OK,
		 "the first slot claims");
	ct_check(dlm_relq_claim(&q, 0u, out.req_lkid, &out) != DLM_REQ_OK,
		 "*** a second claim of the same slot names NOTHING ***");
	ct_check(q.stale_refused > 0u, "  and is counted, never guessed");

	/* An abandoned staging gives the slot back and emits nothing. */
	release_post(&p, CSID_MASTER);
	p.req_lkid = 0x5000u;
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_OK,
		 "a fresh staging takes the freed slot");
	dlm_relq_abandon(&q, slot, seq);
	ct_check_eq_u32(q.abandoned, 1u,
			"abandoning it (the fork queue refused the work item) "
			"is COUNTED");
	ct_check(dlm_relq_claim(&q, slot, seq, &out) != DLM_REQ_OK,
		 "  and it can never be claimed afterwards");
}

/*
 * *** THE REACHABILITY PROPERTY, at the FSM's own rung. *** The LKB is
 * DESTROYED between the staging and the claim -- which is exactly what a $DEQ
 * does -- and the op-0x03 still goes out, built from the snapshot, while a
 * refill of the same handle fails. This is the shape of the bug and the shape
 * of the fix in one scenario.
 */
static void test_relq_survives_the_lkb(void)
{
	struct vms_dlm_proxy_post p, claimed;
	struct dlm_relq q;
	uint32_t slot = 0u, seq = 0u, n;
	struct vms_dlm_deq d;

	printf("-- a staged release SURVIVES the LKB's death and still emits\n");
	fe_reset("F11B$aSYSDSK1", VMS_LCK_CR, 0x00adu, 1, CSID_MASTER);
	g.lkb.master_lkid = 0x0DEFACE0u;
	dlm_relq_init(&q);

	release_post(&p, CSID_MASTER);
	ct_check(dlm_relq_stage(&q, &p, &slot, &seq) == DLM_REQ_OK,
		 "the release is staged while the LKB is still real");

	g.lkb.exists = 0;   /* the $DEQ tore it down */
	{
		struct vms_dlm_proxy_post refilled;

		ct_check(fe_refill(&g, p.req_lkid, VMS_DLM_POST_DEQ,
				   CSID_MASTER, &refilled) != 0,
			 "*** a refill now FAILS -- the old path's dead end ***");
	}

	n = g.n_sent;
	ct_check(dlm_relq_claim(&q, slot, seq, &claimed) == DLM_REQ_OK,
		 "the fork thread claims the snapshot instead");
	ct_check(dlm_req_fsm_post(&g_fsm, &claimed) == DLM_REQ_OK,
		 "*** and the FSM really transmits the op-0x03 ***");
	ct_check_eq_u32(g.n_sent, n + 1u, "  one frame went out");
	ct_check_eq_u32(sent_opcode(&g.sent[n]), VMS_DLM_WIREOP_DEQ,
			"  a grounded op-0x03 $DEQ");
	ct_check(parse_deq(&g.sent[n], &d) == 0, "  it parses through the codec");
	ct_check_eq_u32(d.master_lkid, 0x0DEFACE0u,
			"  naming the master's handle the LKB held at release");
	ct_check_eq_u32(d.mode, VMS_LCK_CR,
			"  and the mode it was really released at");
	ct_check_eq_u32(g_fsm.releases_sent, 1u, "counted as sent");
	ct_check_eq_u32(g_fsm.releases_no_wire_op, 0u, "and not as a gap");
	check_only_grounded_opcodes_were_sent("staged release");
}

int main(void)
{
	printf("== FC-P4.6 R1: the DLM requester FSM ==\n");
	test_full_path();
	test_granted_lock_is_usable_and_releasable();
	test_post_grant_frame_reads_the_lkb_not_the_frame();
	test_hash_unknown_refuses();
	test_redirect();
	test_redirect_budget_terminates();
	test_assume_mastery();
	test_decline_reresolve_then_stop();
	test_deny_at_master();
	test_convert();
	test_release_gates();
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
	test_relq_snapshot_is_the_lkb_read();
	test_relq_refusals();
	test_relq_survives_the_lkb();
	return ct_summary("test_dlm_requester");
}
