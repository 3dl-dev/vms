/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_send.c - FC-P1.3 R1: the port's body-level send services
 * (`pe_vc_send_msg`/`pe_vc_send_dg`, vms_pe_fsm.h SS8c) against a FAKE UPPER
 * LAYER, exactly as the plan's done-condition asks.
 *
 * Builds on FC-P1.2's landed VC FSM (pe_fake_vc.h's helpers, `drive_vc_to`
 * pattern from test_pe_vc.c) and proves the E9 bridge: a caller-built
 * "body" (SCS's own 56-71 header plus the 132-byte SYSAP body, exactly as
 * `scs_send_msg` will one day hand it down) goes out through the circuit
 * with the RIGHT sequence, a received sequenced message reaches the fake
 * upper layer's (SB, Con.ID) callback with the right payload, and the
 * datagram path sends unsequenced and unringed.
 *
 * Every stimulus and every assertion goes through the codec, never a raw
 * offset (design SS3.9 rule 2; this file is not the codec).
 */

#include <string.h>

#include "cluster_test.h"
#include "pe_fake_vc.h"
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN/VMS_CM_SCA_CONTENT: a
				     * realistic 190-class body shape for this
				     * test's fixtures only -- the port itself
				     * never includes this header */
#include "vms_cluster_codec_scs.h"  /* VMS_SCSCTRL_FMTWORD_CONST, same reason */

#define OVMX_SYSID 1030u
#define VAX1_SYSID 1025u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

#define LAB_CREDITS 10u

/* body-relative offsets of the two fields this test needs to plant/read
 * inside the SCS-header span (abs 56-71) it hands to pe_vc_send_msg/dg --
 * named from the SAME codec.h constants the port itself would use, never a
 * literal (design SS3.9 rule 2). */
#define BODY_CONID_REMOTE_OFF (VMS_OFF_SCS_CONID_REMOTE - PE_SEND_BODY_OFF)
#define BODY_CONID_LOCAL_OFF  (VMS_OFF_SCS_CONID_LOCAL  - PE_SEND_BODY_OFF)
#define BODY_SYSAP_OFF        (VMS_OFF_SYSAP_BODY       - PE_SEND_BODY_OFF)

/* ------------------------------------------------------------------ *
 * The fake upper layer: records exactly what it was handed, including the
 * payload bytes, so "the right payload" is an assertion and not a hope.
 * ------------------------------------------------------------------ */
struct fake_upper {
	uint32_t       messages, datagrams;
	vms_scs_sysid_t last_from;
	vms_conid_t    last_conid;
	uint32_t       last_len;
	uint8_t        last_payload[VMS_CM_BODY_LEN + 32];
};

static void fu_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
		       const uint8_t *body, uint32_t len)
{
	struct fake_upper *u = (struct fake_upper *)ctx;

	u->messages++;
	u->last_from = from;
	u->last_conid = conid;
	u->last_len = len;
	if (len > sizeof(u->last_payload))
		len = (uint32_t)sizeof(u->last_payload);
	memcpy(u->last_payload, body, len);
}

static void fu_datagram(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			uint32_t len)
{
	struct fake_upper *u = (struct fake_upper *)ctx;

	(void)from; (void)body; (void)len;
	u->datagrams++;
}

static void fu_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)ctx; (void)peer;
}

static void fu_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	(void)ctx; (void)peer; (void)reason;
}

/* ------------------------------------------------------------------ *
 * The node under test: an OPEN circuit to VAX1, exactly the shape
 * test_pe_vc.c's drive_vc_to(..., VMS_PE_VC_OPEN) reaches, rebuilt here so
 * this file stands alone (test_pe_vc.c's helpers are file-local statics).
 * ------------------------------------------------------------------ */
struct send_env {
	struct pe_fsm       fsm;
	struct pe_ops       ops;
	struct fake_pe      fake;
	struct fake_peer    peer;
	struct fake_upper   upper_rec;
	struct pe_upper_ops upper;
	struct pe_vc        vcs[2];
	uint8_t             buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

#define OVMX_BOOT_TIME 0x00bc0552690a0000ull

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

static void env_init(struct send_env *e)
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

static void rx_frame(struct send_env *e, uint32_t len)
{
	if (len != 0)
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
}

static void rx_hello(struct send_env *e, uint8_t word, uint16_t incarnation)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_hello(&e->peer, ovmx_hw, dst_lavc, word,
				    incarnation, 0, e->buf, sizeof(e->buf)));
}

static void rx_start(struct send_env *e, uint16_t round, uint16_t send_seq,
		     uint16_t recv_ack)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_start(&e->peer, VAX1_SYSID, ovmx_hw, dst_lavc,
				    round, send_seq, recv_ack, LAB_CREDITS,
				    e->buf, sizeof(e->buf)));
}

static void rx_vc_ack(struct send_env *e)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_vc_ack(&e->peer, ovmx_hw, dst_lavc, 1, 0,
				     e->buf, sizeof(e->buf)));
}

/* b2 => b4 (the established-member-first-contact ladder test_pe_vc.c
 * documents) => CHANNEL_UP => START SENT, then the peer's START/ACK opens
 * the circuit -- the SAME path a real formation takes. */
static void open_circuit(struct send_env *e)
{
	env_init(e);
	rx_hello(e, PE_PFW_VERIFY_B2, 1);
	rx_hello(e, PE_PFW_VERIFY_B4, 1);
	rx_start(e, 0, 1, 0);
	rx_vc_ack(e);
	fake_pe_clear_frames(&e->fake);
	memset(&e->upper_rec, 0, sizeof(e->upper_rec));
}

static struct pe_vc *the_vc(struct send_env *e)
{
	return pe_fsm_vc_at(&e->fsm, 0);
}

/* A realistic PE_SEND_BODY_LEN body: a plausible SCS envelope (56-71, per
 * spec sec 4(d)/(1b): inner length 146, format 0x0004, MTYPE 10 APPL_MSG,
 * credit 1, the Con.ID pair) followed by a distinctive 132-byte SYSAP
 * payload -- exactly the shape `scs_send_msg` will build once FC-P2.2
 * lands. `fill` seeds the SYSAP span so two different bodies are
 * distinguishable in an assertion. */
static void build_body(uint8_t *body, uint32_t remote_conid,
		       uint32_t local_conid, uint8_t fill)
{
	vms_wire_buf_t w;
	uint32_t i;

	memset(body, 0, PE_SEND_BODY_LEN);
	vms_wire_buf_init(&w, body, PE_SEND_BODY_LEN);
	vms_wire_put_le16(&w, 0u, (uint16_t)(VMS_CM_SCA_CONTENT - 44u)); /* inner length, spec (1b) */
	vms_wire_put_le16(&w, 2u, VMS_SCSCTRL_FMTWORD_CONST);            /* abs 58 */
	vms_wire_put_le16(&w, 4u, 10u);                                  /* abs 60: MTYPE APPL_MSG */
	vms_wire_put_le16(&w, 6u, 1u);                                   /* abs 62: credit */
	vms_wire_put_le32(&w, BODY_CONID_REMOTE_OFF, remote_conid);
	vms_wire_put_le32(&w, BODY_CONID_LOCAL_OFF, local_conid);
	for (i = 0; i < VMS_CM_BODY_LEN; i++)
		body[BODY_SYSAP_OFF + i] = (uint8_t)(fill + i);
}

/* ------------------------------------------------------------------ *
 * pe_vc_send_msg: goes out on the VC with the right sequence
 * ------------------------------------------------------------------ */
static void test_send_msg_goes_out_with_right_sequence(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN];
	struct fake_vc_decoded d;
	struct pe_vc *vc;
	uint32_t remote_conid = 0x33580008u, local_conid = 0x62c50009u;
	int rc;

	printf("-- pe_vc_send_msg: goes out on the VC with the right sequence\n");
	open_circuit(&e);
	build_body(body, remote_conid, local_conid, 0xa5u);

	rc = pe_vc_send_msg(&e.fsm, VAX1_SYSID, (vms_conid_t)0xdeadbeefu, body,
			    PE_SEND_BODY_LEN);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_OK,
			"pe_vc_send_msg returns OK");
	ct_check_eq_u32(e.fake.n_frames, 1, "exactly one frame went out");

	d = fake_vc_last(&e.fake, FAKE_VC_SEQ);
	ct_check(d.ok, "the emitted frame classifies as a sequenced SCS message");
	ct_check_eq_u32(d.send_seq, 1, "send_seq is this circuit's first (1)");
	ct_check_eq_u32(d.msgtype, VMS_SCS_MT_MSG,
			"msgtype is the sequenced-application marker (0x4b)");

	vc = the_vc(&e);
	ct_check(vc != NULL, "the circuit still exists");
	ct_check_eq_u32(vc->send_seq, 2, "the circuit's next seq advanced to 2");
	ct_check_eq_u32(vc->unacked, 1,
			"the frame is held in the unacked ring for retransmit");

	/* Round-trip the Con.ID pair and the SYSAP payload straight off the
	 * wire the port actually built -- proving abs 56-71 and 72-203 are
	 * exactly what the caller's `body` carried. */
	{
		struct vms_frame_info fi;
		uint32_t rc32, lc32;
		uint32_t i;
		int payload_ok = 1;

		ct_check_eq_u32((unsigned long)vms_frame_classify(
					e.fake.frame[0].b, e.fake.frame[0].len,
					&fi), VMS_CODEC_OK,
				"the emitted frame reclassifies cleanly");
		ct_check_eq_u32((unsigned long)vms_scs_conid(
					e.fake.frame[0].b, e.fake.frame[0].len,
					&fi, &rc32, &lc32), VMS_CODEC_OK,
				"the Con.ID pair is readable");
		ct_check_eq_u32(rc32, remote_conid,
				"remote Con.ID is exactly what `body` carried");
		ct_check_eq_u32(lc32, local_conid,
				"local Con.ID is exactly what `body` carried");
		for (i = 0; i < VMS_CM_BODY_LEN; i++) {
			if (e.fake.frame[0].b[VMS_OFF_SYSAP_BODY + i] !=
			    (uint8_t)(0xa5u + i)) {
				payload_ok = 0;
				break;
			}
		}
		ct_check(payload_ok,
			"the 132-byte SYSAP payload round-trips byte-exact");
	}
}

static void test_send_msg_dst_conid_param_not_on_wire(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN];
	struct vms_frame_info fi;
	uint32_t rc32, lc32;
	int rc;

	printf("-- pe_vc_send_msg: dst_conid is bookkeeping, not a wire write\n");
	open_circuit(&e);
	build_body(body, 0x11112222u, 0x33334444u, 0x01u);

	/* A dst_conid that does NOT match anything in `body` -- if the port
	 * ever started writing it to the wire, this would corrupt the
	 * Con.ID pair `body` already carries (design SS3.2.4: SCS owns 56-71,
	 * the port never writes a Con.ID). */
	rc = pe_vc_send_msg(&e.fsm, VAX1_SYSID, (vms_conid_t)0x99999999u, body,
			    PE_SEND_BODY_LEN);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_OK,
			"send still succeeds");
	(void)vms_frame_classify(e.fake.frame[0].b, e.fake.frame[0].len, &fi);
	(void)vms_scs_conid(e.fake.frame[0].b, e.fake.frame[0].len, &fi, &rc32,
			    &lc32);
	ct_check_eq_u32(rc32, 0x11112222u,
			"the wire's remote Con.ID is body's, not dst_conid's");
	ct_check_eq_u32(lc32, 0x33334444u,
			"the wire's local Con.ID is body's, not dst_conid's");
}

static void test_send_msg_wrong_len_refused(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN + 1];
	int rc;

	printf("-- pe_vc_send_msg: a body of the wrong length is refused\n");
	open_circuit(&e);
	memset(body, 0, sizeof(body));

	rc = pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0, body, PE_SEND_BODY_LEN - 1u);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_BADFRAME,
			"too short is BADFRAME");
	rc = pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0, body, PE_SEND_BODY_LEN + 1u);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_BADFRAME,
			"too long is BADFRAME");
	ct_check_eq_u32(e.fake.n_frames, 0, "neither attempt transmitted");
}

static void test_send_msg_no_circuit_refused(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN];
	int rc;

	printf("-- pe_vc_send_msg: no circuit to the destination is refused\n");
	env_init(&e);   /* no formation at all: no circuit exists yet */
	build_body(body, 1, 2, 0);

	rc = pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0, body, PE_SEND_BODY_LEN);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_NOCIRCUIT,
			"NOCIRCUIT with nothing formed");
	ct_check_eq_u32(e.fake.n_frames, 0, "nothing transmitted");
}

/* ------------------------------------------------------------------ *
 * Receiving: the fake upper layer's (SB, Con.ID) callback fires right
 * ------------------------------------------------------------------ */
static void test_recv_message_reaches_upper_by_sb_and_conid(void)
{
	struct send_env e;
	uint32_t len;

	printf("-- receive: the (SB, Con.ID) callback fires with the right payload\n");
	open_circuit(&e);

	{
		uint8_t dst_lavc[6];

		ovmx_lavc(dst_lavc);
		len = fake_peer_seqmsg(&e.peer, ovmx_hw, dst_lavc, 1, 0,
				       0x62c50009u, 0x33580008u, e.buf,
				       sizeof(e.buf));
	}
	ct_check(len != 0, "the peer's sequenced message was built");
	rx_frame(&e, len);

	ct_check_eq_u32(e.upper_rec.messages, 1, "the upper's message() fired once");
	ct_check_eq_u32((unsigned long)e.upper_rec.last_from, VAX1_SYSID,
			"keyed by the sending SB (SCSSYSTEMID)");
	/* abs 64 ("Remote Connection ID" in the sender's own naming, spec
	 * sec 4(d)) is the DESTINATION's Con.ID as the peer addresses it --
	 * i.e. OUR Con.ID, which is exactly what vc_deliver hands up as
	 * dst_conid (vms_pe_fsm.c: rx->dst_conid = remote). */
	ct_check_eq_u32((unsigned long)e.upper_rec.last_conid, 0x62c50009u,
			"keyed by the frame's destination Con.ID (abs 64, the peer's addressing of US)");
	ct_check_eq_u32(e.upper_rec.datagrams, 0,
			"a sequenced message never fires the datagram callback");

	/* Transport ack is a fact independent of delivery (SS3b(a)): the
	 * frame is acknowledged even though this test's fake upper does not
	 * gate on anything. */
	ct_check_eq_u32(the_vc(&e)->recv_seq, 1, "recv_seq advanced to 1");
}

/* ------------------------------------------------------------------ *
 * pe_vc_send_dg: unsequenced, unacknowledged, never ringed
 * ------------------------------------------------------------------ */
static void test_send_dg_is_unsequenced_and_unringed(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN];
	struct fake_vc_decoded d;
	struct pe_vc *vc;
	int rc;

	printf("-- pe_vc_send_dg: unsequenced, unacknowledged, never ringed\n");
	open_circuit(&e);
	build_body(body, 0xaaaa0001u, 0xbbbb0002u, 0x40u);

	vc = the_vc(&e);
	ct_check_eq_u32(vc->send_seq, 1, "before: the circuit's seq is still 1");

	rc = pe_vc_send_dg(&e.fsm, VAX1_SYSID, body, PE_SEND_BODY_LEN);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_OK,
			"pe_vc_send_dg returns OK");
	ct_check_eq_u32(e.fake.n_frames, 1, "exactly one frame went out");

	d = fake_vc_last(&e.fake, FAKE_VC_ANY);
	ct_check(d.ok, "the emitted frame classifies");
	ct_check_eq_u32(d.send_seq, 0,
			"send_seq is 0: no ordered slot claimed (sec 4h(3)/(4))");

	ct_check_eq_u32(vc->send_seq, 1,
			"after: the circuit's seq did NOT advance");
	ct_check_eq_u32(vc->unacked, 0,
			"the datagram never entered the unacked ring");
	ct_check_eq_u32(vc->dg_tx, 1, "counted on the circuit's own dg_tx");
	ct_check_eq_u32(vc->msgs_tx, 0,
			"and NOT counted as a sequenced message");
}

static void test_send_dg_requires_open_circuit(void)
{
	struct send_env e;
	uint8_t body[PE_SEND_BODY_LEN];
	int rc;

	printf("-- pe_vc_send_dg: refuses without an OPEN circuit\n");
	env_init(&e);
	build_body(body, 1, 2, 0);

	rc = pe_vc_send_dg(&e.fsm, VAX1_SYSID, body, PE_SEND_BODY_LEN);
	ct_check_eq_u32((unsigned long)rc, (unsigned long)PE_VC_SEND_NOCIRCUIT,
			"NOCIRCUIT with nothing formed");
	ct_check_eq_u32(e.fake.n_frames, 0, "nothing transmitted");
}

/* ------------------------------------------------------------------ *
 * E70: the port keeps WHICH refusal it made, and the live state behind it
 *
 * THE WALL THIS CLOSES. pe_send_status() maps this port's five refusals onto
 * three SS$_ statuses -- NOCIRCUIT and RINGFULL both become SS$_DEVOFFLINE --
 * because Rule 8 forbids inventing the statuses OpenVMS uses for them. On a
 * live cluster "there is no circuit to that system" and "the unacked ring is
 * full" are different defects with different fixes, and the SYSAP above could
 * not tell them apart. pe_vc_send_refusal_get() reports what the PORT decided
 * plus the counters that explain it, every field read off a real pe_vc.
 * ------------------------------------------------------------------ */
static void test_send_refusal_names_the_port_reason(void)
{
	struct send_env e;
	struct pe_vc_send_refusal r;
	uint8_t body[PE_SEND_BODY_LEN];
	struct pe_vc *vc;
	uint32_t i;

	printf("-- E70: the port records WHICH refusal it made\n");

	/* 1. NO CIRCUIT OBJECT AT ALL: the absence is the answer, and there is
	 * nowhere to have recorded a code -- so none is invented. */
	env_init(&e);
	build_body(body, 1, 2, 0);
	ct_check_eq_u32((unsigned long)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0,
						      body, PE_SEND_BODY_LEN),
			(unsigned long)PE_VC_SEND_NOCIRCUIT,
			"a send with nothing formed is refused NOCIRCUIT");
	ct_check_eq_u32((unsigned long)pe_vc_send_refusal_get(&e.fsm,
							      VAX1_SYSID, &r),
			0u, "the readback answers");
	ct_check_eq_u32(r.vc_present, 0u,
			"... with vc_present 0: there IS no circuit object, "
			"which is the fact itself");
	ct_check_eq_u32((uint32_t)r.code, 0u,
			"... and no refusal code invented for a circuit that "
			"does not exist");

	/* 2. NO SEND CREDIT: the peer's window, spent. */
	open_circuit(&e);
	for (i = 0; i < LAB_CREDITS; i++) {
		build_body(body, 1, 2, (uint8_t)i);
		ct_check_eq_u32((unsigned long)pe_vc_send_msg(&e.fsm,
							      VAX1_SYSID, 0,
							      body,
							      PE_SEND_BODY_LEN),
				(unsigned long)PE_VC_SEND_OK,
				"a send inside the peer's grant goes");
	}
	ct_check_eq_u32((unsigned long)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0,
						      body, PE_SEND_BODY_LEN),
			(unsigned long)PE_VC_SEND_NOCREDIT,
			"the send past the grant is refused NOCREDIT");
	(void)pe_vc_send_refusal_get(&e.fsm, VAX1_SYSID, &r);
	ct_check_eq_u32(r.vc_present, 1u, "the circuit is there");
	ct_check_eq_u32((uint32_t)r.code, (uint32_t)PE_VC_SEND_NOCREDIT,
			"... and the PORT's own code says which refusal it "
			"was -- not the SS$_DEVOFFLINE it shares with "
			"NOCIRCUIT and RINGFULL");
	ct_check_eq_u32(r.send_refused_credit, 1u,
			"the credit-refusal counter is a REAL count");
	ct_check_eq_u32(r.send_refused_ring, 0u,
			"and the ring-refusal counter did NOT move: the two "
			"causes stay apart");
	ct_check_eq_u32(r.send_credit, 0u, "the live window is spent");
	ct_check_eq_u32(r.send_credit_max, LAB_CREDITS,
			"beside the PEER's own grant, so a reader sees how big "
			"the window was");

	/* 3. RING FULL: a different refusal that maps to the SAME SS$_ status
	 * as NOCIRCUIT. The window is widened by hand -- the lab peer grants
	 * fewer credits than the ring holds, so this refusal is unreachable
	 * otherwise, and it is exactly the one a bigger grant would hit. */
	vc = the_vc(&e);
	ct_check(vc != NULL, "the circuit is still there");
	vc->send_credit = (uint8_t)(PE_VC_UNACKED_MAX + 4u);
	for (i = (uint32_t)vc->unacked; i < PE_VC_UNACKED_MAX; i++) {
		build_body(body, 1, 2, (uint8_t)i);
		(void)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0, body,
				     PE_SEND_BODY_LEN);
	}
	ct_check_eq_u32(vc->unacked, PE_VC_UNACKED_MAX,
			"the unacked ring is full");
	ct_check_eq_u32((unsigned long)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0,
						      body, PE_SEND_BODY_LEN),
			(unsigned long)PE_VC_SEND_RINGFULL,
			"the next send is refused RINGFULL");
	(void)pe_vc_send_refusal_get(&e.fsm, VAX1_SYSID, &r);
	ct_check_eq_u32((uint32_t)r.code, (uint32_t)PE_VC_SEND_RINGFULL,
			"the port names RINGFULL, not the DEVOFFLINE it shares "
			"with NOCIRCUIT");
	ct_check_eq_u32(r.send_refused_ring, 1u, "counted, once");
	ct_check_eq_u32(r.send_refused_credit, 1u,
			"and the credit count from step 2 is untouched");
	ct_check_eq_u32(r.unacked, PE_VC_UNACKED_MAX,
			"the ring depth is reported LIVE beside it");

	/* 4. THE INTERFACE refused the frame: a third cause, a third code. */
	open_circuit(&e);
	e.fake.send_fails = 1;
	build_body(body, 1, 2, 0x5au);
	ct_check_eq_u32((unsigned long)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0,
						      body, PE_SEND_BODY_LEN),
			(unsigned long)PE_VC_SEND_TXFAIL,
			"exec_lan_xmit refusing is TXFAIL");
	(void)pe_vc_send_refusal_get(&e.fsm, VAX1_SYSID, &r);
	ct_check_eq_u32((uint32_t)r.code, (uint32_t)PE_VC_SEND_TXFAIL,
			"... and the port says so, distinctly");

	/* 5. AN UNSENDABLE BODY: the fifth cause. */
	open_circuit(&e);
	(void)pe_vc_send_msg(&e.fsm, VAX1_SYSID, 0, body,
			     PE_SEND_BODY_LEN - 1u);
	(void)pe_vc_send_refusal_get(&e.fsm, VAX1_SYSID, &r);
	ct_check_eq_u32((uint32_t)r.code, (uint32_t)PE_VC_SEND_BADFRAME,
			"a body of the wrong length is recorded as BADFRAME");

	/* AND A CIRCUIT THAT REFUSED NOTHING SAYS SO. */
	open_circuit(&e);
	(void)pe_vc_send_refusal_get(&e.fsm, VAX1_SYSID, &r);
	ct_check_eq_u32((uint32_t)r.code, 0u,
			"a fresh circuit reports NO refusal, which is not the "
			"same as a refusal of 0");
	ct_check_eq_u32(r.vc_state, (uint32_t)VMS_PE_VC_OPEN,
			"... and its LIVE state, so \"not sendable\" and \"the "
			"port refused\" can never be confused");
}

int main(void)
{
	test_send_msg_goes_out_with_right_sequence();
	test_send_msg_dst_conid_param_not_on_wire();
	test_send_msg_wrong_len_refused();
	test_send_msg_no_circuit_refused();
	test_recv_message_reaches_upper_by_sb_and_conid();
	test_send_dg_is_unsequenced_and_unringed();
	test_send_dg_requires_open_circuit();
	test_send_refusal_names_the_port_reason();
	return ct_summary("test_pe_send");
}
