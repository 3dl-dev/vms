/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_emit_guard.c - E82 R1: the EMIT-TIME WIRE-SAFETY GUARD, driven
 * through the REAL port on a REAL open circuit.
 *
 * WHAT THIS FILE HAS TO PROVE, and why each half matters as much as the other:
 *
 *   1. THE GUARD NEVER BLOCKS CORRECT OUTPUT. A golden-shaped connection
 *      manager dialogue -- the peer speaking, this node answering, a
 *      coalesced credit ack -- produces ZERO refusals and ZERO warnings, and
 *      every frame reaches the wire. A guard that refused a correct frame
 *      would break the join exactly as thoroughly as the crash it prevents,
 *      so this is the FIRST test in the file and the one that must never be
 *      relaxed to make another pass.
 *
 *   2. EVERY CRASH CLASS IS REFUSED AT THE SEND POINT. Each of the vectors
 *      cm_wire_safety_audit.py measures is synthesised here, offered to
 *      pe_vc_send_msg, and proved to have been DROPPED -- not merely counted:
 *      nothing reached the fake interface, no sequence was consumed and no
 *      unacked-ring slot was taken.
 *
 * The two observed live crashes have their own named tests: the E76
 * `CNXMGRERR` (an ack for a message the peer never sent) and the E78
 * `INVEXCEPTN` (a per-frame ack burst).
 *
 * Every stimulus and every assertion goes through the codec, never a raw
 * offset (design SS3.9 rule 2; this file is not the codec).
 */

#include <string.h>

#include "cluster_test.h"
#include "pe_fake_vc.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_scs.h"
#include "vms_cluster_emit_guard.h"

/* The userland ring dumper's own copy of the vector names -- included so the
 * drift between the executive and the image that PRINTS its records is a red
 * host test, exactly as test_cnxman_diag.c does for the ring's other tables. */
#include "cnxtrace_names.h"

#define OVMX_SYSID 1030u
#define VAX1_SYSID 1025u
#define LAB_CREDITS 10u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

/* Two real-shaped Con.ID values (spec SS4(t): a longword whose low half indexes
 * a table and whose high half is a reuse tag). OURS is the one at abs 68. */
#define CONID_OURS   0x62c50009u
#define CONID_THEIRS 0x33580008u
/* ...and the pair a REOPENED dialogue uses (E77's rebind). */
#define CONID_OURS2   0x62c5000du
#define CONID_THEIRS2 0x3358000cu

/* Body-relative offsets, every one NAMED from the codec's own constants --
 * never a literal. VMS_OFB_CM_TXN is derived the same way its published
 * siblings are, because the codec header does not spell that one out. */
#define BODY_CONID_REMOTE_OFF (VMS_OFF_SCS_CONID_REMOTE - PE_SEND_BODY_OFF)
#define BODY_CONID_LOCAL_OFF  (VMS_OFF_SCS_CONID_LOCAL  - PE_SEND_BODY_OFF)
#define BODY_SYSAP_OFF        (VMS_OFF_SYSAP_BODY       - PE_SEND_BODY_OFF)
#define OFB_CM_TXN            (VMS_OFF_CM_TXN - VMS_OFF_SYSAP_BODY)

/* ------------------------------------------------------------------ *
 * The node under test: the same open circuit test_pe_send.c drives
 * ------------------------------------------------------------------ */
struct fake_upper_rec {
	uint32_t messages;
};

static void fu_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
		       const uint8_t *body, uint32_t len)
{
	(void)from; (void)conid; (void)body; (void)len;
	((struct fake_upper_rec *)ctx)->messages++;
}

static void fu_datagram(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			uint32_t len)
{
	(void)ctx; (void)from; (void)body; (void)len;
}

static void fu_vc_up(void *ctx, vms_scs_sysid_t peer) { (void)ctx; (void)peer; }

static void fu_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	(void)ctx; (void)peer; (void)reason;
}

struct guard_env {
	struct pe_fsm         fsm;
	struct pe_ops         ops;
	struct fake_pe        fake;
	struct fake_peer      peer;
	struct fake_upper_rec upper_rec;
	struct pe_upper_ops   upper;
	struct pe_vc          vcs[2];
	uint8_t               buf[VMS_HELLO_PADDED_MAX_FRAME];
};

#define OVMX_BOOT_TIME 0x00bc0552690a0000ull

static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

static void env_init(struct guard_env *e)
{
	struct pe_identity id;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);
	e->ops.now_vms = fake_now_vms;

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;
	memcpy(id.sw_version, "VMX V0.6", 8);
	id.sw_version_valid = 1;
	memcpy(id.hw_type, "X86 ", 4);
	id.hw_type_valid = 1;
	id.credits_requested = LAB_CREDITS;
	id.credits_requested_valid = 1;
	id.incarnation_time = OVMX_BOOT_TIME;
	id.incarnation_time_valid = 1;
	id.timvcfail_ms = 16000;
	id.vc_retransmit_ms = 2000;

	(void)pe_fsm_init(&e->fsm, &id, OVMX_SYSID, &e->ops);
	pe_fsm_bind_vcs(&e->fsm, e->vcs, 2);

	e->upper.message = fu_message;
	e->upper.datagram = fu_datagram;
	e->upper.vc_up = fu_vc_up;
	e->upper.vc_down = fu_vc_down;
	e->upper.ctx = &e->upper_rec;
	pe_fsm_set_upper(&e->fsm, &e->upper);

	pe_fsm_start(&e->fsm);
	fake_peer_init(&e->peer, VAX1_SYSID, vax1_hw, "VAX1");
}

static void rx_frame(struct guard_env *e, uint32_t len)
{
	if (len != 0)
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
}

static void rx_hello(struct guard_env *e, uint8_t word, uint16_t incarnation)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_hello(&e->peer, ovmx_hw, dst_lavc, word,
				    incarnation, 0, e->buf, sizeof(e->buf)));
}

static void rx_start(struct guard_env *e, uint16_t round, uint16_t send_seq,
		     uint16_t recv_ack)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_start(&e->peer, VAX1_SYSID, ovmx_hw, dst_lavc,
				    round, send_seq, recv_ack, LAB_CREDITS,
				    e->buf, sizeof(e->buf)));
}

static void rx_vc_ack(struct guard_env *e)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_vc_ack(&e->peer, ovmx_hw, dst_lavc, 1, 0,
				     e->buf, sizeof(e->buf)));
}

static void open_circuit(struct guard_env *e)
{
	env_init(e);
	rx_hello(e, PE_PFW_VERIFY_B2, 1);
	rx_hello(e, PE_PFW_VERIFY_B4, 1);
	rx_start(e, 0, 1, 0);
	rx_vc_ack(e);
	fake_pe_clear_frames(&e->fake);
	memset(&e->upper_rec, 0, sizeof(e->upper_rec));
	/* The FORMATION itself prints (the circuit coming up). Zero the
	 * recorder here so every log assertion below counts the GUARD's lines
	 * and nothing else. */
	e->fake.logs = 0u;
	e->fake.last_log[0] = '\0';
}

static struct pe_vc *the_vc(struct guard_env *e)
{
	return pe_fsm_vc_at(&e->fsm, 0);
}

/* ------------------------------------------------------------------ *
 * Building a REAL VMS$VAXcluster body -- the 16-byte SCS header at abs
 * 56-71 (spec SS4(d)/(1b)) followed by the 10-byte transaction envelope
 * at abs 72-81 (spec SS4(j)). This is the shape CNXMAN hands down.
 * ------------------------------------------------------------------ */
struct cm_spec {
	uint32_t remote_conid;
	uint32_t local_conid;
	uint16_t send_msg;
	uint16_t ack_msg;
	uint16_t txn;
	uint8_t  category;
	uint8_t  opcode;
};

static void cm_body(uint8_t *body, const struct cm_spec *s)
{
	vms_wire_buf_t w;

	memset(body, 0, PE_SEND_BODY_LEN);
	vms_wire_buf_init(&w, body, PE_SEND_BODY_LEN);
	vms_wire_put_le16(&w, 0u, (uint16_t)(VMS_CM_SCA_CONTENT - 44u));
	vms_wire_put_le16(&w, 2u, VMS_SCSCTRL_FMTWORD_CONST);
	vms_wire_put_le16(&w, 4u, 10u);            /* MTYPE 10 APPL_MSG   */
	vms_wire_put_le16(&w, 6u, 1u);             /* piggybacked credit  */
	vms_wire_put_le32(&w, BODY_CONID_REMOTE_OFF, s->remote_conid);
	vms_wire_put_le32(&w, BODY_CONID_LOCAL_OFF, s->local_conid);
	vms_wire_put_le16(&w, BODY_SYSAP_OFF + VMS_OFB_CM_SEND_MSG,
			  s->send_msg);
	vms_wire_put_le16(&w, BODY_SYSAP_OFF + VMS_OFB_CM_ACK_MSG, s->ack_msg);
	vms_wire_put_le16(&w, BODY_SYSAP_OFF + OFB_CM_TXN, s->txn);
	body[BODY_SYSAP_OFF + VMS_OFB_CM_CATEGORY] = s->category;
	body[BODY_SYSAP_OFF + VMS_OFB_CM_OPCODE] = s->opcode;
}

/* Offer one CM message to the port. Returns the port's verbatim answer. */
static int send_cm(struct guard_env *e, const struct cm_spec *s)
{
	uint8_t body[PE_SEND_BODY_LEN];

	cm_body(body, s);
	return pe_vc_send_msg(&e->fsm, VAX1_SYSID, (vms_conid_t)0, body,
			      PE_SEND_BODY_LEN);
}

/* A plain cat-0x01 op-0x02 config message: the ordinary shape of the dialogue. */
static struct cm_spec cm_ok(uint16_t send_msg, uint16_t ack_msg)
{
	struct cm_spec s;

	memset(&s, 0, sizeof(s));
	s.remote_conid = CONID_THEIRS;
	s.local_conid = CONID_OURS;
	s.send_msg = send_msg;
	s.ack_msg = ack_msg;
	s.txn = 0x1234u;
	s.category = VMS_CM_CAT_CONFIG;
	s.opcode = VMS_CM_OP_CONFIG;
	return s;
}

/* The PEER speaks: one real CM frame, in sequence, carrying its own
 * send-msg#. This is the ONLY thing that raises the peer high-water mark, and
 * therefore the only thing that makes this node entitled to acknowledge. */
static void rx_cm(struct guard_env *e, uint16_t seq, uint16_t recv_ack,
		  uint16_t peer_send_msg)
{
	uint8_t dst_lavc[6];
	vms_wire_buf_t w;
	uint32_t len;

	ovmx_lavc(dst_lavc);
	len = fake_peer_seqmsg(&e->peer, ovmx_hw, dst_lavc, seq, recv_ack,
			       CONID_OURS, CONID_THEIRS, e->buf,
			       sizeof(e->buf));
	if (len == 0)
		return;
	vms_wire_buf_init(&w, e->buf, len);
	vms_wire_put_le16(&w, VMS_OFF_CM_SEND_MSG, peer_send_msg);
	vms_wire_put_le16(&w, VMS_OFF_CM_ACK_MSG, recv_ack);
	e->buf[VMS_OFF_CM_CATEGORY] = VMS_CM_CAT_CONFIG;
	e->buf[VMS_OFF_CM_OPCODE] = VMS_CM_OP_CONFIG;
	rx_frame(e, len);
}

/* ================================================================== *
 * 1. THE GUARD NEVER BLOCKS CORRECT OUTPUT
 * ================================================================== */
static void test_golden_dialogue_is_never_refused(void)
{
	struct guard_env e;
	struct cm_spec s;
	struct pe_vc *vc;
	uint32_t i;

	printf("-- a golden CM dialogue produces ZERO refusals and ZERO warnings\n");
	open_circuit(&e);

	/* The peer speaks three times; this node answers each, acking exactly
	 * what really arrived, with a monotone send-msg# of its own. */
	for (i = 1u; i <= 3u; i++) {
		rx_cm(&e, (uint16_t)i, (uint16_t)(i - 1u), (uint16_t)i);
		s = cm_ok((uint16_t)i, (uint16_t)i);
		ct_check_eq_u32((unsigned long)send_cm(&e, &s),
				(unsigned long)PE_VC_SEND_OK,
				"an in-envelope CM message goes out");
	}

	/* ...and one COALESCED credit ack: cat 0x04 advancing the ack-msg# by
	 * 3, which is the smallest advance any real VMS node has ever made
	 * (MEASURED 6549/6549). */
	rx_cm(&e, 4u, 3u, 4u);
	rx_cm(&e, 5u, 3u, 5u);
	rx_cm(&e, 6u, 3u, 6u);
	s = cm_ok(4u, 6u);
	s.category = VMS_CM_CAT_ACK;
	s.opcode = 0u;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"a coalesced cat-04 ack goes out");
	s = cm_ok(5u, 6u);
	s.category = VMS_CM_CAT_ACK;
	s.opcode = 0u;
	rx_cm(&e, 7u, 3u, 9u);
	s.ack_msg = 9u;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"...and so does the next one, three ahead");

	ct_check_eq_u32(e.fsm.guard_judged, 5u,
			"the guard judged every CM frame this node emitted");
	ct_check_eq_u32(e.fsm.guard_refused, 0u,
			"NOTHING correct was refused");
	ct_check_eq_u32(e.fsm.guard_warned, 0u,
			"...and nothing correct was even warned about");
	ct_check_eq_u32(e.fake.logs, 0u,
			"the console stayed silent");
	ct_check_eq_u32(fake_vc_count(&e.fake, FAKE_VC_SEQ), 5u,
			"all five frames really reached the interface");

	vc = the_vc(&e);
	ct_check(vc != NULL, "the circuit is still there");
	ct_check_eq_u32(vc->send_seq, 6u,
			"the circuit consumed exactly five sequences");
	{
		struct fake_vc_decoded d = fake_vc_last(&e.fake, FAKE_VC_SEQ);

		ct_check(d.ok, "the last frame decodes as a sequenced message");
		ct_check_eq_u32(d.send_seq, 5u,
				"...at the fifth sequence, untouched by the guard");
	}
}

/* A send whose SYSAP body is NOT a connection-manager body is not this
 * guard's business, and it says so by counting rather than by judging. */
static void test_non_cm_traffic_is_not_judged(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- a non-CM SYSAP body is COUNTED, never judged\n");
	open_circuit(&e);
	s = cm_ok(1u, 0u);
	s.category = 0x33u;          /* no grounded VMS$VAXcluster category */
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK, "it goes out");
	ct_check_eq_u32(e.fsm.guard_judged, 0u, "the guard judged nothing");
	ct_check_eq_u32(e.fsm.guard_skipped, 1u, "...and said so");
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "nothing was refused");
}

/* ================================================================== *
 * 2. THE OBSERVED CNXMGRERR (E76): an ack for a message never sent
 * ================================================================== */
static void test_e76_unbacked_ack_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;
	struct pe_vc *vc;
	struct pe_vc_send_refusal r;

	printf("-- E76 CNXMGRERR: an ack the peer never earned is REFUSED\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 1u);
	rx_cm(&e, 2u, 0u, 2u);
	fake_pe_clear_frames(&e.fake);
	vc = the_vc(&e);

	/* E76 itself: the counter had been advanced by sends that never left,
	 * so the envelope acked 8 when the peer had sent 2. */
	s = cm_ok(8u, 8u);
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"the port refuses it as UNSAFE");
	ct_check_eq_u32(e.fake.n_frames, 0u,
			"NOTHING reached the interface");
	ct_check_eq_u32(vc->send_seq, 1u,
			"no sequence was consumed");
	ct_check_eq_u32(vc->unacked, 0u,
			"no unacked-ring slot was taken");
	ct_check_eq_u32(e.fsm.guard_refused, 1u, "the refusal is counted");
	ct_check_eq_u32(vc->guard.last_class,
			(unsigned long)CM_GUARD_C_ACK_UNBACKED,
			"and named: ack-unbacked");

	/* The refusal reads back through the E70 port readback, so the many-
	 * to-one SS$_ status a SYSAP is handed is not the end of the story. */
	memset(&r, 0, sizeof(r));
	ct_check_eq_u32((unsigned long)pe_vc_send_refusal_get(&e.fsm,
							      VAX1_SYSID, &r),
			0u, "the port answers the readback");
	ct_check_eq_u32((unsigned)-r.code, (unsigned)-PE_VC_SEND_UNSAFE,
			"...with the guard's own refusal code");
	ct_check_eq_u32(r.guard_class, (unsigned long)CM_GUARD_C_ACK_UNBACKED,
			"...and the vector that caused it");
	ct_check_eq_u32(r.guard_refused, 1u, "...and this circuit's count");

	/* And the console said so, ONCE. */
	ct_check_eq_u32(e.fake.logs, 1u, "exactly one console line");
	ct_check(strstr(e.fake.last_log, "refused to emit") != NULL,
		 "the line says the frame was REFUSED");
	ct_check(strstr(e.fake.last_log, "ack-unbacked") != NULL,
		 "...names the vector");
	ct_check(strstr(e.fake.last_log, "VAX1") != NULL,
		 "...and names the peer, from its own learned SCSNODE");
}

/* The other half of INV-6, and the reason this cannot false-drop: a peer that
 * has said NOTHING is not judged at all. */
static void test_ack_not_judged_before_the_peer_speaks(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- an unheard peer is NOT judged (honest omission, never a drop)\n");
	open_circuit(&e);
	s = cm_ok(1u, 9u);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"the frame goes: this node cannot judge what it never watched");
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "nothing was refused");
	ct_check_eq_u32(e.fsm.guard_judged, 1u, "...but the frame WAS judged");
}

/* The corpus's own +1 tolerance is kept exactly, so a frame the peer really
 * sent but this port has not delivered yet is never the reason a join fails. */
static void test_ack_slack_is_exactly_the_measured_one(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- the ack tolerance is the MEASURED +1, no more and no less\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 2u);

	s = cm_ok(1u, 3u);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"peer_high + 1 is inside the envelope");
	s = cm_ok(2u, 4u);
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"peer_high + 2 is not");
}

/* ================================================================== *
 * 3. THE OBSERVED INVEXCEPTN (E78): a per-frame ack burst
 * ================================================================== */
static void test_e78_ack_burst_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;
	struct pe_vc *vc;

	printf("-- E78 INVEXCEPTN: a per-frame cat-04 ack burst is REFUSED\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 40u);
	fake_pe_clear_frames(&e.fake);

	s = cm_ok(1u, 10u);
	s.category = VMS_CM_CAT_ACK;
	s.opcode = 0u;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK, "the first ack goes");

	/* ...and now the burst: one ack per received frame, advancing by 1.
	 * MEASURED 0 of 6549 real acks do this. */
	s = cm_ok(2u, 11u);
	s.category = VMS_CM_CAT_ACK;
	s.opcode = 0u;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"an advance of 1 is refused");
	s.ack_msg = 12u;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"...and so is an advance of 2");

	/* The COALESCED ack -- the thing a real node emits -- still goes. */
	s.ack_msg = 13u;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"an advance of 3 is what real VMS does, and it goes");

	vc = the_vc(&e);
	ct_check_eq_u32(vc->guard.refused, 2u, "both burst frames were dropped");
	ct_check_eq_u32(fake_vc_count(&e.fake, FAKE_VC_SEQ), 2u,
			"only the two coalesced acks reached the interface");
}

/* ================================================================== *
 * 4. The addressing, framing and transaction classes
 * ================================================================== */
static void test_zero_conid_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- N3: a CM frame with a zero Con.ID is REFUSED\n");
	open_circuit(&e);

	s = cm_ok(1u, 0u);
	s.local_conid = 0u;          /* spec SS4(O.26): OVMX did this after a
				      * VC break */
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"a zero LOCAL Con.ID is refused");
	s = cm_ok(1u, 0u);
	s.remote_conid = 0u;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"...and so is a zero REMOTE Con.ID");
	ct_check_eq_u32(e.fake.n_frames, 0u, "neither reached the interface");
	ct_check_eq_u32(the_vc(&e)->guard.last_class,
			(unsigned long)CM_GUARD_C_CONID_ZERO, "named conid-zero");
}

static void test_answered_notification_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- S8: answering a notification (op 0x0a / 0x0c) is REFUSED\n");
	open_circuit(&e);

	s = cm_ok(1u, 0u);
	s.category = (uint8_t)(VMS_CM_CAT_CONFIG | 0x80u);
	s.opcode = VMS_CM_OP_XITION_GO;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"a response to the barrier GO is refused");
	s.opcode = VMS_CM_OP_BARRIER_REL;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"...and to the step release");

	/* The notifications THEMSELVES are ordinary requests and still go. */
	s = cm_ok(1u, 0u);
	s.opcode = VMS_CM_OP_XITION_GO;
	s.txn = 0u;                  /* SS4(p): a notification carries txn 0 */
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"originating one is not answering one");
}

static void test_response_without_transaction_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- S9: a response carrying txn 0 is REFUSED\n");
	open_circuit(&e);

	s = cm_ok(1u, 0u);
	s.category = (uint8_t)(VMS_CM_CAT_CONFIG | 0x80u);
	s.opcode = VMS_CM_OP_COMMIT;
	s.txn = 0u;
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"a response the peer cannot look up is refused");
	s.txn = 0x4321u;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"...and the same response WITH a transaction goes");
}

/*
 * S12. The peer's grant is a byte of its own START body; the outstanding count
 * is this circuit's real send_seq minus the peer's real cumulative ack. Both
 * are executive reads -- the test moves the CIRCUIT, never the guard.
 */
static void test_credit_oversend_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;
	struct pe_vc *vc;

	printf("-- N2: sending past the peer's advertised grant is REFUSED\n");
	open_circuit(&e);
	vc = the_vc(&e);
	ct_check_eq_u32(vc->send_credit_max, LAB_CREDITS,
			"the grant came off the peer's own abs-95 byte");

	/* The peer acknowledged 1, so an outstanding count past its grant is
	 * an over-send however much local window the ledger happens to show.
	 * (The port's own credit gate is stepped over deliberately: the guard
	 * is a BACKSTOP, and this proves it still catches the over-send when
	 * the ledger above it has gone wrong.) */
	vc->peer_recv_ack = 1u;
	vc->send_seq = (uint16_t)(1u + LAB_CREDITS + 1u);
	vc->send_credit = LAB_CREDITS;

	s = cm_ok(1u, 0u);
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE,
			"eleven outstanding against a grant of ten is refused");
	ct_check_eq_u32(vc->guard.last_class,
			(unsigned long)CM_GUARD_C_CREDIT_OVERSEND,
			"named credit-oversend");

	vc->send_seq = (uint16_t)(1u + LAB_CREDITS);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"...and exactly ten outstanding is inside the grant");
}

/* S11. The class is a FIXED 204 bytes (MEASURED 306670/306670). A frame that
 * claims the class and is not that long is refused through the frame-level
 * entry, which is where a length can disagree with the class at all. */
static void test_short_cm_frame_is_refused(void)
{
	struct guard_env e;
	struct cm_spec s;
	uint8_t body[PE_SEND_BODY_LEN];
	uint8_t frame[PE_VC_FRAME_MAX];
	struct vms_scs_seq_envelope env;
	struct vms_frame_info fi;
	uint32_t total = PE_SEND_BODY_OFF + PE_SEND_BODY_LEN;
	int rc;

	printf("-- S11: a CM-class frame that is not 204 bytes is REFUSED\n");
	open_circuit(&e);

	s = cm_ok(1u, 0u);
	cm_body(body, &s);
	memset(&env, 0, sizeof(env));
	ct_check_eq_u32((unsigned long)pe_vc_addr(&e.fsm, VAX1_SYSID,
						  &env.addr), 0u,
			"the circuit supplied its own addressing");
	env.msgtype = VMS_SCS_MT_MSG;
	memset(frame, 0, sizeof(frame));
	ct_check_eq_u32((unsigned long)vms_scs_seq_envelope_build(
				&env, frame, (uint32_t)sizeof(frame), NULL),
			VMS_CODEC_OK, "the envelope built");
	memcpy(frame + PE_SEND_BODY_OFF, body, PE_SEND_BODY_LEN);
	/* The SCA length field still ASSERTS the 190-content class... */
	ct_check_eq_u32((unsigned long)vms_scs_seq_envelope_fixup_len(
				frame, (uint32_t)sizeof(frame), total),
			VMS_CODEC_OK, "...and the length field says 190");

	/* ...but the frame handed down is one byte short of it, so the class
	 * and the length disagree -- which is exactly the shape whose BODY the
	 * decode would refuse, and therefore the shape the guard has to judge
	 * before it decodes anything. */
	ct_check_eq_u32((unsigned long)vms_frame_classify(frame, total - 1u,
							  &fi), VMS_CODEC_OK,
			"the short frame still classifies");
	ct_check_eq_u32(fi.cls, (unsigned long)VMS_FCLS_SCS_MSG,
			"...and still ASSERTS the 190-content CM class");
	rc = pe_vc_send_frame(&e.fsm, VAX1_SYSID, frame, total - 1u);
	ct_check_eq_u32((unsigned)-rc, (unsigned)-PE_VC_SEND_UNSAFE,
			"a short CM-class frame is refused");
	ct_check_eq_u32(the_vc(&e)->guard.last_class,
			(unsigned long)CM_GUARD_C_FRAME_SIZE,
			"named frame-size");
	ct_check_eq_u32(e.fake.n_frames, 0u, "nothing reached the interface");
}

/* ================================================================== *
 * 5. The WARN half: counted and announced, but the frame still goes
 * ================================================================== */
static void test_envelope_jump_warns_but_does_not_drop(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- S1 is a WARN: a jumped dialogue open is reported, not dropped\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 1u);
	s = cm_ok(1u, 1u);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK, "the first frame goes");
	fake_pe_clear_frames(&e.fake);

	/* A DIFFERENT Con.ID pair, opened at a number that neither restarts at
	 * 1 nor continues from the 1 already sent. The auditor's own
	 * re-measurement found real VMS nodes doing something close enough
	 * that this may NOT be a drop. */
	s = cm_ok(7u, 1u);
	s.local_conid = CONID_OURS2;
	s.remote_conid = CONID_THEIRS2;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"the frame still goes out");
	ct_check_eq_u32(e.fsm.guard_warned, 1u, "...and the jump is counted");
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "nothing was dropped");
	ct_check_eq_u32(fake_vc_count(&e.fake, FAKE_VC_SEQ), 1u,
			"the frame really reached the interface");
	ct_check(strstr(e.fake.last_log, "outside the measured envelope") != NULL,
		 "the console line does NOT claim the frame was refused");
	ct_check(strstr(e.fake.last_log, "envelope-jump") != NULL,
		 "...and names the vector");
}

/*
 * E77's CONNECTION REBIND, and the false positive it would have caused.
 *
 * cnxman_csb_bind_connection() RESTARTS this node's send-msg# at zero when it
 * adopts a different connection for a system, so the first frame on the new
 * Con.ID pair is legitimately 1 and the next is 2 -- both far below the
 * node-level high-water mark the previous dialogue left behind. Measuring "did
 * the counter advance" against that mark would report every frame of a
 * perfectly healthy re-opened dialogue, so the advance rule is stated against
 * THIS DIALOGUE's own last number. This test is that distinction.
 */
static void test_a_rebound_dialogue_restarting_at_one_is_clean(void)
{
	struct guard_env e;
	struct cm_spec s;
	uint32_t i;

	printf("-- an E77 rebind that RESTARTS at 1 produces no finding at all\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 1u);
	for (i = 1u; i <= 4u; i++) {
		s = cm_ok((uint16_t)i, 1u);
		ct_check_eq_u32((unsigned long)send_cm(&e, &s),
				(unsigned long)PE_VC_SEND_OK,
				"the first dialogue runs 1,2,3,4");
	}

	/* The connection is rebound; the CSB's counters start again at zero, so
	 * this node's next origination on the new pair carries 1. */
	for (i = 1u; i <= 3u; i++) {
		s = cm_ok((uint16_t)i, 1u);
		s.local_conid = CONID_OURS2;
		s.remote_conid = CONID_THEIRS2;
		ct_check_eq_u32((unsigned long)send_cm(&e, &s),
				(unsigned long)PE_VC_SEND_OK,
				"...and the rebound one runs 1,2,3");
	}
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "nothing was refused");
	ct_check_eq_u32(e.fsm.guard_warned, 0u,
			"...and nothing was even warned about");
	ct_check_eq_u32(e.fake.logs, 0u, "the console stayed silent");
}

/* The mid-dialogue half of spec SS4(j)'s "strictly monotonic per sender": a
 * repeat is outside the envelope and is REPORTED -- at WARN, because the
 * auditor does not judge the mid-dialogue case and this guard refuses only
 * what that calibration covers. */
static void test_a_stalled_send_msg_warns(void)
{
	struct guard_env e;
	struct cm_spec s;

	printf("-- a send-msg# that does not advance is a WARN, not a drop\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 1u);
	s = cm_ok(4u, 1u);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK, "the first frame goes");
	s = cm_ok(4u, 1u);            /* the SAME number, again */
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"...and so does the repeat");
	ct_check_eq_u32(e.fsm.guard_warned, 1u, "but it is reported");
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "and nothing was dropped");
	ct_check(strstr(e.fake.last_log, "envelope-stall") != NULL,
		 "the console names the vector");
}

/* ================================================================== *
 * 6. The two properties that keep the guard honest
 * ================================================================== */

/*
 * E76's ROOT CAUSE, guarded against in the guard itself: a counter advanced by
 * a send that never left the node. The ledger moves only when the substrate
 * really took the bytes.
 */
static void test_ledger_does_not_advance_on_a_failed_send(void)
{
	struct guard_env e;
	struct cm_spec s;
	struct pe_vc *vc;

	printf("-- the ledger advances ONLY on a send that really left\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 5u);
	vc = the_vc(&e);

	s = cm_ok(1u, 1u);
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK, "one frame really went");
	ct_check_eq_u32(vc->guard.own_high_send_msg, 1u,
			"the high-water mark is the 1 that left");

	/* Now the interface refuses. The frame is HELD for the retransmit
	 * ladder (SS3b(1)) and has NOT been transmitted, so the ledger must
	 * not move. */
	e.fake.send_fails = 1;
	s = cm_ok(2u, 1u);
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_TXFAIL, "the interface refused it");
	ct_check_eq_u32(vc->guard.own_high_send_msg, 1u,
			"the high-water mark did NOT advance to 2");

	/* ...and neither does a GUARD refusal. */
	e.fake.send_fails = 0;
	s = cm_ok(3u, 99u);
	ct_check_eq_u32((unsigned)-send_cm(&e, &s),
			(unsigned)-PE_VC_SEND_UNSAFE, "the guard refused it");
	ct_check_eq_u32(vc->guard.own_high_send_msg, 1u,
			"...and the mark is still the 1 that really left");
}

/* The guard must never become the flood it exists to stop. */
static void test_the_console_is_throttled(void)
{
	struct guard_env e;
	struct cm_spec s;
	uint32_t i;

	printf("-- the guard's own console output is THROTTLED\n");
	open_circuit(&e);
	rx_cm(&e, 1u, 0u, 1u);

	for (i = 0u; i < 200u; i++) {
		s = cm_ok(1u, 50u);          /* the same vector, every time */
		(void)send_cm(&e, &s);
	}
	ct_check_eq_u32(e.fsm.guard_refused, 200u, "every one was refused");
	ct_check_eq_u32(e.fake.logs, 1u,
			"but the console got exactly ONE line");

	/* A DIFFERENT vector still announces itself immediately: the first
	 * sighting of a class is the diagnosis and is never throttled away. */
	s = cm_ok(1u, 1u);
	s.local_conid = 0u;
	(void)send_cm(&e, &s);
	ct_check_eq_u32(e.fake.logs, 2u, "a new vector prints at once");
	ct_check(strstr(e.fake.last_log, "conid-zero") != NULL,
		 "...and names itself");

	/* Past the interval the repeat may print again -- once. */
	e.fake.now_ms += CM_GUARD_LOG_INTERVAL_MS + 1u;
	for (i = 0u; i < 50u; i++) {
		s = cm_ok(1u, 50u);
		(void)send_cm(&e, &s);
	}
	ct_check_eq_u32(e.fake.logs, 3u,
			"one more line for the whole next interval");
}

/* ================================================================== *
 * 7. The vector names, executive vs the image that prints them
 * ================================================================== */
static void test_guard_class_names_do_not_drift(void)
{
	uint32_t i;
	uint32_t n = (uint32_t)(sizeof(cnxtrace_guard_class_names) /
				sizeof(cnxtrace_guard_class_names[0]));

	printf("-- the vector names match the userland ring dumper's copy\n");
	ct_check_eq_u32(n, (unsigned long)CM_GUARD_C__COUNT,
			"the dumper's table has one entry per vector");
	for (i = 0u; i < n && i < (uint32_t)CM_GUARD_C__COUNT; i++)
		ct_check(strcmp(cm_guard_class_name((uint8_t)i),
				cnxtrace_name(cnxtrace_guard_class_names,
					      (unsigned)n, i)) == 0,
			 "the executive's name and the dumper's agree");
	/* ...and the dumper's own out-of-range answer, which is what it would
	 * print for a vector this build does not know. */
	ct_check(strcmp(cnxtrace_name(cnxtrace_guard_class_names, (unsigned)n,
				      n + 5u), "?") == 0,
		 "an unknown ordinal renders as ? and never as a neighbour");
}

/*
 * E81 -- THE RE-ISSUE THE FIX PRODUCES IS GENUINELY SAFE, not merely caught.
 *
 * When the p. 7-30 reconnect ladder rebinds a system to a fresh Con.ID, the CSB
 * restarts BOTH dialogue cells (cnxman_csb_bind_connection), so the first body
 * this node originates on it carries send-msg# 1 and ack 0. That is the shape
 * asserted here, at the point every sequenced frame really passes: it goes out,
 * and the guard finds NOTHING -- no refusal and no warning. A fix whose output
 * only survived because the guard tolerated it would not be a fix.
 *
 * WHAT THIS TEST DELIBERATELY DOES NOT ASSERT. It does not claim the guard would
 * catch a re-issue that carried the OLD dialogue's ack onto the new Con.ID pair.
 * It would not, and must not: this guard's high-water mark is per CIRCUIT, not
 * per Con.ID pair, because the reference corpus measures real VMS nodes opening
 * a new pair and CONTINUING their counters across it (see `struct cm_guard`).
 * The per-connection rule is the CSB's to keep, and test_cnxman_csb.c keeps it;
 * what the guard owns, and what the arm below proves it still owns, is acking
 * above what the peer has really said ON THIS CIRCUIT.
 */
static void test_e81_reconnect_reissue_is_clean_and_the_regression_is_not(void)
{
	struct guard_env e;
	struct cm_spec s;
	uint32_t i;

	printf("-- E81: a rebound re-issue at send 1 / ack 0 is clean; an ack "
	       "above the circuit's real high is refused\n");
	open_circuit(&e);

	/* The dialogue on the connection that later closes: the peer speaks
	 * five times and this node answers, acking what really arrived. */
	for (i = 1u; i <= 5u; i++) {
		rx_cm(&e, (uint16_t)i, (uint16_t)(i - 1u), (uint16_t)i);
		s = cm_ok((uint16_t)i, (uint16_t)i);
		ct_check_eq_u32((unsigned long)send_cm(&e, &s),
				(unsigned long)PE_VC_SEND_OK,
				"the pre-reconnect dialogue runs clean");
	}
	ct_check_eq_u32(e.fsm.guard_refused, 0u, "nothing refused so far");

	/* The ladder reconnects: a NEW Con.ID pair, and the CSB's restarted
	 * cells put send-msg# 1 / ack 0 in the envelope. */
	s = cm_ok(1u, 0u);
	s.local_conid = CONID_OURS2;
	s.remote_conid = CONID_THEIRS2;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_OK,
			"the E81 re-issue goes out");
	ct_check_eq_u32(e.fsm.guard_refused, 0u,
			"guard_refused is 0 -- the corrected re-issue is SAFE, "
			"not tolerated");
	ct_check_eq_u32(e.fsm.guard_warned, 0u, "and nothing was warned about");

	/* The half the guard does own: an ack above what the peer has really
	 * sent on this circuit is still refused outright. */
	s = cm_ok(2u, 99u);
	s.local_conid = CONID_OURS2;
	s.remote_conid = CONID_THEIRS2;
	ct_check_eq_u32((unsigned long)send_cm(&e, &s),
			(unsigned long)PE_VC_SEND_UNSAFE,
			"an unbacked ack is REFUSED, on the new pair as on the old");
	ct_check_eq_u32(e.fsm.guard_refused, 1u, "and counted exactly once");
	ct_check(strstr(e.fake.last_log, "ack-unbacked") != NULL,
		 "named as the S2 vector");
}

int main(void)
{
	test_golden_dialogue_is_never_refused();
	test_non_cm_traffic_is_not_judged();
	test_e76_unbacked_ack_is_refused();
	test_ack_not_judged_before_the_peer_speaks();
	test_ack_slack_is_exactly_the_measured_one();
	test_e78_ack_burst_is_refused();
	test_zero_conid_is_refused();
	test_answered_notification_is_refused();
	test_response_without_transaction_is_refused();
	test_credit_oversend_is_refused();
	test_short_cm_frame_is_refused();
	test_envelope_jump_warns_but_does_not_drop();
	test_a_rebound_dialogue_restarting_at_one_is_clean();
	test_e81_reconnect_reissue_is_clean_and_the_regression_is_not();
	test_a_stalled_send_msg_warns();
	test_ledger_does_not_advance_on_a_failed_send();
	test_the_console_is_throttled();
	test_guard_class_names_do_not_drift();
	return ct_summary("test_pe_emit_guard");
}
