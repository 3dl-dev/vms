/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_scs_fsm_credit.c - R1: the credit ledger (FC-P2.2).
 *
 * Two of the plan row's four test groups live here.
 *
 * 1. THE CONSERVATION PROPERTY. Credits are neither created nor destroyed
 *    across a message + credit-return cycle. Stated locally, per CDT, as
 *
 *        credit_receive + credit_held + credit_pending == credit_grant
 *
 *    and asserted after EVERY step of a long randomised cycle, not just at the
 *    ends -- an invariant that only holds at the endpoints is not an
 *    invariant. Stated across the pair, at quiescence, as
 *
 *        A.credit_send == B.credit_receive
 *
 *    which is p. 2-44's own definition: "local SCS maintains a Receive Credit
 *    count for the connection that is effectively a mirror image of remote
 *    SCS's Send Credit count."
 *
 * 2. THE 8-BEFORE-DISCONNECT INVARIANT. Spec SS4(h)(1f), each figure N-of-N
 *    over the reference library: a dialogue that disconnects carries a type 8
 *    (131/131), no dialogue disconnects without one (0/131), the type-8 sender
 *    is the first DISCONNECT_REQ sender (131/131), and the only frame between
 *    the 8 and the op 6 is the 9 (131/131). The tests below assert the same
 *    four facts about what THIS FSM puts on the wire.
 *
 * Every credit figure checked here is read back OFF THE BUILT FRAME through
 * the codec (scsh_record_ctrl), so what is asserted is what the wire would
 * carry -- not what the caller passed in. That is the point of the item:
 * INV-6, a credit count is a ledger read.
 */

#include "cluster_test.h"
#include "scs_test_harness.h"

static struct scsh_node a_node;
static struct scsh_node b_node;
static struct scsh_sysap a_sysap;
static struct scsh_sysap b_sysap;

static void rig(vms_conid_t *a_conid, vms_conid_t *b_conid,
		uint16_t a_grant, uint16_t b_grant)
{
	scsh_wire_reset();
	scsh_node_init(&a_node, 0x0101u, 0x3358u);
	scsh_node_init(&b_node, 0x0202u, 0xe995u);
	scsh_sysap_init(&a_sysap, &a_node);
	scsh_sysap_init(&b_sysap, &b_node);
	b_sysap.connect_decision = 0;
	(void)scs_fsm_listen(&b_node.fsm, scsh_name_b, &b_sysap.ops, b_grant);
	scsh_link(&a_node, &b_node);
	(void)scsh_open_pair(&a_node, &b_node, a_grant, a_conid);
	*b_conid = b_sysap.last_opened_conid;
}

static void fill_body(uint8_t *b, uint8_t v)
{
	uint32_t i;

	for (i = 0; i < SCS_SYSAP_BODY_LEN; i++)
		b[i] = v;
}

/* ------------------------------------------------------------------ *
 * 1. The grant seeds the ledger, and the wire carries the ledger value
 * ------------------------------------------------------------------ */
static void t_grant_is_a_ledger_read(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca, *cb;

	printf("-- the initial grant: ledger, wire and peer agree\n");
	rig(&a_conid, &b_conid, 6u, 10u);
	ca = scsh_cdt(&a_node, a_conid);
	cb = scsh_cdt(&b_node, b_conid);

	ct_check_eq_u32(a_node.tx_credit[0], 6u,
			"A's op 0 carried A's OWN grant (6)");
	ct_check_eq_u32(b_node.tx_credit[1], 10u,
			"B's op 2 carried B's OWN grant (10)");
	ct_check_eq_u32(ca->credit_grant, 6u, "A extended 6");
	ct_check_eq_u32(ca->credit_receive, 6u, "A's Receive Credit is 6");
	ct_check_eq_u32(ca->credit_send, 10u, "A may send 10: B granted them");
	ct_check_eq_u32(cb->credit_send, 6u, "B may send 6: A granted them");
	ct_check(scsh_ledger_balanced(ca), "A's ledger balances at connect");
	ct_check(scsh_ledger_balanced(cb), "B's ledger balances at connect");
	ct_check_eq_u32(ca->credit_send, cb->credit_receive,
			"A.send == B.receive (p. 2-44's mirror image)");
	ct_check_eq_u32(cb->credit_send, ca->credit_receive,
			"and the other way round");
}

/* ------------------------------------------------------------------ *
 * 2. CONSERVATION across a long message + credit-return cycle
 * ------------------------------------------------------------------ */
static int step_send(struct scsh_node *from, vms_conid_t conid, uint8_t v)
{
	uint8_t body[SCS_SYSAP_BODY_LEN];

	fill_body(body, v);
	return scs_fsm_send_msg(&from->fsm, conid, body, SCS_SYSAP_BODY_LEN);
}

static void t_conservation_property(void)
{
	vms_conid_t a_conid, b_conid;
	uint32_t round;
	int unbalanced = 0;
	int created = 0;
	uint32_t a_grant, b_grant;

	printf("-- conservation: credits are neither created nor destroyed\n");
	rig(&a_conid, &b_conid, 6u, 10u);
	/* The SYSAPs release each buffer as they take it, which is what makes
	 * the pending count move and the piggyback carry a varying value. */
	a_sysap.return_credit_immediately = 1u;
	b_sysap.return_credit_immediately = 1u;

	a_grant = scsh_cdt(&a_node, a_conid)->credit_grant;
	b_grant = scsh_cdt(&b_node, b_conid)->credit_grant;

	for (round = 0; round < 40u; round++) {
		struct scs_cdt *ca, *cb;

		/* A pseudo-random but reproducible mix of both directions and
		 * of send-then-drain versus drain-then-send. */
		if ((round % 3u) != 2u)
			(void)step_send(&a_node, a_conid, (uint8_t)round);
		if ((round % 4u) != 3u)
			(void)step_send(&b_node, b_conid, (uint8_t)(round ^ 0x5au));
		(void)scsh_pump();

		ca = scsh_cdt(&a_node, a_conid);
		cb = scsh_cdt(&b_node, b_conid);
		if (ca == (struct scs_cdt *)0 || cb == (struct scs_cdt *)0) {
			unbalanced = 1;
			break;
		}
		if (!scsh_ledger_balanced(ca) || !scsh_ledger_balanced(cb))
			unbalanced = 1;
		if (ca->credit_grant != a_grant || cb->credit_grant != b_grant)
			created = 1;
		/* Nothing may ever hold more than was extended. */
		if (ca->credit_send > b_grant || cb->credit_send > a_grant)
			created = 1;
	}

	ct_check(!unbalanced,
		 "receive + held + pending == grant, after EVERY step");
	ct_check(!created,
		 "no credit was created: send never exceeds the peer's grant");

	{
		struct scs_cdt *ca = scsh_cdt(&a_node, a_conid);
		struct scs_cdt *cb = scsh_cdt(&b_node, b_conid);

		ct_check_eq_u32(ca->credit_send, cb->credit_receive,
				"at quiescence A.send == B.receive");
		ct_check_eq_u32(cb->credit_send, ca->credit_receive,
				"at quiescence B.send == A.receive");
		ct_check(ca->msgs_sent > 0u && cb->msgs_received > 0u,
			 "and real messages actually moved");
		ct_check_eq_u32(ca->msgs_sent, cb->msgs_received,
				"every message A sent, B received");
		ct_check_eq_u32(cb->msgs_sent, ca->msgs_received,
				"and every message B sent, A received");
		ct_check_eq_u32(ca->credit_overruns + cb->credit_overruns, 0u,
				"neither end ever sent past its window");
	}
}

/* ------------------------------------------------------------------ *
 * 3. A creditless send goes into CREDIT WAIT and is drained, not lost
 * ------------------------------------------------------------------ */
static void t_credit_wait(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;
	uint32_t i;

	printf("-- Credit Wait: no credit, no send -- and nothing is lost\n");
	/* B extends exactly 1 buffer, so A's second send has no credit. */
	rig(&a_conid, &b_conid, 6u, 1u);
	b_sysap.return_credit_immediately = 0u;

	ct_check_eq_u32(scsh_cdt(&a_node, a_conid)->credit_send, 1u,
			"A may send exactly one message");
	ct_check(step_send(&a_node, a_conid, 1u) == SCS_OK, "the first goes");
	(void)scsh_pump();
	ct_check(step_send(&a_node, a_conid, 2u) == SCS_OK,
		 "the second is ACCEPTED -- into Credit Wait");
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32(ca->sw_count, 1u, "one send is queued on the CDT");
	ct_check_eq_u32(ca->credit_stalls, 1u, "and the stall is counted");
	ct_check_eq_u32(b_sysap.n_message, 1u,
			"the peer has seen ONE message, not two");

	/* B's SYSAP releases the buffer: the credit comes back and the queued
	 * send goes out by itself (p. 2-45's resume rule). */
	(void)scs_fsm_return_credit(&b_node.fsm, b_conid, 1u);
	ct_check(step_send(&b_node, b_conid, 9u) == SCS_OK,
		 "B sends, piggybacking the returned credit");
	(void)scsh_pump();
	ct_check_eq_u32(scsh_cdt(&a_node, a_conid)->sw_count, 0u,
			"A's Credit Wait queue drained");
	(void)scsh_pump();
	ct_check_eq_u32(b_sysap.n_message, 2u,
			"the held message was delivered -- not dropped, not "
			"sent twice");
	ct_check(scsh_ledger_balanced(scsh_cdt(&a_node, a_conid)),
		 "A's ledger still balances");

	/* With NO Credit Wait pool bound, a creditless send is REFUSED, loudly.
	 * (Same rig, but the pool unbound.) */
	rig(&a_conid, &b_conid, 6u, 1u);
	a_node.fsm.sw = (struct scs_sendwait *)0;
	a_node.fsm.n_sw = 0u;
	ct_check(step_send(&a_node, a_conid, 1u) == SCS_OK, "one send fits");
	(void)scsh_pump();
	i = (uint32_t)(-step_send(&a_node, a_conid, 2u));
	ct_check_eq_u32(i, (uint32_t)(-SCS_ERR_NOCREDIT),
			"with no queue the second is REFUSED, never sent anyway");
}

/* ------------------------------------------------------------------ *
 * 4. THE 8-BEFORE-DISCONNECT INVARIANT (spec SS4(h)(1f))
 * ------------------------------------------------------------------ */
static void t_eight_before_disconnect(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;
	int i8, i6, i9;

	printf("-- the 8->9 exchange precedes DISCONNECT_REQ (SS4(h)(1f))\n");
	rig(&a_conid, &b_conid, 6u, 10u);
	a_sysap.return_credit_immediately = 1u;

	/* Give the ledger something real to return, so the op 8's credit field
	 * is a non-trivial ledger read rather than an accidental zero. A never
	 * sends, so nothing piggybacks and the pending count accumulates --
	 * which is exactly the one-way-flow case p. 2-44 introduces the special
	 * credit message for. */
	(void)step_send(&b_node, b_conid, 0x11u);
	(void)scsh_pump();
	(void)step_send(&b_node, b_conid, 0x12u);
	(void)scsh_pump();
	(void)step_send(&b_node, b_conid, 0x13u);
	(void)scsh_pump();
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32(ca->credit_pending, 3u,
			"A holds three Pending Receive Credits, unpiggybacked");
	ct_check(scsh_ledger_balanced(ca), "and its ledger balances");

	a_node.n_tx = 0u;   /* trace only the teardown */
	b_node.n_tx = 0u;
	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) == SCS_OK,
		 "A initiates the teardown");
	(void)scsh_pump();
	(void)scsh_pump();

	i8 = scsh_first_op(&a_node, SCS_MTYPE_CR_REQ);
	i6 = scsh_first_op(&a_node, SCS_MTYPE_DISC_REQ);
	ct_check(i8 >= 0, "A sent a type 8 (131/131 dialogues that disconnect)");
	ct_check(i6 >= 0, "A sent the DISCONNECT_REQ");
	ct_check(i8 < i6, "and the 8 came FIRST -- the invariant");
	ct_check_eq_u32(a_node.tx_credit[i8], 3u,
			"the op 8 carried the LEDGER's real pending count (3), "
			"not the constant 1 the corpus happens to show");
	ct_check_eq_u32(a_node.tx_credit[i6], 0u,
			"the op 6 carried credit 0 (131/131: the last credit "
			"on a connection is never returned)");
	ct_check_eq_u32((unsigned long)(i6 - i8), 1u,
			"A sent NOTHING between its 8 and its 6");

	i9 = scsh_first_op(&b_node, SCS_MTYPE_CR_RSP);
	ct_check(i9 >= 0, "B answered the 8 with a 9 (131/131)");
	ct_check_eq_u32(i9, 0u,
			"and the 9 is the ONLY frame in that window -- the "
			"whole connection's sequence is 8, 9, 6 (SS4(h)(1f))");
	ct_check_eq_u32(a_node.fsm.credit_msgs_sent, 1u, "one 8 originated");
	ct_check_eq_u32(a_node.fsm.credit_msgs_answered, 1u, "one 9 matched it");
	ct_check_eq_u32(a_node.fsm.disc_without_credit_msg, 0u,
			"no teardown had to skip the credit message");
}

/* The other half of the same invariant: the end that did NOT initiate the
 * teardown sends no type 8 (SS4(h)(1f): the type-8 sender is the connection's
 * opener AND the first DISCONNECT_REQ sender, 131/131). */
static void t_no_eight_when_answering(void)
{
	vms_conid_t a_conid, b_conid;

	printf("-- the answering end sends NO type 8\n");
	rig(&a_conid, &b_conid, 6u, 10u);
	b_node.n_tx = 0u;
	(void)scs_fsm_disconnect(&a_node.fsm, a_conid);
	(void)scsh_pump();
	(void)scsh_pump();
	(void)scsh_pump();

	ct_check_eq_u32(scsh_count_op(&b_node, SCS_MTYPE_CR_REQ), 0u,
			"B originated no op 8");
	ct_check_eq_u32(b_node.fsm.credit_msgs_sent, 0u, "counted as zero");
	ct_check_eq_u32(scsh_count_op(&b_node, SCS_MTYPE_CR_RSP), 1u,
			"B answered A's 8 with exactly one 9");
	ct_check(scsh_count_op(&b_node, SCS_MTYPE_DISC_RSP) >= 1u,
		 "B answered A's op 6 with an op 7");
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "A's connection is gone");
	ct_check(scsh_cdt(&b_node, b_conid) == (struct scs_cdt *)0,
		 "B's connection is gone");
	ct_check_eq_u32(a_sysap.n_closed, 1u, "A's SYSAP was told once");
	ct_check_eq_u32(b_sysap.n_closed, 1u, "B's SYSAP was told once");
}

/* ------------------------------------------------------------------ *
 * 5. The special credit message also fires on p. 2-44's low-credit rule
 * ------------------------------------------------------------------ */
static void t_flowcush_trigger(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_fsm_cfg cfg;
	uint32_t i;

	printf("-- SCSFLOWCUSH: a dangerously low Receive Credit sends an 8\n");
	rig(&a_conid, &b_conid, 6u, 10u);

	/* At the PUBLISHED default cushion of 1 a real VAX emitted ZERO type-8
	 * frames in 440 367 (spec SS4(h)(1g)); reproduce that first. */
	a_sysap.return_credit_immediately = 1u;
	for (i = 0; i < 5u; i++) {
		(void)step_send(&b_node, b_conid, (uint8_t)i);
		(void)scsh_pump();
	}
	ct_check_eq_u32(a_node.fsm.credit_msgs_sent, 0u,
			"cushion 1: no special credit message, as measured");

	/* Raise the cushion, as the lab did with WRITE ACTIVE, and the same
	 * traffic starts producing them -- the dose-response of SS4(h)(1g). */
	cfg = a_node.fsm.cfg;
	cfg.flowcush = 16u;
	scs_fsm_set_cfg(&a_node.fsm, &cfg);
	a_sysap.return_credit_immediately = 0u;
	(void)step_send(&b_node, b_conid, 0x77u);
	(void)scsh_pump();
	(void)scs_fsm_return_credit(&a_node.fsm, a_conid, 1u);
	ct_check(a_node.fsm.credit_msgs_sent >= 1u,
		 "cushion 16: the same traffic now produces a type 8");
	/* p. 2-44's threshold is SCSFLOWCUSH + the REMOTE SYSAP's Minimum Send
	 * Credits. Integration note E65 grounds that second term on the wire
	 * (SCS$W_MIN_CR, abs 72-73), so this end reads it off the connect verb
	 * that stated it instead of omitting it: the full rule runs, and the
	 * partial-threshold fallback -- which is still there for a connection
	 * that never learned one -- does not fire. */
	ct_check_eq_u32(a_node.fsm.credit_msg_partial_threshold, 0u,
			"and the FULL p. 2-44 threshold ran: no partial "
			"fallback was needed");
	ct_check(scsh_cdt(&a_node, a_conid)->peer_min_send_credits_valid == 1u,
		 "...because the peer's Minimum Send Credits was LEARNED off "
		 "its op-2 ACCEPT_REQ, not guessed");
	ct_check(scsh_ledger_balanced(scsh_cdt(&a_node, a_conid)),
		 "the ledger still balances after the special credit message");
}

/* ------------------------------------------------------------------ *
 * 6. A frame that never went out must not move the ledger
 * ------------------------------------------------------------------ */
static void t_failed_send_keeps_the_credit(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;
	uint16_t pending_before, send_before;

	printf("-- a transmit that FAILED returns no credit to anybody\n");
	rig(&a_conid, &b_conid, 6u, 10u);

	/* Build up a real pending count on A: B sends, A's SYSAP releases. */
	(void)step_send(&b_node, b_conid, 0x21u);
	(void)scsh_pump();
	(void)scs_fsm_return_credit(&a_node.fsm, a_conid, 1u);
	ca = scsh_cdt(&a_node, a_conid);
	pending_before = ca->credit_pending;
	send_before = ca->credit_send;
	ct_check_eq_u32(pending_before, 1u, "A holds one pending credit");

	a_node.fail_msg = 1;   /* the port refuses the next message */
	ct_check(step_send(&a_node, a_conid, 0x22u) != SCS_OK,
		 "the send is refused by the port");
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32(ca->credit_pending, pending_before,
			"the pending count is INTACT -- it was never returned "
			"to a peer that did not receive it");
	ct_check_eq_u32(ca->credit_send, send_before,
			"and no Send Credit was spent on a frame that never went");
	ct_check(scsh_ledger_balanced(ca), "the ledger still balances");

	/* And the credit really does go out on the next successful message. */
	ct_check(step_send(&a_node, a_conid, 0x23u) == SCS_OK, "the retry goes");
	ct_check_eq_u32(a_node.tx_credit[a_node.n_tx - 1u], pending_before,
			"carrying exactly the credit that was held back");
	ct_check_eq_u32(scsh_cdt(&a_node, a_conid)->credit_pending, 0u,
			"and only NOW is the pending count reset");
}

int main(void)
{
	t_grant_is_a_ledger_read();
	t_conservation_property();
	t_credit_wait();
	t_eight_before_disconnect();
	t_no_eight_when_answering();
	t_flowcush_trigger();
	t_failed_send_keeps_the_credit();
	return ct_summary("test_scs_fsm_credit");
}
