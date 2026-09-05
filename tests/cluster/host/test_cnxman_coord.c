/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_coord.c - the cluster state transition, COORDINATOR side
 * (FC-P3.12, test-ladder rung R1).
 *
 * WHAT THIS PROVES, AND WHY EACH CASE EXISTS. The coordinator's failure mode is
 * not "we do not coordinate" -- it is "the cluster breaks". Releasing a barrier
 * step before every member has reported it destroys the lock-step the whole
 * lock-database rebuild depends on; never releasing it times the transition out
 * and DROPS HEALTHY MEMBERS. So the two laws get direct cases with real counts:
 *
 *   THE COUNT   #0x0c == 12 x (M-1), with M taken from real SELECTED CSBs.
 *   THE ORDER   0x0c#N never precedes the LAST 0x0b#N.
 *
 * and the INFERRED selection predicate gets cases of its own, including the
 * negative one that matters most: with nobody asking, this node elects itself
 * for nothing and puts no frame on the wire.
 *
 * GROUNDING. Wire: docs/cluster-protocol-spec.md sec 4(o) (the join dialogue),
 * sec 4(p) (the 12-step barrier census over 41 captures, the membership bitmap
 * and its 0x1e = VAX1+VAX2+VAX3+OVMX specimen, the never-answered
 * notifications), sec 4(r) (the role slot and transition class), sec 4(O.31)
 * (the op-0x12 RELAY between op 0x02 and op 0x03, decoded from a real-VAX
 * readmission). Book: *VAXcluster Principles* (Davis 1993) pp. 7-2, 7-25,
 * 7-30, 7-32, 7-37..7-42, 7-46, 7-49 -- host-only transcript, page cites only
 * (Rule 8).
 *
 * The clock, the link counters and the lock manager are all INJECTED, so a
 * whole 4-member transition -- 36 step reports, 36 acknowledgements and 36
 * releases -- runs here in microseconds with no wire, no daemon and no boot.
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
#include "vms_cnxman_phase2.h"
#include "vms_cnxman_coord_fsm.h"
#include "vms_cluster_codec_cm.h"
#include "vms_frame_compose.h"   /* test-only: struct vms_cm_link, RX specimen
				  * assembly (FC-P3.15, design sec 3.2.4) */

/* ==========================================================================
 * The bed: OVMX coordinating, two established members, one joiner
 *
 * The CSIDs are the lab's own first-use slots (book p. 7-25: CSID =
 * (sequence << 16) | CSV index), which is what makes the expected nodemap
 * below the spec's own 0x1e specimen rather than a number this test invented.
 * ========================================================================== */

#define VAX1_CSID  0x00010001u   /* CSV slot 1 */
#define VAX2_CSID  0x00010002u   /* CSV slot 2 */
#define OWN_CSID   0x00010003u   /* CSV slot 3 -- the local node             */
#define JOIN_SLOT  4u            /* what the coordinator must assign next    */
#define JOIN_CSID  0x00010004u
#define START_EPOCH 0x0000000eu

/* Slot indices in the CLUB's CSB table, in allocation order. */
#define CSB_LOCAL 0
#define CSB_VAX1  1
#define CSB_VAX2  2
#define CSB_JOIN  3

struct sent_frame {
	uint8_t  bytes[VMS_CM_FRAME_LEN];
	uint32_t len;
	uint32_t dst;
	int      was_response;
	uint8_t  category;
	uint8_t  opcode;
};

#define MAX_SENT 256

struct bed {
	struct vms_cluster              cl;
	struct cnxman_ops               ops;
	struct fake_cnx                 fake;
	struct cnxman_coord_rebuild_ops rb_ops;
	struct cnxman_coord             c;

	struct sent_frame sent[MAX_SENT];
	uint32_t          n_sent;
	uint32_t          rebuild_outstanding;

	/* The CLUB slot the frame currently being dispatched arrived from --
	 * set by coord_feed() below, read by bed_respond() so its simulated
	 * dialogue-counter advance (see bed_advance_dialogue) touches the
	 * right CSB. -1 outside a dispatch. */
	int32_t           dispatching_from_csb;

	/* the DLM seam */
	uint32_t          dlm_begins;
	uint32_t          dlm_ends;
	int               dlm_last_completed;
	struct cnxman_transition dlm_last_tr;
};

static struct bed g;

/*
 * Body[0:8] is the connection manager's job alone now: cnxman_envelope_stamp()
 * (vms_cnxman_csb.h) reads it straight off the destination CSB's real
 * cm_send_msg/cm_ack_msg/cm_txn/cm_token fields (design sec 3.2.4 ruling E1).
 * This bed simulates the ONE thing FC-P3.8's glue owns and this item does
 * not -- ADVANCING those counters after a real send -- so a test can still
 * prove what it always has: a token that is deliberately UNRELATED to any
 * step or member ordinal (a prior implementation used one, collided with
 * itself, and stalled a real barrier).
 */
static void bed_advance_dialogue(struct vms_csb *csb)
{
	if (csb == NULL)
		return;
	csb->cm_send_msg++;
	/* `cm_token` IS NOT advanced here any more (E85): the executive owns
	 * it, in cnxman_envelope_originate(). See the barrier bed's note for
	 * what the bed-only maintenance cost on the real wire.
	 * cm_ack_msg/cm_txn are likewise not advanced here: ack_msg changes
	 * only on a real receive, and txn is per-dialogue, not per-message. */
}

static uint32_t bed_rebuild_outstanding(void *ctx)
{
	return ((struct bed *)ctx)->rebuild_outstanding;
}

/* CLUB-slot lookup by index, mirroring the FSM's own coord_csb_at() --
 * duplicated here in the test rather than exposed from the FSM, since it is
 * a one-line array bound check and adding a production accessor JUST for a
 * test bed would be scope creep this item does not need. */
static struct vms_csb *coord_csb_at_test(int32_t index)
{
	if (index < 0 || (uint32_t)index >= g.cl.club.n_csb)
		return NULL;
	if (!g.cl.club.csb[index].in_use)
		return NULL;
	return &g.cl.club.csb[index];
}

/* A fixed, arbitrary abs [0,72) span -- NOT asserted by this file (this
 * item's grounded scope is the body, design sec 3.2.4), needed only so
 * bed_record()'s own vms_frame_classify() call below succeeds exactly as it
 * would on a real port-assembled frame. */
static void bed_record_link(struct vms_cm_link *l)
{
	static const uint8_t dmac[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
	static const uint8_t smac[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

	memset(l, 0, sizeof(*l));
	memcpy(l->hdr.eth_dst, dmac, 6);
	memcpy(l->hdr.eth_src, smac, 6);
	memcpy(l->hdr.dst_lavc, dmac, 6);
	memcpy(l->hdr.src_lavc, smac, 6);
	l->hdr.connect_flag = 0x0001;
	l->recv_ack = 0x0007;
	l->send_seq = 0x0009;
	l->remote_conid = 0x62c50009u;
	l->local_conid = 0x33580008u;
}

static void bed_record(struct bed *bp, const uint8_t *body, uint32_t len,
		       uint32_t dst, int was_response)
{
	struct vms_frame_info fi;
	struct vms_cm_envelope env;
	struct sent_frame *s;
	struct vms_cm_link link;
	uint32_t written = 0;

	if (bp->n_sent >= MAX_SENT)
		return;
	(void)len;   /* every real builder emits exactly VMS_CM_BODY_LEN */
	s = &bp->sent[bp->n_sent++];
	/* Recorded as a full, CLASSIFIABLE specimen -- this file's own
	 * bed_record (unlike the barrier bed) needs vms_frame_classify() to
	 * succeed below, so it composes through the test-only frame
	 * composer (design sec 3.2.4) rather than leaving abs [0,72) zero. */
	bed_record_link(&link);
	(void)vms_frame_compose(&link, body, s->bytes,
				(uint32_t)sizeof(s->bytes), &written);
	s->len = written;
	s->dst = dst;
	s->was_response = was_response;
	s->category = 0xffu;
	s->opcode = 0xffu;
	if (vms_frame_classify(s->bytes, s->len, &fi) != VMS_CODEC_OK)
		return;
	if (vms_cm_envelope_parse(s->bytes + VMS_OFF_SYSAP_BODY,
				  s->len - VMS_OFF_SYSAP_BODY,
				  &env) != VMS_CODEC_OK)
		return;
	s->category = env.category;
	s->opcode = env.opcode;
}

static int bed_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	bed_record(&g, body, len, (uint32_t)dst, 0);
	bed_advance_dialogue(cnxman_club_find_csid(&g.cl.club, dst));
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	bed_record(&g, body, len, 0u, 1);
	bed_advance_dialogue(coord_csb_at_test(g.dispatching_from_csb));
	return 0;
}

static void bed_dlm_begin(void *ctx, const struct cnxman_transition *tr)
{
	struct bed *bp = (struct bed *)ctx;

	bp->dlm_begins++;
	bp->dlm_last_tr = *tr;
}

static void bed_dlm_end(void *ctx, const struct cnxman_transition *tr,
			int completed)
{
	struct bed *bp = (struct bed *)ctx;

	bp->dlm_ends++;
	bp->dlm_last_completed = completed;
	bp->dlm_last_tr = *tr;
}

static const struct dlm_scs_role_ops bed_dlm_ops = {
	bed_dlm_begin, NULL, bed_dlm_end, NULL, &g
};

/* Seed a CSB's real SYSAP dialogue counters (design sec 3.2.4 ruling E1):
 * 0x0140/0x0100/0x0009/0x07f5 are simply where this bed's simulated
 * connection to that system starts -- NOT placeholders, the honest initial
 * state of a fresh per-CSB counter (see vms_cluster.h's own comment on the
 * fields), applied uniformly so EVERY destination this coordinator can
 * address -- including the joiner, whose CSB exists before it has a CSID --
 * carries the same real, advancing state cnxman_envelope_stamp() reads. */
static void bed_seed_dialogue(struct vms_csb *csb)
{
	csb->cm_send_msg = 0x0140u;
	csb->cm_ack_msg = 0x0100u;
	csb->cm_txn = 0x0009u;
	csb->cm_token = 0x07f5u;   /* nothing like a step or member ordinal */
}

static struct vms_csb *bed_member(vms_scs_sysid_t sysid, const char *name,
				  vms_csid_t csid)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(&g.cl.club, sysid, 1);

	cnxman_csb_set_scsnode(csb, (const uint8_t *)name, (uint8_t)strlen(name));
	cnxman_csb_set_csid(csb, csid);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER |
					     VMS_CSB_F_STATUS_RCVD));
	bed_seed_dialogue(csb);
	return csb;
}

/* `n_members` = how many established members besides us (0, 1 or 2), plus a
 * joiner CSB that carries NO cluster system id -- because it has none yet, and
 * assigning it one is the thing under test. */
static void bed_init(uint32_t n_members)
{
	struct vms_csb *local, *joiner;

	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;
	g.fake.now_ms = 100000u;
	g.dispatching_from_csb = -1;

	memcpy(g.cl.params.scsnode, "OVMX01", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = 0x000004000103ull;
	g.cl.params.vaxcluster = 2;

	local = cnxman_club_init(&g.cl);
	cnxman_club_learn_local_csid(&g.cl.club, OWN_CSID);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));
	g.cl.state = VMS_CLUSTER_MEMBER;
	g.cl.club.epoch = START_EPOCH;

	if (n_members >= 1)
		(void)bed_member(0x000004000101ull, "VAX1", VAX1_CSID);
	if (n_members >= 2)
		(void)bed_member(0x000004000102ull, "VAX2", VAX2_CSID);

	/* the joiner: a real CSB, no CSID, not selected -- but a real
	 * connection (and so real dialogue state) all the same. */
	joiner = cnxman_club_alloc_csb(&g.cl.club, 0x000004000104ull, 1);
	bed_seed_dialogue(joiner);

	g.rb_ops.outstanding = bed_rebuild_outstanding;
	g.rb_ops.ctx = &g;
	cnxman_coord_init(&g.c, &g.cl, &g.ops);
	cnxman_coord_set_rebuild(&g.c, &g.rb_ops);
}

/* With two members the joiner lands in slot 3; with fewer, earlier. */
static int32_t bed_join_csb(uint32_t n_members)
{
	return (int32_t)(n_members + 1u);
}

/*
 * Dispatch one inbound frame, recording which CLUB slot it came from so
 * bed_respond() (above) can advance the right CSB's dialogue counters --
 * exactly the correlation the real connection manager gets from the CDT the
 * request arrived on (vms_cnxman.h's own doc comment on `respond`).
 */
static enum cnxman_coord_rx coord_feed(struct cnxman_coord *c,
				       const uint8_t *f, uint32_t n,
				       int32_t from_csb)
{
	g.dispatching_from_csb = from_csb;
	/* E73: the coordinator parses the SYSAP BODY too. */
	return cnxman_coord_rx_body(c, f + VMS_OFF_SYSAP_BODY,
				    n - VMS_OFF_SYSAP_BODY, from_csb);
}

/* ==========================================================================
 * Building the frames the coordinator RECEIVES
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
	(void)vms_frame_compose_link(&l, f, VMS_CM_FRAME_LEN, &written);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFF_CM_SEND_MSG, 0x0220);
	vms_wire_put_le16(&w, VMS_OFF_CM_ACK_MSG, 0x0140);
	vms_wire_put_le16(&w, VMS_OFF_CM_TXN, 0x0031);
	vms_wire_put_le16(&w, VMS_OFF_CM_TOKEN, 0x0abc);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY, cat);
	vms_wire_put_u8(&w, VMS_OFF_CM_OPCODE, op);
	return VMS_CM_FRAME_LEN;
}

/* cat-0x01 op-0x02: a system's join/add-member request. Spec sec 4(o) step 4:
 * "config/topology -- this starts admission". */
static uint32_t mk_join_request(uint8_t *f)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_CONFIG);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_COMMIT);
	return n;
}

/* A 0x81 response to one of the coordinator's own requests. */
static uint32_t mk_response(uint8_t *f, uint8_t cat, uint8_t op)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, cat, op);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_u8(&w, VMS_OFF_CM_CATEGORY,
			vms_wire_response_category(cat));
	vms_wire_put_u8(&w, VMS_OFF_CM_RESP_MARK, 0x01);
	return n;
}

/* cat-0x01 op-0x0b: one member reporting barrier step N. */
static uint32_t mk_step(uint8_t *f, uint32_t epoch, uint32_t step)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_le32(&w, VMS_OFF_CM_STEP, step);
	return n;
}

/* Another connection manager's class-0x02 ADD open -- a collision. */
static uint32_t mk_foreign_open(uint8_t *f, uint32_t epoch)
{
	vms_wire_buf_t w;
	uint32_t n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD);

	vms_wire_buf_init(&w, f, VMS_CM_FRAME_LEN);
	vms_wire_put_le32(&w, VMS_OFF_CM_EPOCH, epoch);
	vms_wire_put_u8(&w, VMS_OFF_CM_ROLE, VMS_CM_ROLE_XITION);
	vms_wire_put_u8(&w, VMS_OFF_CM_CLASS, VMS_CM_CLASS_ADD);
	vms_wire_put_u8(&w, VMS_OFF_CM_BITMAP, 0x0eu);
	return n;
}

/* ==========================================================================
 * Reading what the coordinator EMITTED
 * ========================================================================== */

static uint32_t count_sent(uint8_t cat, uint8_t op)
{
	uint32_t i, n = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].category == cat && g.sent[i].opcode == op)
			n++;
	}
	return n;
}

static const struct sent_frame *nth_sent(uint8_t cat, uint8_t op, uint32_t k)
{
	uint32_t i, n = 0;

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].category != cat || g.sent[i].opcode != op)
			continue;
		if (n == k)
			return &g.sent[i];
		n++;
	}
	return NULL;
}

static uint32_t sent_le32(const struct sent_frame *s, uint32_t off)
{
	vms_wire_view_t v;

	vms_wire_view_init(&v, s->bytes, s->len);
	return vms_wire_get_le32(&v, off);
}

static uint8_t sent_u8(const struct sent_frame *s, uint32_t off)
{
	vms_wire_view_t v;

	vms_wire_view_init(&v, s->bytes, s->len);
	return vms_wire_get_u8(&v, off);
}

static uint16_t sent_le16(const struct sent_frame *s, uint32_t off)
{
	vms_wire_view_t v;

	vms_wire_view_init(&v, s->bytes, s->len);
	return vms_wire_get_le16(&v, off);
}

/* ==========================================================================
 * Driving the reference ADD to the point where the barrier starts
 * ========================================================================== */

static void drive_relay_acks(uint32_t n_members)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY);
	uint32_t i;

	for (i = 0; i < n_members; i++)
		(void)coord_feed(&g.c, f, n, (int32_t)(i + 1u));
}

static void drive_commit_ack(uint32_t n_members)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT);

	(void)coord_feed(&g.c, f, n, bed_join_csb(n_members));
}

/* Every frozen participant acknowledges Phase 1 (p. 7-41). */
static void drive_open_acks(uint32_t n_members)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD);
	uint32_t i;

	for (i = 0; i < n_members; i++)
		(void)coord_feed(&g.c, f, n, (int32_t)(i + 1u));
	(void)coord_feed(&g.c, f, n, bed_join_csb(n_members));
}

static void drive_add_to_barrier(uint32_t n_members)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n = mk_join_request(f);

	(void)coord_feed(&g.c, f, n, bed_join_csb(n_members));
	drive_relay_acks(n_members);
	drive_commit_ack(n_members);
	drive_open_acks(n_members);
}

/* One member reports step N. */
static void report_step(int32_t csb, uint32_t step)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n = mk_step(f, g.c.epoch, step);

	(void)coord_feed(&g.c, f, n, csb);
}

/* ==========================================================================
 * 1. THE SELECTION PREDICATE -- design sec 5.5, book-grounding D7
 * ========================================================================== */

static void test_nobody_asks_nothing_happens(void)
{
	printf("\n-- with nobody asking, this node elects itself for nothing --\n");
	bed_init(2);

	cnxman_coord_timer(&g.c);
	cnxman_coord_timer(&g.c);

	ct_check_eq_u32(g.n_sent, 0, "not one frame originated unprompted");
	ct_check_eq_u32(g.c.transitions_driven, 0, "no transition started");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_IDLE, "still idle");
	ct_check_eq_u32(g.cl.club.we_coordinate, 0, "the CLUB is not claimed");
}

static void test_being_asked_is_the_selection(void)
{
	printf("\n-- being asked IS the selection (book pp. 7-37/7-38) --\n");
	bed_init(2);

	ct_check(cnxman_coord_select(&g.c, CNXMAN_COORD_TRIG_ASKED,
				     bed_join_csb(2)) == CNXMAN_COORD_DRIVE,
		 "a member with a learned CSID, asked, drives it");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_NONE, "no refusal");
}

static void test_no_local_csid_refuses(void)
{
	printf("\n-- a node with no learned CSID cannot coordinate --\n");
	bed_init(2);
	g.cl.club.local_csid_valid = 0u;

	ct_check(cnxman_coord_select(&g.c, CNXMAN_COORD_TRIG_ASKED,
				     bed_join_csb(2)) == CNXMAN_COORD_REFUSE,
		 "refused");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_NOT_MEMBER,
			"because this node has no cluster system id");
	ct_check_eq_u32(g.n_sent, 0, "and nothing went out");
}

static void test_unknown_subject_refuses(void)
{
	printf("\n-- a subject we hold no CSB for is refused, never invented --\n");
	bed_init(2);

	ct_check(cnxman_coord_select(&g.c, CNXMAN_COORD_TRIG_ASKED, -1) ==
		 CNXMAN_COORD_REFUSE, "refused");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_NO_SUBJECT,
			"because there is no system block for it");
}

static void test_coordinator_lock_held_elsewhere_defers(void)
{
	printf("\n-- the coordinator lock's local half (book pp. 7-30/7-32) --\n");
	bed_init(2);

	/* Another connection manager opened one and we acknowledged it: real
	 * CLUB state, not an opinion. */
	g.cl.club.transition_active = 1u;
	g.cl.club.we_coordinate = 0u;

	ct_check(cnxman_coord_select(&g.c, CNXMAN_COORD_TRIG_ASKED,
				     bed_join_csb(2)) == CNXMAN_COORD_BACKOFF,
		 "we do not drive while another CM holds the transition");

	/* And the op 0x02 arriving in that state backs off rather than racing. */
	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_DEFER, "state is defer");
	ct_check_eq_u32(g.c.deferrals, 1, "one deferral counted");
	ct_check_eq_u32(g.n_sent, 0, "and nothing was put on the wire");
	ct_check(g.fake.timers_armed >= 1, "a back-off timer was armed");
	ct_check(g.fake.last_arm_ms >= CNXMAN_COORD_BACKOFF_BASE_MS &&
		 g.fake.last_arm_ms < CNXMAN_COORD_BACKOFF_BASE_MS +
				      CNXMAN_COORD_BACKOFF_SPAN_MS,
		 "inside the p. 7-32 short interval");
}

static void test_backoff_expiry_does_not_admit_on_our_own_say_so(void)
{
	printf("\n-- a deferred ADD waits to be asked again, never self-drives --\n");
	bed_init(2);
	g.cl.club.transition_active = 1u;

	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	/* The other transition finished; the back-off elapses. */
	g.cl.club.transition_active = 0u;
	g.fake.now_ms += CNXMAN_COORD_BACKOFF_BASE_MS +
			 CNXMAN_COORD_BACKOFF_SPAN_MS;
	cnxman_coord_timer(&g.c);

	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_IDLE, "back to idle");
	ct_check_eq_u32(g.n_sent, 0,
			"no transition manufactured for a system that "
			"stopped asking");
}

static void test_already_coordinating_refuses(void)
{
	printf("\n-- one transition at a time from this node --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	ct_check(cnxman_coord_select(&g.c, CNXMAN_COORD_TRIG_DETECTED,
				     CSB_VAX1) == CNXMAN_COORD_REFUSE,
		 "refused while one is running");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_BUSY, "as BUSY");
}

/* ==========================================================================
 * 2. THE PROLOGUE -- op 0x02 -> op 0x12 relay -> op 0x03 commit
 * ========================================================================== */

static void test_relay_precedes_commit(void)
{
	printf("\n-- the relay is the commit gate (spec sec 4(O.31)) --\n");
	bed_init(2);

	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY), 2,
			"one relay per OTHER member, none to the joiner");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT), 0,
			"and NO commit until they answer");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_RELAY, "state is relay");

	drive_relay_acks(1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT), 0,
			"one of two answering is not enough");
	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY);

		(void)coord_feed(&g.c, f, n, CSB_VAX2);
	}
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT), 1,
			"both answered: the subject is committed");
	{
		const struct sent_frame *s =
			nth_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT, 0);

		ct_check_eq_u32(s->dst, JOIN_CSID,
				"addressed to the CSID we assigned the joiner");
		ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_ROLE), VMS_CM_ROLE_COMMIT,
				"role slot 0x20 (spec sec 4(r))");
	}
}

static void test_two_node_cluster_has_an_empty_relay_set(void)
{
	printf("\n-- with no other member there is nothing to relay to --\n");
	bed_init(0);

	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(0));
	}
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY), 0,
			"no relays");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT), 1,
			"straight to the commit");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_COMMIT, "state is commit");
}

/* ==========================================================================
 * 3. CSID ASSIGNMENT AND THE NODEMAP -- book p. 7-25, spec sec 4(p)
 * ========================================================================== */

static void test_csid_assignment_and_the_reference_bitmap(void)
{
	const struct sent_frame *s;
	struct vms_csb *joiner;

	printf("\n-- the coordinator assigns the CSID and builds the nodemap --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	joiner = cnxman_club_csb_at(&g.cl.club, (uint32_t)bed_join_csb(2));
	ct_check(joiner != NULL && joiner->csid_valid, "the joiner has a CSID");
	ct_check_eq_u32(joiner->csid, JOIN_CSID,
			"slot 4, sequence 1 -- one past the highest ever seen "
			"(book p. 7-25: a vacated slot is not reused)");
	ct_check_eq_u32(g.c.csids_assigned, 1, "exactly one assignment");

	s = nth_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD, 0);
	ct_check(s != NULL, "a class-0x02 open went out");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_BITMAP), 0x1eu,
			"nodemap 0x1e -- bits 1,2,3,4, the spec's own "
			"VAX1+VAX2+VAX3+OVMX specimen");
	ct_check_eq_u32(cnxman_phase2_popcount8(0x1eu), 4,
			"popcount == the post-transition member count");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_ROLE), VMS_CM_ROLE_XITION,
			"role slot 0x40");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_CLASS), VMS_CM_CLASS_ADD,
			"class 0x02: tag 0x0240 (spec sec 4(r))");
}

static void test_slot_outside_the_grounded_byte_is_refused(void)
{
	printf("\n-- a slot the nodemap byte cannot name refuses the ADD --\n");
	bed_init(2);
	/* Push the highest known slot to the last one the byte can express. */
	cnxman_csb_set_csid(cnxman_club_csb_at(&g.cl.club, CSB_VAX2),
			    (vms_csid_t)0x00010007u);

	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	ct_check_eq_u32(g.n_sent, 0, "not one frame originated");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_NO_SLOT,
			"refused for want of an expressible CSV slot");
	ct_check_eq_u32(g.cl.club.we_coordinate, 0, "the CLUB was not claimed");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_IDLE, "still idle");
}

static void test_member_outside_the_nodemap_refuses(void)
{
	struct vms_csb *joiner;

	printf("\n-- a system the nodemap cannot name refuses the whole open --\n");
	bed_init(2);
	/*
	 * A CSID whose CSV slot is 0. Book p. 7-25: slot 0 is never used, and
	 * the wire agrees (spec sec 4(p): "bit 0 is never set"). If the cluster
	 * ever asserts one anyway, our base assumption about where the map
	 * starts is wrong -- and an open built on it would go out with THIS
	 * NODE missing from its own membership map. Refuse, loudly.
	 */
	cnxman_club_learn_local_csid(&g.cl.club, (vms_csid_t)0x00010000u);

	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	ct_check_eq_u32(g.n_sent, 0, "not one frame originated");
	ct_check_eq_u32(g.c.last_refusal, CNXMAN_COORD_REF_NO_NODEMAP,
			"refused rather than dropping a member from the map");
	joiner = cnxman_club_csb_at(&g.cl.club, (uint32_t)bed_join_csb(2));
	ct_check_eq_u32(joiner->csid_valid, 0,
			"and no identity was stamped on the joiner");
}

/* ==========================================================================
 * 4. PHASE 1 / PHASE 2 -- book pp. 7-41/7-42
 * ========================================================================== */

static void test_go_waits_for_every_phase1_ack(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n;

	printf("\n-- the GO waits for every Phase 1 acknowledgement --\n");
	bed_init(2);
	{
		uint8_t r[VMS_CM_FRAME_LEN];
		uint32_t m = mk_join_request(r);

		(void)coord_feed(&g.c, r, m, bed_join_csb(2));
	}
	drive_relay_acks(2);
	drive_commit_ack(2);

	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD), 3,
			"the proposal went to all three participants");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 0,
			"no GO yet");

	n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD);
	(void)coord_feed(&g.c, f, n, CSB_VAX1);
	(void)coord_feed(&g.c, f, n, CSB_VAX2);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 0,
			"two of three is not enough (p. 7-41)");
	ct_check_eq_u32(g.c.phase2_committed, 0, "and nothing is committed");

	(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 3,
			"the last ack releases the GO to everybody");
	ct_check_eq_u32(g.c.phase2_committed, 1, "Phase 2 has run");
}

static void test_count_commits_in_phase2_before_the_barrier(void)
{
	printf("\n-- the member count commits at Phase 2, before the rebuild --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_BARRIER, "barrier running");
	ct_check_eq_u32(g.c.step, 1, "at step 1");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"before a single release");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 4,
			"and the count is already 4 (book p. 7-42, D13)");
	ct_check_eq_u32(g.c.count_mismatch, 0,
			"the CSB count agrees with the nodemap we asserted");
	{
		struct vms_csb *j =
			cnxman_club_csb_at(&g.cl.club, (uint32_t)bed_join_csb(2));

		ct_check(cnxman_csb_is_member(j),
			 "the joiner's CSB carries member");
	}
}

static void test_the_go_is_a_notification(void)
{
	const struct sent_frame *s;

	printf("\n-- the GO carries txn=0 and the 0x0260 tag (spec sec 4(p)/(r)) --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	s = nth_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO, 0);
	ct_check(s != NULL, "a GO went out");
	ct_check_eq_u32(sent_le16(s, VMS_OFF_CM_TXN), 0,
			"txn=0: notifications are never answered");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_ROLE), VMS_CM_ROLE_GO,
			"role 0x60");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_CLASS), VMS_CM_CLASS_ADD,
			"class 0x02 -- the tag is 0x0260");
	ct_check_eq_u32(sent_le32(s, VMS_OFF_CM_EPOCH), g.c.epoch,
			"and the epoch of THIS transition");
}

/* ==========================================================================
 * 5. THE LAW -- 12 x (M-1), and 0x0c#N never before the last 0x0b#N
 * ========================================================================== */

/* Run the whole census for a cluster with `n_members` established members
 * besides us, plus the joiner. Returns M-1. */
static uint32_t run_full_barrier(uint32_t n_members)
{
	uint32_t participants = n_members + 1u;   /* + the joiner */
	uint32_t step, i;

	for (step = 1; step <= CNXMAN_COORD_BARRIER_STEPS; step++) {
		for (i = 0; i < n_members; i++)
			report_step((int32_t)(i + 1u), step);
		report_step(bed_join_csb(n_members), step);
	}
	return participants;
}

static void test_twelve_times_m_minus_one(void)
{
	uint32_t m_minus_1;

	printf("\n-- THE LAW: #0x0c == #0x0b == 12 x (M-1) (spec sec 4(p)) --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	ct_check_eq_u32(g.c.n_participants, 3,
			"M-1 == 3, from the real SELECTED CSBs plus the joiner");
	ct_check_eq_u32(cnxman_coord_expected_releases(&g.c), 36,
			"12 x (M-1) owed");

	m_minus_1 = run_full_barrier(2);

	ct_check_eq_u32(g.c.steps_received, 12 * m_minus_1,
			"12 x (M-1) step reports consumed");
	ct_check_eq_u32(g.c.step_acks_sent, 12 * m_minus_1,
			"one 0x81/0x0b acknowledgement each");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL),
			12 * m_minus_1, "and 12 x (M-1) releases ORIGINATED");
	ct_check_eq_u32(g.c.releases_sent, 12 * m_minus_1,
			"the FSM's own count agrees");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_COMPLETE,
		 "release #12 completed the transition");
	ct_check_eq_u32(g.c.transitions_completed, 1, "one completed");
	ct_check_eq_u32(g.cl.club.we_coordinate, 0,
			"and the coordinator lock is released (p. 7-42)");
	ct_check_eq_u32(g.cl.club.transition_active, 0, "no transition active");
}

static void test_law_scales_with_m(void)
{
	printf("\n-- the same law at M=2 and M=3 --\n");
	bed_init(0);
	drive_add_to_barrier(0);
	ct_check_eq_u32(g.c.n_participants, 1, "M=2 -> one participant");
	(void)run_full_barrier(0);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL),
			12, "12 x 1 releases");

	bed_init(1);
	drive_add_to_barrier(1);
	ct_check_eq_u32(g.c.n_participants, 2, "M=3 -> two participants");
	(void)run_full_barrier(1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL),
			24, "12 x 2 releases");
}

static void test_release_never_precedes_the_last_report(void)
{
	printf("\n-- 0x0c#N never precedes the LAST 0x0b#N --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	report_step(CSB_VAX1, 1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"one of three reported: nothing released");
	report_step(CSB_VAX2, 1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"two of three: still nothing");
	report_step(bed_join_csb(2), 1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 3,
			"the slowest member reported: released to ALL three");
	ct_check_eq_u32(g.c.step, 2, "and the barrier advanced to step 2");
}

static void test_release_fields_trace_to_real_state(void)
{
	const struct sent_frame *s;
	uint32_t i;

	printf("\n-- every release field traces to real executive state --\n");
	bed_init(2);
	drive_add_to_barrier(2);
	report_step(CSB_VAX1, 1);
	report_step(CSB_VAX2, 1);
	report_step(bed_join_csb(2), 1);

	for (i = 0; i < 3; i++) {
		s = nth_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL, i);
		ct_check(s != NULL, "release present");
		ct_check_eq_u32(sent_le32(s, VMS_OFF_CM_EPOCH), g.c.epoch,
				"epoch is the CLUB's, advanced by us");
		ct_check_eq_u32(sent_le32(s, VMS_OFF_CM_STEP), 1,
				"step index 1");
		ct_check_eq_u32(sent_le16(s, VMS_OFF_CM_TXN), 0,
				"txn=0: a release is never answered");
		/*
		 * ... AND SO IS THE TOKEN (E85, a CORRECTION). This used to
		 * assert the token came from the CM's counter (>= the bed's
		 * seed), on the reasoning that only txn was spec-constrained.
		 * The wire says otherwise: 1104 of 1104 real op-0x0c releases
		 * in the capture library carry a ZERO token, as do 125 of 125
		 * op-0x0a GOs. A message no node ever answers has nothing to
		 * correlate, and the tightened assertion (== 0, not >= a bed
		 * seed) is the one the census supports. The anti-LARP check it
		 * replaced now lives where the token is really asserted -- the
		 * op-0x0b step, test_cnxman_barrier.c.
		 */
		ct_check_eq_u32(sent_le16(s, VMS_OFF_CM_TOKEN), 0,
				"and token=0 with it: a notification carries "
				"no correlation pair at all");
	}
	ct_check_eq_u32(g.c.epoch, START_EPOCH + 1u,
			"the epoch is the CLUB's own, advanced (spec sec 4(r) "
			"grounds monotonicity)");
}

static void test_step_ack_shape(void)
{
	const struct sent_frame *s;
	uint32_t i, responses = 0;

	printf("\n-- the 0x81/0x0b ack: response bit + role slot 0x10 --\n");
	bed_init(2);
	drive_add_to_barrier(2);
	report_step(CSB_VAX1, 1);

	for (i = 0; i < g.n_sent; i++) {
		if (g.sent[i].was_response)
			responses++;
	}
	ct_check_eq_u32(responses, 1, "exactly one response emitted");
	s = &g.sent[g.n_sent - 1];
	ct_check_eq_u32(s->category,
			vms_wire_response_category(VMS_CM_CAT_CONFIG),
			"category 0x81");
	ct_check_eq_u32(s->opcode, VMS_CM_OP_BARRIER, "opcode 0x0b");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_ROLE), VMS_CM_ROLE_RELAY,
			"role slot 0x10 (spec sec 4(r)'s 26-capture census)");
	ct_check_eq_u32(sent_le16(s, VMS_OFF_CM_TOKEN), 0x0abc,
			"the REQUEST's token echoed: that is the correlation");
}

static void test_out_of_order_and_stale_steps(void)
{
	printf("\n-- a report we cannot place credits nothing --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	/* A step ahead of the one in flight. */
	report_step(CSB_VAX1, 5);
	ct_check_eq_u32(g.c.step_out_of_order, 1, "counted as out of order");
	ct_check_eq_u32(g.c.steps_received, 0, "credited to nobody");

	/* A report for a different transition. */
	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_step(f, g.c.epoch + 99u, 1);

		(void)coord_feed(&g.c, f, n, CSB_VAX1);
	}
	ct_check_eq_u32(g.c.epoch_mismatch, 1, "counted as another epoch");

	/* A system that is not in the frozen census. */
	report_step(CSB_LOCAL, 1);
	ct_check_eq_u32(g.c.unknown_peer, 1, "counted as an unknown reporter");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"and not one release went out");
}

static void test_retransmitted_step_is_reacknowledged(void)
{
	printf("\n-- a retransmitted step is answered again, counted once --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	report_step(CSB_VAX1, 1);
	report_step(CSB_VAX1, 1);
	ct_check_eq_u32(g.c.step_acks_sent, 2, "both acknowledged");
	ct_check_eq_u32(g.c.step_duplicates, 1, "one counted as a duplicate");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"and the census did not advance");
}

/* ==========================================================================
 * 6. THE REBUILD GATE -- spec sec 4(p)
 * ========================================================================== */

static void test_release_held_for_outstanding_rebuild_records(void)
{
	printf("\n-- the release is HELD while a rebuild record is outstanding --\n");
	bed_init(2);
	drive_add_to_barrier(2);
	g.rebuild_outstanding = 5u;   /* the five that froze a real barrier */

	report_step(CSB_VAX1, 1);
	report_step(CSB_VAX2, 1);
	report_step(bed_join_csb(2), 1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"everybody reported, but the records gate the step");
	ct_check(g.c.rebuild_holds >= 1, "the hold is counted");
	ct_check_eq_u32(g.c.step, 1, "still at step 1");

	g.rebuild_outstanding = 0u;
	cnxman_coord_timer(&g.c);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 3,
			"drained: the beat releases it");
	ct_check_eq_u32(g.c.step, 2, "and the barrier advances");
}

/* ==========================================================================
 * 7. COLLISIONS AND LOSSES -- book pp. 7-32/7-41/7-42
 * ========================================================================== */

static void test_collision_before_the_go_hands_off(void)
{
	enum cnxman_coord_rx rx;
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n;

	printf("\n-- a colliding open before our GO: we drop ours and hand off --\n");
	bed_init(2);
	{
		uint8_t r[VMS_CM_FRAME_LEN];
		uint32_t m = mk_join_request(r);

		(void)coord_feed(&g.c, r, m, bed_join_csb(2));
	}
	drive_relay_acks(2);
	drive_commit_ack(2);
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_OPEN, "in Phase 1");

	n = mk_foreign_open(f, 0x20u);
	rx = coord_feed(&g.c, f, n, CSB_VAX1);

	ct_check(rx == CNXMAN_COORD_RX_NOT_MINE,
		 "the frame is handed to the participant FSM");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_ABANDONED, "ours abandoned");
	ct_check_eq_u32(g.c.collisions, 1, "collision counted");
	ct_check_eq_u32(g.cl.club.we_coordinate, 0, "the CLUB is released");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 0,
			"and no GO was ever sent");
}

static void test_collision_after_the_go_is_past_the_point_of_no_return(void)
{
	enum cnxman_coord_rx rx;
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n;

	printf("\n-- after the GO a collision cannot un-commit it (p. 7-42) --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	n = mk_foreign_open(f, 0x20u);
	rx = coord_feed(&g.c, f, n, CSB_VAX1);

	ct_check(rx == CNXMAN_COORD_RX_NOT_MINE, "still routed on");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_BARRIER, "ours stands");
	ct_check_eq_u32(g.c.collisions, 1, "counted");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 4, "the count is untouched");
}

static void test_lost_participant_before_the_go_abandons(void)
{
	printf("\n-- a lost participant in Phase 1 abandons it (p. 7-41) --\n");
	bed_init(2);
	{
		uint8_t r[VMS_CM_FRAME_LEN];
		uint32_t m = mk_join_request(r);

		(void)coord_feed(&g.c, r, m, bed_join_csb(2));
	}
	drive_relay_acks(2);
	drive_commit_ack(2);

	cnxman_coord_participant_lost(&g.c, CSB_VAX2);
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_ABANDONED, "abandoned");
	ct_check_eq_u32(g.c.transitions_abandoned, 1, "counted");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 0,
			"no GO");
}

static void test_lost_participant_in_the_barrier_does_not_strand_it(void)
{
	printf("\n-- a member lost mid-barrier does not strand the survivors --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	report_step(CSB_VAX1, 1);
	report_step(bed_join_csb(2), 1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"held for VAX2");

	cnxman_coord_participant_lost(&g.c, CSB_VAX2);
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_BARRIER,
		 "the committed transition is NOT abandoned (p. 7-42)");
	ct_check_eq_u32(g.c.n_participants, 2, "the census shrank to 2");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 2,
			"and the survivors were released at once");
}

/* ==========================================================================
 * 8. THE REMOVE CLASS -- same law, different opening
 * ========================================================================== */

static void test_remove_class(void)
{
	const struct sent_frame *s;
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n, step, i;

	printf("\n-- a class-0x03 removal: op 0x08, no nodemap, same 12 steps --\n");
	bed_init(2);

	ct_check(cnxman_coord_propose_remove(&g.c, CSB_VAX2) ==
		 CNXMAN_COORD_DRIVE, "the first detector drives it (p. 7-2)");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY), 0,
			"a removal has no relay");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT), 0,
			"and no commit (spec sec 4(r))");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_REM), 1,
			"one op 0x08 open, to the one surviving member");
	ct_check_eq_u32(g.c.n_participants, 1,
			"the departing system is NOT in the census");

	s = nth_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_REM, 0);
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_CLASS), VMS_CM_CLASS_REMOVE,
			"class 0x03: tag 0x0340");
	ct_check_eq_u32(sent_u8(s, VMS_OFF_CM_BITMAP), 0,
			"and NO nodemap (spec sec 4(p))");

	n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_REM);
	(void)coord_feed(&g.c, f, n, CSB_VAX1);
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO), 1,
			"Phase 1 acknowledged: the GO goes out");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 2,
			"and the count drops to 2 (p. 7-46)");
	{
		struct vms_csb *gone = cnxman_club_csb_at(&g.cl.club, CSB_VAX2);

		ct_check((gone->flags & VMS_CSB_F_REMOVED) != 0u,
			 "the departing system is flagged removed");
		ct_check_eq_u32(gone->flags & VMS_CSB_F_SELECTED, 0,
				"and deselected");
	}

	for (step = 1; step <= CNXMAN_COORD_BARRIER_STEPS; step++)
		report_step(CSB_VAX1, step);
	i = count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL);
	ct_check_eq_u32(i, 12, "12 x (M-1) with M=2: twelve releases");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_COMPLETE, "completed");
}

static void test_removing_the_last_peer_completes_locally(void)
{
	printf("\n-- removing the last peer: 12 x (M-1) = 0, and it completes --\n");
	bed_init(1);

	ct_check(cnxman_coord_propose_remove(&g.c, CSB_VAX1) ==
		 CNXMAN_COORD_DRIVE, "driven");
	ct_check_eq_u32(g.c.n_participants, 0, "nobody left to barrier with");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"12 x 0 releases");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_COMPLETE,
		 "and it does NOT hang in Phase 1 waiting on an empty set");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 1, "the survivor is alone");
	ct_check_eq_u32(g.cl.club.transition_active, 0, "and it is over");
}

/* ==========================================================================
 * 9. HONESTY: no link, honest omissions, the DLM seam, readback
 * ========================================================================== */

static void test_no_link_originates_nothing(void)
{
	uint32_t releases_before;

	printf("\n-- no CSB for a participant means no frame to it, never a "
	       "zero-filled one --\n");
	bed_init(2);
	drive_add_to_barrier(2);   /* real CSBs for VAX1, VAX2, the joiner */

	/* Two of the three frozen participants lose their CSB mid-barrier --
	 * a real, honest scenario (design sec 3.2.4 ruling E1: reachability
	 * IS CSB presence now, there is no separate link probe). The census
	 * itself (part_flags[]) is untouched: these two are still owed
	 * releases, and the FSM must refuse to send them rather than invent
	 * a frame (INV-6). */
	cnxman_club_free_csb(&g.cl.club, cnxman_club_csb_at(&g.cl.club, CSB_VAX1));
	cnxman_club_free_csb(&g.cl.club,
			     cnxman_club_csb_at(&g.cl.club, (uint32_t)bed_join_csb(2)));

	releases_before = g.c.send_failures;
	report_step(CSB_VAX1, 1);
	report_step(CSB_VAX2, 1);
	report_step(bed_join_csb(2), 1);

	ct_check(g.c.send_failures - releases_before >= 2,
		 "each refusal to a vanished CSB is counted");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 1,
			"ONLY the still-reachable participant (VAX2) got a "
			"release -- never a zero-filled substitute for the "
			"other two");
}

static void test_omissions_are_counted_not_faked(void)
{
	printf("\n-- what we cannot place on the wire is counted, not guessed --\n");
	bed_init(2);
	drive_add_to_barrier(2);

	ct_check_eq_u32(g.c.membership_burst_omitted, 1,
			"the op 0x06 membership burst is omitted (its record "
			"layout has no isolated offset)");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_MEMBERSHIP), 0,
			"and no empty membership burst was invented");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_LOCKRB), 0,
			"nor an empty op 0x05 lock-rebuild burst");
	ct_check_eq_u32(g.c.open_cells_omitted, 3,
			"the Phase 1 proposal's un-isolated cells are counted "
			"per open (book p. 7-40)");
	ct_check_eq_u32(g.c.relay_subject_omitted, 2,
			"and the relay's subject field, per relay");
}

static void test_dlm_seam(void)
{
	printf("\n-- the DLM sees the transition it coordinates --\n");
	bed_init(2);
	cnxman_coord_set_dlm(&g.c, &bed_dlm_ops);
	drive_add_to_barrier(2);

	ct_check_eq_u32(g.dlm_begins, 1, "transition_begin once");
	ct_check_eq_u32(g.dlm_last_tr.we_coordinate, 1,
			"and it is told WE coordinate");
	ct_check_eq_u32(g.dlm_last_tr.subject_csid_valid, 1,
			"the subject is known here, unlike on the participant "
			"side");
	ct_check_eq_u32(g.dlm_last_tr.subject_csid, JOIN_CSID,
			"and it is the CSID we assigned");
	ct_check_eq_u32(g.dlm_last_tr.coordinator_csid, OWN_CSID,
			"the coordinator is us");

	(void)run_full_barrier(2);
	ct_check_eq_u32(g.dlm_ends, 1, "transition_end once");
	ct_check_eq_u32((uint32_t)g.dlm_last_completed, 1, "completed");
}

static void test_dlm_told_when_abandoned(void)
{
	printf("\n-- an abandoned transition unwinds the DLM's rebuild --\n");
	bed_init(2);
	cnxman_coord_set_dlm(&g.c, &bed_dlm_ops);
	{
		uint8_t f[VMS_CM_FRAME_LEN];
		uint32_t n = mk_join_request(f);

		(void)coord_feed(&g.c, f, n, bed_join_csb(2));
	}
	cnxman_coord_abandon(&g.c, NULL);
	ct_check_eq_u32(g.dlm_ends, 1, "transition_end once");
	ct_check_eq_u32((uint32_t)g.dlm_last_completed, 0, "completed = 0");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_ABANDONED, "abandoned");
}

static void test_foreign_frames_route_on(void)
{
	uint8_t f[VMS_CM_FRAME_LEN];
	uint32_t n;

	printf("\n-- when idle, a peer's transition belongs to the participant --\n");
	bed_init(2);

	n = mk_foreign_open(f, 0x20u);
	ct_check(coord_feed(&g.c, f, n, CSB_VAX1) ==
		 CNXMAN_COORD_RX_NOT_MINE, "an open routes on");

	n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO);
	ct_check(coord_feed(&g.c, f, n, CSB_VAX1) ==
		 CNXMAN_COORD_RX_NOT_MINE, "a GO routes on");

	n = mk_frame(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_ABORT);
	ct_check(coord_feed(&g.c, f, n, CSB_VAX1) ==
		 CNXMAN_COORD_RX_NOT_MINE, "an abort routes on");

	n = mk_frame(f, VMS_CM_CAT_DLM, VMS_CM_OP_DLM_REBUILD);
	ct_check(coord_feed(&g.c, f, n, CSB_VAX1) ==
		 CNXMAN_COORD_RX_NOT_MINE, "an inbound rebuild record routes on");

	n = mk_response(f, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER);
	ct_check(coord_feed(&g.c, f, n, CSB_VAX1) ==
		 CNXMAN_COORD_RX_NOT_MINE,
		 "and a 0x81/0x0b is the participant's, never ours");
	ct_check_eq_u32(g.n_sent, 0, "none of them made us emit anything");
}

static void test_slow_step_is_never_timed_out(void)
{
	uint32_t i;

	printf("\n-- a slow member is counted, never timed out (spec sec 4(p)) --\n");
	bed_init(2);
	drive_add_to_barrier(2);
	report_step(CSB_VAX1, 1);

	for (i = 0; i < 60; i++) {
		g.fake.now_ms += CNXMAN_COORD_WATCH_MS;
		cnxman_coord_timer(&g.c);
	}
	ct_check_eq_u32(g.c.slow_steps, 60, "each beat counted");
	ct_check(g.c.state == (uint8_t)CNXMAN_COORD_BARRIER,
		 "the transition is still running after a minute");
	ct_check_eq_u32(g.c.transitions_abandoned, 0, "nothing abandoned");
	ct_check_eq_u32(count_sent(VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL), 0,
			"and no release was forced");
}

static void test_transition_readback(void)
{
	struct cnxman_transition tr;

	printf("\n-- the readback reports a real transition or none --\n");
	bed_init(2);
	ct_check(cnxman_coord_transition(&g.c, &tr) != 0,
		 "idle: no transition, not a zeroed one");
	ct_check_eq_u32(cnxman_coord_expected_releases(&g.c), 0,
			"and nothing is owed");

	drive_add_to_barrier(2);
	ct_check(cnxman_coord_transition(&g.c, &tr) == 0, "running: reported");
	ct_check_eq_u32(tr.epoch, g.c.epoch, "the epoch");
	ct_check_eq_u32(tr.we_coordinate, 1, "we coordinate");
	ct_check_eq_u32(tr.tr_class, VMS_CM_CLASS_ADD, "class 0x02");
	ct_check_eq_u32(tr.coordinator_csid, OWN_CSID, "coordinator is us");
}

static void test_two_transitions_back_to_back(void)
{
	printf("\n-- a second transition gets its own epoch and census --\n");
	bed_init(2);
	drive_add_to_barrier(2);
	(void)run_full_barrier(2);
	ct_check_eq_u32(g.c.epoch, START_EPOCH + 1u, "first epoch");

	ct_check(cnxman_coord_propose_remove(&g.c, CSB_VAX1) ==
		 CNXMAN_COORD_DRIVE, "a removal may follow immediately");
	ct_check_eq_u32(g.c.epoch, START_EPOCH + 2u,
			"with the next epoch (monotone, spec sec 4(r))");
	ct_check_eq_u32(g.c.transitions_driven, 2, "two driven");
}

static void test_names(void)
{
	printf("\n-- names, for the console and the diagnostics --\n");
	ct_check(cnxman_coord_state_name(CNXMAN_COORD_BARRIER)[0] == 'b',
		 "barrier");
	ct_check(cnxman_coord_state_name(CNXMAN_COORD_STATE__COUNT)[0] == '?',
		 "an out-of-range state is '?'");
	ct_check(cnxman_coord_verdict_name(CNXMAN_COORD_DRIVE)[0] == 'd',
		 "drive");
	ct_check(cnxman_coord_verdict_name(CNXMAN_COORD_BACKOFF)[0] == 'b',
		 "backoff");
}

int main(void)
{
	test_nobody_asks_nothing_happens();
	test_being_asked_is_the_selection();
	test_no_local_csid_refuses();
	test_unknown_subject_refuses();
	test_coordinator_lock_held_elsewhere_defers();
	test_backoff_expiry_does_not_admit_on_our_own_say_so();
	test_already_coordinating_refuses();

	test_relay_precedes_commit();
	test_two_node_cluster_has_an_empty_relay_set();

	test_csid_assignment_and_the_reference_bitmap();
	test_slot_outside_the_grounded_byte_is_refused();
	test_member_outside_the_nodemap_refuses();

	test_go_waits_for_every_phase1_ack();
	test_count_commits_in_phase2_before_the_barrier();
	test_the_go_is_a_notification();

	test_twelve_times_m_minus_one();
	test_law_scales_with_m();
	test_release_never_precedes_the_last_report();
	test_release_fields_trace_to_real_state();
	test_step_ack_shape();
	test_out_of_order_and_stale_steps();
	test_retransmitted_step_is_reacknowledged();

	test_release_held_for_outstanding_rebuild_records();

	test_collision_before_the_go_hands_off();
	test_collision_after_the_go_is_past_the_point_of_no_return();
	test_lost_participant_before_the_go_abandons();
	test_lost_participant_in_the_barrier_does_not_strand_it();

	test_remove_class();
	test_removing_the_last_peer_completes_locally();

	test_no_link_originates_nothing();
	test_omissions_are_counted_not_faked();
	test_dlm_seam();
	test_dlm_told_when_abandoned();
	test_foreign_frames_route_on();
	test_slow_step_is_never_timed_out();
	test_transition_readback();
	test_two_transitions_back_to_back();
	test_names();
	return ct_summary("test_cnxman_coord");
}
