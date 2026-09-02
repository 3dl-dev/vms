/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_barrier.c - the state-transition BARRIER, participant side
 * (FC-P3.5, test-ladder rung R1).
 *
 * WHAT THIS PROVES, AND WHY EACH CASE EXISTS. The failure mode of this FSM is
 * not "our node does not join" -- it is "the cluster breaks": a participant that
 * ignores the barrier leaves the coordinator permanently one member short, the
 * transition times out, and HEALTHY MEMBERS ARE DROPPED (spec sec 4(p),
 * observed twice on the real lab cluster). So every participant obligation gets
 * a case, and the two that killed real VAXes -- answering something ungrounded,
 * and answering a notification -- get NEGATIVE cases that fail if a response is
 * ever emitted.
 *
 * GROUNDING. Wire: docs/cluster-protocol-spec.md sec 4(o)/(p)/(q)/(r) -- the
 * 12-step census over 41 captures, the never-answered notifications, the three
 * echo mutations, the membership bitmap, the role/class byte pair, the
 * interleaved cat-0x02 op-0x0d records. Book: *VAXcluster Principles* (Davis
 * 1993) pp. 7-40..7-42 -- Phase 1 proposal + acknowledgement, Phase 2 commit
 * ("point of no return"), and THE COUNT COMMITTING IN PHASE 2 BEFORE THE
 * SYNCHRONISED REBUILD. Host-only transcript, page cites only (Rule 8).
 *
 * The clock, the link counters and the lock manager are all INJECTED, so a
 * whole 12-step transition with 216 interleaved rebuild records runs here in
 * microseconds with no wire, no daemon and no boot.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * The bed: one node, a coordinator CSB, and a recording transport
 * ========================================================================== */

#define COORD_CSID 0x00010001u
#define PEER_CSID  0x00010002u
#define OWN_CSID   0x00010003u
#define EPOCH      0x0000000eu

struct sent_frame {
	uint8_t  bytes[VMS_CM_FRAME_LEN];
	uint32_t len;
	uint32_t dst;
	int      was_response;
};

#define MAX_SENT 64

struct bed {
	struct vms_cluster                 cl;
	struct cnxman_ops                  ops;
	struct fake_cnx                    fake;
	struct cnxman_barrier_link_ops     link_ops;
	struct cnxman_barrier              b;

	struct sent_frame sent[MAX_SENT];
	uint32_t          n_sent;
	uint32_t          n_out_refused;   /* next_out told the FSM "no link"  */
	int               link_down;       /* make next_out fail               */
	uint16_t          next_token;      /* the CM's own continuous counter  */
	uint16_t          next_send_msg;
};

static struct bed g;

/*
 * The connection manager's outbound link. This is the ONLY source of the
 * envelope counters, and in particular of the correlation token at body[6:8]:
 * a prior implementation used the barrier step ordinal there, collided with its
 * own step-1 value, and the coordinator dropped the frame. The counter here is
 * deliberately UNRELATED to the step number so the test can prove the FSM never
 * substitutes one.
 */
static int bed_next_out(void *ctx, vms_csid_t dst, struct vms_cm_link *l,
			struct vms_cm_envelope *env)
{
	struct bed *bp = (struct bed *)ctx;
	static const uint8_t dmac[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
	static const uint8_t smac[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

	(void)dst;
	if (bp->link_down) {
		bp->n_out_refused++;
		return -1;
	}
	memcpy(l->hdr.eth_dst, dmac, 6);
	memcpy(l->hdr.eth_src, smac, 6);
	memcpy(l->hdr.dst_lavc, dmac, 6);
	memcpy(l->hdr.src_lavc, smac, 6);
	l->hdr.connect_flag = 0x0001;
	l->recv_ack = 0x0007;
	l->send_seq = 0x0009;
	l->remote_conid = 0x62c50009u;
	l->local_conid = 0x33580008u;

	env->send_msg = bp->next_send_msg++;
	env->ack_msg = 0x0100;
	env->txn = 0x0009;
	env->token = bp->next_token++;
	return 0;
}

static void bed_record(struct bed *bp, const uint8_t *body, uint32_t len,
		       uint32_t dst, int was_response)
{
	if (bp->n_sent >= MAX_SENT)
		return;
	if (len > VMS_CM_FRAME_LEN)
		len = VMS_CM_FRAME_LEN;
	memcpy(bp->sent[bp->n_sent].bytes, body, len);
	bp->sent[bp->n_sent].len = len;
	bp->sent[bp->n_sent].dst = dst;
	bp->sent[bp->n_sent].was_response = was_response;
	bp->n_sent++;
}

/*
 * `ctx` here is fake_ops_init's own struct fake_cnx (the injected clock and
 * console), not the bed -- so the recorder reaches the single test bed
 * directly. Keeping one ctx per ops vtable is what the FC-P3.6 fake already
 * established; this file only adds the two members it left NULL.
 */
static int bed_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	bed_record(&g, body, len, (uint32_t)dst, 0);
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	bed_record(&g, body, len, 0u, 1);
	return 0;
}

static void bed_init(void)
{
	struct vms_csb *local, *coord, *peer;

	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;
	g.fake.now_ms = 100000u;
	g.next_token = 0x07f5u;    /* nothing like a step ordinal */
	g.next_send_msg = 0x0140u;

	memcpy(g.cl.params.scsnode, "OVMXJ0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = 0x000004000103ull;
	g.cl.params.vaxcluster = 2;

	local = cnxman_club_init(&g.cl);
	cnxman_club_learn_local_csid(&g.cl.club, OWN_CSID);
	(void)local;

	coord = cnxman_club_alloc_csb(&g.cl.club, 0x000004000101ull, 1);
	cnxman_csb_set_scsnode(coord, (const uint8_t *)"VAX1", 4);
	cnxman_csb_set_csid(coord, COORD_CSID);

	peer = cnxman_club_alloc_csb(&g.cl.club, 0x000004000102ull, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"VAX2", 4);
	cnxman_csb_set_csid(peer, PEER_CSID);

	g.link_ops.next_out = bed_next_out;
	g.link_ops.ctx = &g;
	cnxman_barrier_init(&g.b, &g.cl, &g.ops, &g.link_ops);
}

/* ==========================================================================
 * Building the COORDINATOR's frames
 *
 * Through the codec's own named offsets and put primitives -- no magic numbers
 * in a test either (design sec 3.9 rule 2).
 * ========================================================================== */

static uint32_t mk_frame(uint8_t *f, uint8_t cat, uint8_t op)
{
	struct vms_cm_link l;
	vms_wire_buf_t w;
	uint32_t written = 0;
	static const uint8_t dmac[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
	static const uint8_t smac[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };

	memset(f, 0, VMS_CM_FRAME_LEN);
	memset(&l, 0, sizeof(l));
	memcpy(l.hdr.eth_dst, dmac, 6);
	memcpy(l.hdr.eth_src, smac, 6);
	memcpy(l.hdr.dst_lavc, dmac, 6);
	memcpy(l.hdr.src_lavc, smac, 6);
	l.hdr.connect_flag = 0x0001;
	l.recv_ack = 0x0011;
	l.send_seq = 0x0012;
	l.remote_conid = 0x33580008u;
	l.local_conid = 0x62c50009u;
	(void)vms_cm_link_build(&l, f, VMS_CM_FRAME_LEN, &written);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_SEND_MSG, 0x0220);
	vms_wire_put_le16(&w, VMS_OFF_CM_ACK_MSG, 0x0140);
	vms_wire_put_le16(&w, VMS_OFF_CM_TXN, 0x0009);
	vms_wire_put_le16(&w, VMS_OFF_CM_TOKEN, 0x0abc);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY, cat);
	vms_wire_put_u8(&w, VMS_OFF_CM_OPCODE, op);
	return VMS_CM_FRAME_LEN;
}

/* op 0x09: the class-0x02 ADD open. tag 0x0240 = (class << 8) | role. */
static uint32_t mk_open_add(uint8_t *f, uint32_t epoch, uint8_t bitmap)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_ADD);
	vms_wire_put_u8(&w, VMS_OFF_CM_BITMAP, bitmap);
	return n;
}

/* op 0x08: the class-0x03 REMOVE open. Carries NO bitmap (spec sec 4(p)). */
static uint32_t mk_open_remove(uint8_t *f, uint32_t epoch)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_REM);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_REMOVE);
	return n;
}

/* cat-0x01 op 0x0d: the class-0x04 self-departure open (NOT the cat-0x02
 * op-0x0d DLM rebuild record -- different category, different message). */
static uint32_t mk_open_depart(uint8_t *f, uint32_t epoch)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_DEPART_XITION);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_DEPART);
	return n;
}

/* op 0x0a: the GO. tag = (class << 8) | 0x60. */
static uint32_t mk_go(uint8_t *f, uint32_t epoch, uint8_t cls, uint8_t role)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, role);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, cls);
	return n;
}

/* op 0x0c: the release of step N. body[16:20] is a plain LE u32 index here. */
static uint32_t mk_release(uint8_t *f, uint32_t epoch, uint32_t step)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_le32(&w, VMS_OFF_CM_STEP, step);
	return n;
}

/* 0x81/0x0b: the coordinator's acknowledgement of our step -- NOT the release. */
static uint32_t mk_step_ack(uint8_t *f, uint32_t epoch, uint32_t step)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY,
			vms_wire_response_category(VMS_CM_CAT_CONFIG));
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_le32(&w, VMS_OFF_CM_STEP, step);
	return n;
}

/* cat-0x02 op-0x0d: one lock-resource rebuild record, interleaved with the
 * barrier and gating the next step. */
static uint32_t mk_rebuild(uint8_t *f, const char *resname)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_DLM, VMS_CM_OP_DLM_REBUILD);
	uint8_t len = (uint8_t)strlen(resname);
	uint32_t i;

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_DLM_L1_TAG, 0x0001);
	vms_wire_put_le16(&w, VMS_OFF_CM_DLM_L1_TAG2, 0x0003);
	vms_wire_put_u8(&w, VMS_OFF_CM_DLM_L1_LEN, 0x20);
	vms_wire_put_u8(&w, VMS_OFF_CM_DLM_RESULT, 0xf9);
	vms_wire_put_u8(&w, VMS_OFF_CM_DLM_RESNAMLEN, len);
	for (i = 0; i < len; i++)
		vms_wire_put_u8(&w, VMS_OFF_CM_DLM_RESNAME + i,
				(uint8_t)resname[i]);
	return n;
}

/* cat-0x01 op-0x04, role 0x50: the coordinator aborting the transition. */
static uint32_t mk_abort(uint8_t *f)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_ABORT);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_ABORT);
	return n;
}

static enum cnxman_barrier_rx feed(const uint8_t *f, uint32_t len)
{
	return cnxman_barrier_rx_frame(&g.b, f, len, COORD_CSID, 1);
}

/* ==========================================================================
 * Reading what went out
 * ========================================================================== */

static uint8_t sent_cat(uint32_t i) { return g.sent[i].bytes[VMS_OFF_CM_CATEGORY]; }
static uint8_t sent_op(uint32_t i)  { return g.sent[i].bytes[VMS_OFF_CM_OPCODE]; }

static uint32_t sent_le32(uint32_t i, uint32_t off)
{
	const uint8_t *p = &g.sent[i].bytes[off];

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t sent_le16(uint32_t i, uint32_t off)
{
	const uint8_t *p = &g.sent[i].bytes[off];

	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* How many frames carrying (cat, op) went out at all. */
static uint32_t count_sent(uint8_t cat, uint8_t op)
{
	uint32_t i, n = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (sent_cat(i) == cat && sent_op(i) == op)
			n++;
	}
	return n;
}

/* ==========================================================================
 * 1. PHASE 1 -- the open is recorded and ACKNOWLEDGED (p. 7-41)
 * ========================================================================== */
static void test_open_is_acknowledged(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] the transition open is Phase 1, and it is answered "
	       "(p. 7-41, spec sec 4(p))\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));

	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_OPEN,
		 "an op-0x09 open moves the participant to OPEN");
	ct_check_eq_u32(g.b.epoch, EPOCH, "the epoch comes off body[12:16]");
	ct_check_eq_u32(g.b.tr_class, VMS_CM_CLASS_ADD,
			"body[17] is the transition CLASS (spec sec 4(r))");
	ct_check_eq_u32(g.b.coordinator_csid, COORD_CSID,
			"the sender is recorded as the coordinator");
	ct_check_eq_u32(g.n_sent, 1, "exactly one frame went out");
	ct_check(g.sent[0].was_response,
		 "and it went out through the CORRELATED response path");
	ct_check_eq_u32(sent_cat(0), 0x81,
			"the acknowledgement is the 0x81 echo (body[8] |= 0x80)");
	ct_check_eq_u32(sent_op(0), VMS_CM_OP_XITION_ADD,
			"echoing the request's opcode");
	ct_check_eq_u32(g.sent[0].bytes[VMS_OFF_CM_RESP_MARK], 0x01,
			"mutation 2: body[18] = 0x01");
	ct_check_eq_u32(g.sent[0].bytes[VMS_OFF_CM_BITMAP], 0x00,
			"mutation 3: body[55] cleared -- the responder REFUSES "
			"to assert the membership bitmap (spec sec 4(p))");
	ct_check_eq_u32(sent_le16(0, VMS_OFF_CM_TXN), 0x0009,
			"the request's txn is echoed, not invented");
	ct_check_eq_u32(sent_le16(0, VMS_OFF_CM_TOKEN), 0x0abc,
			"and so is the correlation token whose derivation is "
			"UNKNOWN (spec sec 4(j))");
}

/* Phase 1 is NOT the commit: nothing about membership has moved yet. */
static void test_open_does_not_commit(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] Phase 1 is a PROPOSAL: nothing commits before the GO "
	       "(p. 7-41/7-42)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));

	ct_check(!cnxman_barrier_phase2_committed(&g.b),
		 "phase2_committed is false while the proposal is outstanding");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 0,
			"the member count has not been written");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER,
		 "and this node is not yet a member");
	ct_check_eq_u32(g.b.steps_sent, 0, "no barrier step has gone out");
}

/* ==========================================================================
 * 2. PHASE 2 -- the count commits at the GO, BEFORE the rebuild (p. 7-42)
 *
 * This is the case the whole item hangs on. The book puts "the total number of
 * members ... is stored in the CLUB" and "the MEMBER flag is set" among the
 * Phase 2 tasks, and only THEN says "the particular type of lock management
 * database rebuild required by this state transition is now performed", with
 * each system waiting until all the others are ready. So the count cannot be
 * gated on the rebuild -- and here it is not: it is committed with zero barrier
 * steps released and zero rebuild records answered.
 * ========================================================================== */
static void test_count_commits_in_phase2_before_the_rebuild(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] THE COUNT COMMITS IN PHASE 2, BEFORE THE DLM REBUILD "
	       "(p. 7-42)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));   /* slots 1,2,3 -> M=3 */
	g.n_sent = 0;
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check(cnxman_barrier_phase2_committed(&g.b),
		 "the GO commits Phase 2 -- the point of no return (p. 7-42)");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 3,
			"the member count is stored NOW: 3 CSBs with SELECTED "
			"set, from the coordinator's nodemap (p. 7-42/7-49)");
	ct_check(g.cl.state == VMS_CLUSTER_MEMBER,
		 "and this node's own CSB carries MEMBER, so it IS a member");

	ct_check_eq_u32(g.b.releases, 0,
			"...with ZERO barrier releases consumed");
	ct_check_eq_u32(g.b.rebuild_records, 0,
			"...and ZERO lock-rebuild records answered: the count "
			"is NOT DLM-completion-gated");
	ct_check_eq_u32(g.b.steps_sent, 1,
			"the barrier then starts at step 1 (spec sec 4(p))");
	ct_check_eq_u32(g.b.step, 1, "and step 1 is the step in flight");
}

/* The nodemap really is what selected those three: a bitmap naming only our own
 * slot leaves a one-member count and clears the others. */
static void test_nodemap_drives_selection(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	struct vms_csb *coord;

	printf("[barrier] the committed count comes from the coordinator's "
	       "nodemap, not from a constant\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x08u));  /* slot 3 only = us */
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check_eq_u32(g.cl.club.cluster_nodes, 1,
			"popcount 1 -> one selected CSB");
	coord = cnxman_club_find_csid(&g.cl.club, COORD_CSID);
	ct_check(coord != NULL && !cnxman_csb_is_member(coord),
		 "a system absent from the nodemap is not a member");
}

/* ==========================================================================
 * 3. THE 12 STEPS -- a driven, complete lockstep run
 * ========================================================================== */
static void test_twelve_step_sequence(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n;

	printf("[barrier] a driven 12 x (0x0b -> 0x81/0x0b -> 0x0c) sequence "
	       "completes (spec sec 4(p), 30 of 30 captures)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	for (n = 1; n <= CNXMAN_BARRIER_STEPS; n++) {
		char what[128];

		snprintf(what, sizeof(what),
			 "  step %u: our op-0x0b carries step %u", n, n);
		ct_check(g.b.steps_sent == n &&
			 sent_le32(g.n_sent - 1, VMS_OFF_CM_STEP) == n, what);

		/* The ack is NOT the release: it must not advance anything. */
		(void)feed(f, mk_step_ack(f, EPOCH, n));
		snprintf(what, sizeof(what),
			 "  step %u: the 0x81/0x0b ack does NOT advance", n);
		ct_check(g.b.steps_sent == n && g.b.step == n, what);

		(void)feed(f, mk_release(f, EPOCH, n));
	}

	ct_check_eq_u32(g.b.steps_sent, CNXMAN_BARRIER_STEPS,
			"exactly 12 op-0x0b requests went out -- never a 13th");
	ct_check_eq_u32(g.b.releases, CNXMAN_BARRIER_STEPS,
			"exactly 12 releases were consumed");
	ct_check_eq_u32(g.b.step_acks, CNXMAN_BARRIER_STEPS,
			"and 12 acks were seen and distinguished from them");
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_COMPLETE,
		 "release #12 completes the transition (spec sec 4(p): 12 is "
		 "the only termination signal)");
	ct_check_eq_u32(g.b.transitions_completed, 1, "counted once");
	ct_check_eq_u32(g.cl.club.transition_active, 0,
			"and the CLUB no longer shows a transition in progress");
	ct_check_eq_u32(g.b.step_mismatch, 0, "no step ran out of order");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER), 12,
			"12 op-0x0b frames total on the wire");
}

/*
 * Every originated step carries the CM's own token, never the step ordinal.
 * The regression this pins: a step whose token equalled its index collided with
 * itself, the coordinator dropped the frame, its recv_ack froze, and the
 * barrier stalled and then regressed.
 */
static void test_step_token_is_the_cms_counter(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i, step_no = 0, bad = 0;

	printf("[barrier] a barrier step's token is the connection manager's "
	       "own counter, NEVER the step ordinal\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
		(void)feed(f, mk_release(f, EPOCH, i));

	for (i = 0; i < g.n_sent; i++) {
		if (sent_cat(i) != VMS_CM_CAT_CONFIG ||
		    sent_op(i) != VMS_CM_OP_BARRIER)
			continue;
		step_no++;
		if (sent_le16(i, VMS_OFF_CM_TOKEN) == (uint16_t)step_no)
			bad++;
	}
	ct_check_eq_u32(step_no, 12, "12 steps inspected");
	ct_check_eq_u32(bad, 0, "not one token equals its step index");
	/* sent[0] is the open's acknowledgement, which took 0x07f5; the steps
	 * then take 0x07f6..0x0801 -- ONE unbroken per-node counter across the
	 * acknowledgement and every step, exactly the shape the reference
	 * joiner shows (07f5, f6, f7 ... 0801 at step 12). */
	ct_check_eq_u32(sent_le16(1, VMS_OFF_CM_TOKEN), 0x07f6,
			"step 1 carries the CM's next token after the open ack");
	ct_check_eq_u32(sent_le16(2, VMS_OFF_CM_TOKEN), 0x07f7,
			"and the counter runs on, unbroken, across the steps");
	ct_check_eq_u32(sent_le16(12, VMS_OFF_CM_TOKEN), 0x0801,
			"step 12 lands on 0x0801, twelve past the open");
}

/* The step's other two asserted fields trace to real state as well. */
static void test_step_fields_trace_to_real_state(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] every field of an originated op-0x0b traces to real "
	       "state (INV-6)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check_eq_u32(sent_le32(1, VMS_OFF_CM_EPOCH), EPOCH,
			"the epoch is the one the coordinator's open carried");
	ct_check_eq_u32(sent_le32(1, VMS_OFF_CM_STEP), 1,
			"the step is the FSM's own counter");
	ct_check_eq_u32(g.sent[1].dst, COORD_CSID,
			"and it is addressed to the coordinator we recorded");
	ct_check(!g.sent[1].was_response,
		 "a step is an ORIGINATION, not a response");
}

/* ==========================================================================
 * 4. THE NEGATIVE CASES -- what a participant must NEVER send
 * ========================================================================== */
static void test_go_and_release_are_never_answered(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i, answered = 0;

	printf("[barrier] the GO and every RELEASE are NEVER answered "
	       "(spec sec 4(p)/(q): no 0x8a or 0x8c exists in any capture)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	g.n_sent = 0;
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
		(void)feed(f, mk_release(f, EPOCH, i));

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].was_response)
			answered++;
	}
	ct_check_eq_u32(answered, 0,
			"not one response was emitted for a notification");
	ct_check_eq_u32(count_sent(0x81, VMS_CM_OP_XITION_GO), 0,
			"no 0x8a was built");
	ct_check_eq_u32(count_sent(0x81, VMS_CM_OP_BARRIER_REL), 0,
			"no 0x8c was built");
	ct_check(g.b.silences >= 13,
		 "and each one is COUNTED as a deliberate silence, not lost");
}

/*
 * "An op 0x0a whose body[16:18] is not 0x0260 (e.g. 0x0460, seen on a running
 * cluster) is NOT a barrier start" (spec sec 4(p)). The role byte is the
 * discriminator; a wrong one must start nothing.
 */
static void test_wrong_go_role_starts_nothing(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] an op-0x0a with the wrong role tag is not a barrier "
	       "start (spec sec 4(p))\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	g.n_sent = 0;
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_RELAY));

	ct_check(!cnxman_barrier_phase2_committed(&g.b),
		 "nothing committed");
	ct_check_eq_u32(g.b.steps_sent, 0, "no barrier step went out");
	ct_check_eq_u32(g.n_sent, 0, "nothing at all went out");
	ct_check_eq_u32(g.b.ignored_events, 1, "and it was counted, not lost");
}

/* Frames another CM FSM owns are routed on, never answered here. */
static void test_foreign_frames_are_routed_on(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] frames belonging to the join / coordinator FSMs are "
	       "routed on, not answered\n");
	bed_init();
	ct_check(feed(f, mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT)) ==
		 CNXMAN_BARRIER_RX_NOT_MINE, "cat-0x01 op-0x03 is FC-P3.3's");
	ct_check(feed(f, mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY)) ==
		 CNXMAN_BARRIER_RX_NOT_MINE, "cat-0x01 op-0x12 is the relay");
	ct_check(feed(f, mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER)) ==
		 CNXMAN_BARRIER_RX_NOT_MINE,
		 "an INBOUND op-0x0b is a member reporting to a COORDINATOR");
	ct_check(feed(f, mk_frame(f, VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE)) ==
		 CNXMAN_BARRIER_RX_NOT_MINE, "cat-0x06 is the member poll");
	ct_check_eq_u32(g.n_sent, 0, "and none of them drew a response here");
}

/* ==========================================================================
 * 5. THE INTERLEAVED REBUILD RECORDS -- op-0d
 * ========================================================================== */
static void test_rebuild_records_are_answered_mid_barrier(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i;

	printf("[barrier] cat-0x02 op-0x0d records are answered mid-barrier "
	       "(spec sec 4(p): five unanswered ones froze step 5)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	for (i = 1; i <= 5; i++)
		(void)feed(f, mk_release(f, EPOCH, i));
	ct_check_eq_u32(g.b.step, 6, "we are at step 6");

	g.n_sent = 0;
	(void)feed(f, mk_rebuild(f, "F11B$aSYSDSK1     "));
	(void)feed(f, mk_rebuild(f, "CACHE$cmSYSDSK1     "));

	ct_check_eq_u32(g.b.rebuild_records, 2, "both records were seen");
	ct_check_eq_u32(g.b.rebuild_echoed, 2,
			"and both were answered with the grounded echo");
	ct_check_eq_u32(g.n_sent, 2, "two responses went out");
	ct_check_eq_u32(sent_cat(0), 0x82, "cat 0x02 -> 0x82");
	ct_check_eq_u32(g.sent[0].bytes[VMS_OFF_CM_DLM_RESULT], 0xf9,
			"body[34] = 0xf9, the MANDATORY stamp");
	ct_check_eq_u32(g.b.step, 6, "a rebuild record does NOT advance the "
			"barrier");

	/* THE LOCKMGRERR REGRESSION. body[55] is the 8th byte of the resource
	 * name here; a prior build applied the cat-0x01 mutation and shipped
	 * "CACHE$c\0SYSDSK1", and VAX1 and VAX3 took a fatal LOCKMGRERR. */
	/* body[55] is the EIGHTH byte of the resource name here -- index 7,
	 * the 'm' of "CACHE$cm..." -- and a prior build applied the cat-0x01
	 * clearing mutation there, shipped "CACHE$c\0SYSDSK1", and VAX1 and
	 * VAX3 took a fatal LOCKMGRERR. */
	ct_check_eq_u32(g.sent[1].bytes[VMS_OFF_CM_BITMAP], 'm',
			"body[55] IS the 8th resource-name byte and survives -- "
			"the cat-0x01 mutation is NOT applied (LOCKMGRERR)");
	ct_check_eq_u32(g.sent[1].bytes[VMS_OFF_CM_RESP_MARK], 0x00,
			"and body[18], inside the L1 region here, is echoed "
			"(0) not forced to 1");
	for (i = 0; i < strlen("CACHE$cmSYSDSK1     "); i++) {
		if (g.sent[1].bytes[VMS_OFF_CM_DLM_RESNAME + i] !=
		    (uint8_t)"CACHE$cmSYSDSK1     "[i])
			break;
	}
	ct_check(i == strlen("CACHE$cmSYSDSK1     "),
		 "the whole resource name survives the echo byte for byte");
}

/* A record arriving before the barrier and after it are both answered: a
 * lock-less member answered 216 of them around a real join. */
static void test_rebuild_records_outside_the_barrier(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i;

	printf("[barrier] rebuild records are answered before and after the "
	       "barrier too (spec sec 4(q))\n");
	bed_init();
	(void)feed(f, mk_rebuild(f, "SYS$SYS_ID"));
	ct_check_eq_u32(g.b.rebuild_echoed, 1, "answered while IDLE");

	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
		(void)feed(f, mk_release(f, EPOCH, i));
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_COMPLETE, "transition done");

	(void)feed(f, mk_rebuild(f, "VCC$vSYSDSK1     "));
	ct_check_eq_u32(g.b.rebuild_echoed, 2, "and answered after it too");
}

/* ==========================================================================
 * 6. THE DLM SEAM -- a real lock manager gets first refusal (P5)
 * ========================================================================== */
static int g_dlm_begin, g_dlm_end, g_dlm_end_completed, g_dlm_calls;
static int g_dlm_reply_len;   /* what the fake DLM decides to write */
static int g_dlm_decline;

static void fake_dlm_begin(void *ctx, const struct cnxman_transition *tr)
{
	(void)ctx; (void)tr;
	g_dlm_begin++;
}

static void fake_dlm_end(void *ctx, const struct cnxman_transition *tr,
			 int completed)
{
	(void)ctx; (void)tr;
	g_dlm_end++;
	g_dlm_end_completed = completed;
}

static int fake_dlm_request(void *ctx, const struct dlm_scs_request *req,
			    struct dlm_scs_reply *reply)
{
	(void)ctx;
	g_dlm_calls++;
	if (g_dlm_decline)
		return 1;                    /* an honest decline */
	if (g_dlm_reply_len > 0) {
		/* A real DLM fills this from a real LKB/RSB; here it is enough
		 * that the bytes came from the caller's own buffer and go back
		 * through the CORRELATED response path (RULE A). */
		memcpy(reply->body, req->body + VMS_OFF_SYSAP_BODY,
		       VMS_CM_BODY_LEN);
		reply->len = VMS_CM_BODY_LEN;
	}
	return 0;
}

static const struct dlm_scs_role_ops g_fake_dlm = {
	fake_dlm_begin, fake_dlm_request, fake_dlm_end, NULL, NULL
};

static void test_dlm_seam(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] the lock manager gets first refusal on every rebuild "
	       "record, and the transition callbacks fire\n");
	bed_init();
	g_dlm_begin = g_dlm_end = g_dlm_calls = 0;
	g_dlm_reply_len = 1;
	g_dlm_decline = 0;
	cnxman_barrier_set_dlm(&g.b, &g_fake_dlm);

	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	ct_check_eq_u32((uint32_t)g_dlm_begin, 1,
			"transition_begin fires at the open (freeze before the "
			"barrier)");

	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	(void)feed(f, mk_rebuild(f, "F11B$aSYSDSK1     "));
	ct_check_eq_u32((uint32_t)g_dlm_calls, 1, "the record reached the DLM");
	ct_check_eq_u32(g.b.rebuild_dlm, 1,
			"and the DLM's own reply is what went on the wire");
	ct_check_eq_u32(g.b.rebuild_echoed, 0,
			"the P3 echo did NOT also fire");

	/* A DLM that declines gets silence -- never an echo answering for it. */
	g_dlm_decline = 1;
	g.n_sent = 0;
	(void)feed(f, mk_rebuild(f, "SYS$SYS_ID"));
	ct_check_eq_u32(g.b.rebuild_declined, 1, "a decline is counted");
	ct_check_eq_u32(g.n_sent, 0, "and answered with silence, not an echo");

	g_dlm_decline = 0;
	{
		uint32_t i;

		for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
			(void)feed(f, mk_release(f, EPOCH, i));
	}
	ct_check_eq_u32((uint32_t)g_dlm_end, 1, "transition_end fires once");
	ct_check_eq_u32((uint32_t)g_dlm_end_completed, 1,
			"with completed = 1 after release #12");
}

/* ==========================================================================
 * 7. CLASS PARTICIPATION -- ADD / REMOVE / DEPART
 * ========================================================================== */
static void test_class_remove_runs_the_same_barrier(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i;

	printf("[barrier] a class-0x03 REMOVE runs the SAME 12 steps and can "
	       "start with a bare GO (spec sec 4(p)/(r))\n");
	bed_init();
	/* Spec sec 4(p): "a class-0x03 removal has no op 0x09 at all -- it
	 * starts directly at op 0x0a / tag 0x0360". */
	(void)feed(f, mk_go(f, 0x11u, VMS_CM_CLASS_REMOVE, VMS_CM_ROLE_GO));

	ct_check(cnxman_barrier_phase2_committed(&g.b),
		 "a bare GO commits Phase 2");
	ct_check_eq_u32(g.b.tr_class, VMS_CM_CLASS_REMOVE, "class 0x03");
	ct_check_eq_u32(g.b.bitmap_valid, 0,
			"and it carries NO nodemap -- none is invented");
	ct_check_eq_u32(g.b.steps_sent, 1, "the barrier starts at step 1");

	for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
		(void)feed(f, mk_release(f, 0x11u, i));
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_COMPLETE,
		 "and it too tops out at exactly 12");
	ct_check_eq_u32(g.b.steps_sent, 12, "12 steps, same law");
}

static void test_class_remove_open_is_answered(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] the class-0x03 op-0x08 open is acknowledged and "
	       "carries no bitmap\n");
	bed_init();
	(void)feed(f, mk_open_remove(f, 0x12u));
	ct_check_eq_u32(g.n_sent, 1, "it is answered");
	ct_check_eq_u32(sent_op(0), VMS_CM_OP_XITION_REM, "with the 0x81 echo");
	ct_check_eq_u32(g.b.bitmap_valid, 0, "no nodemap was read from it");
	ct_check_eq_u32(g.b.tr_class, VMS_CM_CLASS_REMOVE, "class recorded");
}

static void test_class_depart_starts_no_barrier(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] a class-0x04 self-departure starts NO barrier at all "
	       "(spec sec 4(r))\n");
	bed_init();
	(void)feed(f, mk_open_depart(f, 0x13u));
	ct_check_eq_u32(g.b.tr_class, VMS_CM_CLASS_DEPART, "class 0x04");
	ct_check_eq_u32(g.n_sent, 1, "the open is still acknowledged");

	g.n_sent = 0;
	(void)feed(f, mk_go(f, 0x13u, VMS_CM_CLASS_DEPART, VMS_CM_ROLE_GO));
	ct_check_eq_u32(g.b.steps_sent, 0, "no op-0x0b is ever sent");
	ct_check_eq_u32(g.n_sent, 0, "nothing goes out at the GO");
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_COMPLETE,
		 "the transition is over at the GO -- '0x12 -> 0x03 -> 0x0d -> "
		 "0x0a and then nothing'");
	ct_check(cnxman_barrier_phase2_committed(&g.b),
		 "and Phase 2 still commits: a departure changes membership");
}

/* The class-0x03 extra step, op-0x0f: answered, and it moves nothing. */
static void test_op0f_extra_step(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] op-0x0f, the class-0x03 extra step, is echoed and "
	       "changes no state (spec sec 4(r))\n");
	bed_init();
	(void)feed(f, mk_open_remove(f, 0x12u));
	g.n_sent = 0;
	ct_check(feed(f, mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_0F)) ==
		 CNXMAN_BARRIER_RX_CONSUMED, "it is ours");
	ct_check_eq_u32(g.b.aux_echoes, 1, "and it is answered");
	ct_check_eq_u32(sent_op(0), VMS_CM_OP_0F, "with its own opcode");
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_OPEN,
		 "the state is untouched");
	ct_check_eq_u32(g.b.steps_sent, 0, "and no step was provoked");
}

/* ==========================================================================
 * 8. BITMAP WIDTH -- the edge cases the strawman got wrong
 *
 * Spec sec 4(p): popcount == the post-transition member count (54/54); bit 0 is
 * never set; body[52:55] and body[56:60] are all-zero in every specimen so the
 * field is CERTAINLY wider than a byte, extent and endianness UNDETERMINED;
 * "do not assume 8 slots"; and 12 is grounded only to M=4.
 * ========================================================================== */
static void test_bitmap_popcount_and_slots(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] the bitmap's popcount is the member count, and the "
	       "slots seen are recorded (spec sec 4(p))\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));   /* bits 1,2,3 */
	ct_check_eq_u32(g.b.bitmap_popcount, 3, "popcount(0x0e) == 3 == M");
	ct_check_eq_u32(g.b.max_slot_seen, 4, "the highest slot named is 3");
	ct_check_eq_u32(g.cl.club.bitmap[0], 0x0eu,
			"and the CLUB holds the nodemap Phase 1 delivered "
			"(p. 7-41)");
	ct_check_eq_u32(g.cl.club.bitmap_slots_seen, 8,
			"recording ONLY the 8 slots the wire has spoken about");

	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x1eu));   /* the M=4 library max */
	ct_check_eq_u32(g.b.bitmap_popcount, 4, "popcount(0x1e) == 4");
	ct_check_eq_u32(g.b.m_above_grounded, 0,
			"M=4 is the largest GROUNDED cluster: not flagged");
}

static void test_bitmap_bit0_is_instrumented(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] bit 0 set is recorded, not silently counted as a "
	       "member (CSV slot 0 is never used, p. 7-25)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0fu));
	ct_check_eq_u32(g.b.bitmap_bit0, 1, "the impossible bit is counted");
}

static void test_bitmap_span_residual_is_instrumented(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	vms_wire_buf_t w;

	printf("[barrier] a nonzero byte outside body[55] is the observation "
	       "that widens the field -- counted, never decoded (spec sec 4(p))\n");
	bed_init();
	(void)mk_open_add(f, EPOCH, 0x0eu);
	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_u8(&w, VMS_OFF_CM_BITMAP_SPAN + 4, 0x01);  /* body[56] */
	(void)feed(f, VMS_CM_FRAME_LEN);

	ct_check_eq_u32(g.b.bitmap_span_residual, 1,
			"the residual is recorded");
	ct_check_eq_u32(g.b.bitmap_popcount, 3,
			"and the grounded byte is still read as three members "
			"-- no guessed wider encoding");
}

static void test_slot_beyond_the_byte_is_not_a_removal(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	struct vms_csb *far;

	printf("[barrier] a member in a CSV slot past the grounded byte is "
	       "left alone, not un-selected (the 8-slot assumption)\n");
	bed_init();
	far = cnxman_club_alloc_csb(&g.cl.club, 0x000004000109ull, 1);
	cnxman_csb_set_scsnode(far, (const uint8_t *)"VAX9", 4);
	cnxman_csb_set_csid(far, 0x00010009u);          /* CSV slot 9 */
	cnxman_csb_set_flags(far, (uint16_t)(VMS_CSB_F_MEMBER |
					     VMS_CSB_F_SELECTED));
	{
		struct vms_csb *far2 =
			cnxman_club_alloc_csb(&g.cl.club, 0x00000400010aull, 1);

		cnxman_csb_set_scsnode(far2, (const uint8_t *)"VAXA", 4);
		cnxman_csb_set_csid(far2, 0x0001000au);  /* CSV slot 10 */
		cnxman_csb_set_flags(far2, (uint16_t)(VMS_CSB_F_MEMBER |
						      VMS_CSB_F_SELECTED));
	}

	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check(cnxman_csb_is_member(far),
		 "the slot-9 member keeps its membership: our byte cannot "
		 "speak about slot 9, and 'not in the map I can read' is not "
		 "'removed'");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 5,
			"so the committed count is 5, not the byte's 3");
	ct_check_eq_u32(g.b.count_mismatch, 1,
			"and the disagreement with the nodemap is COUNTED -- "
			"which is exactly the width tell");
	ct_check_eq_u32(g.b.bitmap_short, 1, "flagged as a short bitmap");
	ct_check_eq_u32(g.b.m_above_grounded, 1,
			"and M=4+ is flagged against the 12-step evidence bound");
}

static void test_unmappable_nodemap_leaves_membership_alone(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] a nodemap that matches nothing we know does NOT "
	       "commit a zero-member cluster\n");
	bed_init();
	/* Forget every CSID: nothing in the map can be matched to a CSB. */
	g.cl.club.csb[0].csid_valid = 0u;
	g.cl.club.csb[1].csid_valid = 0u;
	g.cl.club.csb[2].csid_valid = 0u;

	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check_eq_u32(g.b.nodemap_unmapped, 3, "all three bits unmapped");
	ct_check(cnxman_barrier_phase2_committed(&g.b),
		 "Phase 2 still commits -- it cannot be abandoned (p. 7-42)");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 0,
			"and the count honestly reflects what we can account "
			"for, with the mismatch counted");
	ct_check_eq_u32(g.b.count_mismatch, 1, "counted");
}

/* ==========================================================================
 * 9. THE SLOW-STEP RULE, ABORTS AND LOST COORDINATORS
 * ========================================================================== */
static void test_slow_step_is_never_timed_out(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i;

	printf("[barrier] a slow step is instrumented, NEVER timed out "
	       "(spec sec 4(p): the coordinator waits for the slowest member)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	g.n_sent = 0;

	for (i = 0; i < 30; i++) {
		g.fake.now_ms += 1000u;
		cnxman_barrier_timer(&g.b);
	}
	ct_check_eq_u32(g.b.slow_steps, 30, "thirty seconds of waiting, counted");
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_STEP,
		 "the barrier is STILL running after thirty seconds");
	ct_check_eq_u32(g.b.transitions_abandoned, 0, "nothing was abandoned");
	ct_check_eq_u32(g.n_sent, 0, "and not one step was re-sent");

	/* The release still lands and the barrier still completes. */
	for (i = 1; i <= CNXMAN_BARRIER_STEPS; i++)
		(void)feed(f, mk_release(f, EPOCH, i));
	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_COMPLETE,
		 "a late cluster still completes its transition");
}

static void test_abort_does_not_roll_back_a_committed_count(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] an abort after the GO does not un-commit Phase 2 "
	       "(p. 7-42: it 'cannot be abandoned')\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	g.n_sent = 0;
	(void)feed(f, mk_abort(f));

	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_ABANDONED, "abandoned");
	ct_check_eq_u32(g.b.transitions_abandoned, 1, "counted");
	ct_check_eq_u32(g.n_sent, 0,
			"the abort is a NOTIFICATION: never answered");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 3,
			"and the committed member count stands");
}

static void test_release_out_of_order_does_not_advance(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] a release that does not match the step in flight "
	       "advances nothing\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	(void)feed(f, mk_release(f, EPOCH, 7));      /* we are on step 1 */
	ct_check_eq_u32(g.b.step, 1, "still on step 1");
	ct_check_eq_u32(g.b.step_mismatch, 1, "the mismatch is counted");

	(void)feed(f, mk_release(f, EPOCH + 1u, 1)); /* right step, wrong epoch */
	ct_check_eq_u32(g.b.step, 1, "still on step 1");
	ct_check_eq_u32(g.b.step_mismatch, 2, "and counted again");

	(void)feed(f, mk_release(f, EPOCH, 1));
	ct_check_eq_u32(g.b.step, 2, "the RIGHT release advances to step 2");
}

static void test_coordinator_lost_abandons_locally(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] losing the coordinator mid-barrier abandons locally "
	       "without touching membership (p. 7-30/7-42)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
	cnxman_barrier_coordinator_lost(&g.b);

	ct_check(g.b.state == (uint8_t)CNXMAN_BARRIER_ABANDONED, "abandoned");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 3,
			"membership is HELD -- p. 7-30 forbids presuming a "
			"member departed");
	ct_check_eq_u32(g.cl.club.transition_active, 0,
			"but no transition is in progress any more");
}

/* ==========================================================================
 * 10. HONEST FAILURE -- no link, no frame
 * ========================================================================== */
static void test_no_link_originates_nothing(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] with no connection to the coordinator, NOTHING is "
	       "originated -- not a zero-filled frame (INV-6)\n");
	bed_init();
	g.link_down = 1;
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_go(f, EPOCH, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));

	ct_check_eq_u32(g.n_sent, 0, "no frame went out at all");
	ct_check(g.b.send_failures >= 2, "each refusal is counted");
	ct_check_eq_u32(g.b.steps_sent, 0, "no step was recorded as sent");
	ct_check(cnxman_barrier_phase2_committed(&g.b),
		 "yet Phase 2 still committed: it is local bookkeeping, not a "
		 "message");
}

/* ==========================================================================
 * 11. THE TABLE ITSELF
 * ========================================================================== */
static void test_ignored_events_are_counted(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] an event with no cell for the current state is "
	       "ignored and COUNTED, never guessed\n");
	bed_init();
	/* A step ack while IDLE: no cell. */
	(void)feed(f, mk_step_ack(f, EPOCH, 1));
	ct_check_eq_u32(g.b.ignored_events, 1, "counted");
	ct_check_eq_u32(g.n_sent, 0, "and nothing was sent in response");

	/* A release while IDLE has a cell -- it is a real thing that happens
	 * after a transition -- but it advances nothing. */
	(void)feed(f, mk_release(f, EPOCH, 3));
	ct_check_eq_u32(g.b.late_releases, 1, "a late release is recorded");
	ct_check_eq_u32(g.n_sent, 0, "and still never answered");
}

static void test_transition_readback(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	struct cnxman_transition tr;

	printf("[barrier] the transition readback is absent when there is no "
	       "transition, never a zeroed struct\n");
	bed_init();
	ct_check(cnxman_barrier_transition(&g.b, &tr) != 0,
		 "no transition -> a nonzero return, not epoch 0");

	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	ct_check(cnxman_barrier_transition(&g.b, &tr) == 0, "now there is one");
	ct_check_eq_u32(tr.epoch, EPOCH, "carrying the real epoch");
	ct_check_eq_u32(tr.tr_class, VMS_CM_CLASS_ADD, "and the real class");
	ct_check_eq_u32(tr.we_coordinate, 0,
			"this is the PARTICIPANT side: we do not coordinate");
	ct_check_eq_u32(tr.subject_csid_valid, 0,
			"and the subject stays honestly absent (INV-6)");
}

static void test_state_names(void)
{
	printf("[barrier] the states have names for the console\n");
	ct_check(strcmp(cnxman_barrier_state_name(CNXMAN_BARRIER_IDLE),
			"idle") == 0, "idle");
	ct_check(strcmp(cnxman_barrier_state_name(CNXMAN_BARRIER_STEP),
			"step") == 0, "step");
	ct_check(strcmp(cnxman_barrier_state_name(CNXMAN_BARRIER_COMPLETE),
			"complete") == 0, "complete");
}

/* ==========================================================================
 * 12. Two transitions back to back
 * ========================================================================== */
static void test_two_transitions_back_to_back(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t i;

	printf("[barrier] a second transition runs cleanly after the first\n");
	bed_init();
	for (i = 0; i < 2; i++) {
		uint32_t ep = EPOCH + i;
		uint32_t n;

		(void)feed(f, mk_open_add(f, ep, 0x0eu));
		(void)feed(f, mk_go(f, ep, VMS_CM_CLASS_ADD, VMS_CM_ROLE_GO));
		for (n = 1; n <= CNXMAN_BARRIER_STEPS; n++)
			(void)feed(f, mk_release(f, ep, n));
	}
	ct_check_eq_u32(g.b.transitions_completed, 2, "both completed");
	ct_check_eq_u32(g.b.steps_sent, 24, "24 steps, 12 each");
	ct_check_eq_u32(g.b.step_mismatch, 0, "no step confusion across them");
	ct_check_eq_u32(g.cl.club.reformations, 2,
			"and the CLUB counted two transitions");
}

/* A second open before the GO supersedes the first (INFERRED, and instrumented
 * so a capture can correct it). */
static void test_reopen_supersedes_before_the_go(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] a new epoch before the GO supersedes the pending "
	       "proposal (INFERRED, instrumented)\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_open_add(f, EPOCH + 1u, 0x06u));

	ct_check_eq_u32(g.b.epoch, EPOCH + 1u, "the newer epoch stands");
	ct_check_eq_u32(g.b.bitmap_popcount, 2, "with ITS nodemap");
	ct_check_eq_u32(g.b.transitions_superseded, 1, "and it is counted");
	ct_check_eq_u32(g.n_sent, 2, "both proposals were acknowledged");
}

/* A retransmitted open is answered again and changes nothing -- the
 * coordinator retransmits, and an unanswered request strands it. */
static void test_retransmitted_open_is_reanswered(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];

	printf("[barrier] a retransmitted open is answered again and moves "
	       "nothing\n");
	bed_init();
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	(void)feed(f, mk_open_add(f, EPOCH, 0x0eu));
	ct_check_eq_u32(g.n_sent, 2, "answered twice");
	ct_check_eq_u32(g.b.transitions_seen, 1, "but it is ONE transition");
	ct_check_eq_u32(g.b.transitions_superseded, 0, "nothing superseded");
}

int main(void)
{
	test_open_is_acknowledged();
	test_open_does_not_commit();
	test_count_commits_in_phase2_before_the_rebuild();
	test_nodemap_drives_selection();
	test_twelve_step_sequence();
	test_step_token_is_the_cms_counter();
	test_step_fields_trace_to_real_state();
	test_go_and_release_are_never_answered();
	test_wrong_go_role_starts_nothing();
	test_foreign_frames_are_routed_on();
	test_rebuild_records_are_answered_mid_barrier();
	test_rebuild_records_outside_the_barrier();
	test_dlm_seam();
	test_class_remove_runs_the_same_barrier();
	test_class_remove_open_is_answered();
	test_class_depart_starts_no_barrier();
	test_op0f_extra_step();
	test_bitmap_popcount_and_slots();
	test_bitmap_bit0_is_instrumented();
	test_bitmap_span_residual_is_instrumented();
	test_slot_beyond_the_byte_is_not_a_removal();
	test_unmappable_nodemap_leaves_membership_alone();
	test_slow_step_is_never_timed_out();
	test_abort_does_not_roll_back_a_committed_count();
	test_release_out_of_order_does_not_advance();
	test_coordinator_lost_abandons_locally();
	test_no_link_originates_nothing();
	test_ignored_events_are_counted();
	test_transition_readback();
	test_state_names();
	test_two_transitions_back_to_back();
	test_reopen_supersedes_before_the_go();
	test_retransmitted_open_is_reanswered();
	return ct_summary("test_cnxman_barrier");
}
