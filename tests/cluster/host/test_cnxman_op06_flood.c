/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_op06_flood.c - E79: the op-0x06 MEMBERSHIP burst is CONSUMED,
 * and the credit it releases goes back through the ledger -- not as a frame
 * per record.
 *
 * ===========================================================================
 * THE DEFECT THIS FILE EXISTS TO KEEP DEAD
 *
 * E78 paid the p. 2-43 ledger and the live 2-node VAX cluster finally drove
 * the transition: the coordinator (VAX2) sent its op-0x06 MEMBERSHIP burst,
 * which OVMX had never once reached. OVMX answered every single one of them
 * with a category-0x04 body -- 254 frames in 31.6 ms, about 8000 a second --
 * and VAX2 took
 *
 *     **** Fatal BUG CHECK INVEXCEPTN, Exception while above ASTDEL or on
 *          interrupt stack, Current process=OPCOM
 *
 * and halted. OVMX did not fail to join; it took a real VAX down.
 *
 * ===========================================================================
 * THE LAW, MEASURED -- AND BOTH WRONG ANSWERS IT EXCLUDES
 *
 * Wire spec sec 4(o) row 10 says the burst is "acked by the joiner with
 * 0x04/0x49, 0x04/0x00, 0x04/0x02", which reads like three answers and was
 * implemented as one answer each. Measuring
 * `vax3-2to3-established-join-20260730.pcap` on the joiner's own link to the
 * coordinator settles it, and the answer is neither of the readings this code
 * has held:
 *
 *     254 op-0x06 in  ->  84 cat-0x04 out.   Ratio 3.02.
 *
 * and two independent readings of WHY agree exactly:
 *
 *   - the ack-msg word at body[2:4] advances by EXACTLY 3 across 83 of the 83
 *     in-burst gaps -- never 1, never 2, never 0;
 *   - the credit field at abs 62 reads 3 on 85 of the 86 carriers on that link
 *     (the 86th is a tail of 1).
 *
 * Corroborated over the whole reference corpus: 6549 acks from six responder
 * nodes, ack-word advance >= 3 in every one, an advance of 1 or 2 in none.
 *
 * So the cat-0x04 is a CREDIT CARRIER on a COALESCING QUANTUM of three
 * released buffers, and the two quantities are one fact seen twice: one credit
 * is one released receive buffer, and one released buffer is one consumed peer
 * message. The two wrong answers this excludes:
 *
 *   - ONE PER RECORD -- 254 frames, and a halted VAX. What E78 shipped.
 *   - ONE PER EXHAUSTED CREDIT WINDOW -- ~26 frames for the same burst, an
 *     under-ack by a factor of the grant that no reference node emits. It
 *     passes a naive "fewer than N" assertion, which is exactly why the
 *     assertions below are on the QUANTUM and not on a bound.
 *
 * The three "opcodes" are not three messages and not a phase-transition set:
 * they are one message drawn from a rotating pool of three recycled buffers
 * (they recur 28/34/28 times across the burst). sec 4(p) grounds body[9] as
 * uninitialised buffer content, and the bytes agree -- the 0x49 frame reads
 * "\x04\x49IR_LOOKUP  SCS$DIRECTORY", a buffer that last held
 * "DIR_LOOKUP  SCS$DIRECTORY" with body[8] overwritten by the category, and
 * the 0x02 frame is that node's own earlier op-0x02, same one byte overwritten.
 *
 * ===========================================================================
 * WHAT IS PROVED HERE
 *
 * 1. Against the REAL `struct scs_fsm`: a receiving SYSAP that releases every
 *    buffer and answers NOTHING -- which is exactly the op-0x06 shape -- emits
 *    one carrier per THREE records, and every carrier stamps a credit of
 *    exactly the quantum. Both wrong answers red: a per-record emit on the
 *    count, and a per-window emit on the count AND on the stamped credit.
 *
 * 2. That the peer is nonetheless never starved: all N arrive. The two
 *    failure modes sit on opposite sides of this test -- E78 was returning too
 *    little (the coordinator went mute) and E79 was emitting too much (the
 *    coordinator crashed) -- so both directions are asserted together.
 *
 * 3. That an ANSWERED message emits no standalone credit message at all: the
 *    p. 2-44 flush waits for the delivery to settle, because p. 2-44 sends the
 *    special credit message "instead of waiting for a message to ride on" and
 *    inside a delivery there may still be one.
 *
 * 4. That the shipping SYSAP is built this way -- read out of src/kernel-core,
 *    in the shape test_cnxman_credit_return.c established for the files that
 *    are not host-linkable.
 *
 * INV-6: no count here is chosen by the test. Every credit asserted was moved
 * by `scs_fsm_return_credit()`, which refuses to release more than the CDT's
 * own `credit_held`, and every frame counted was recorded by the harness off
 * the frame the FSM really built.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "scs_test_harness.h"

/* SYSGEN CLUSTER_CREDITS, and the grant the reference VC really carries: the
 * golden's four VMS$VAXcluster connect frames all stamp 10 at abs 62. */
#define VC_GRANT   10u

/* The live burst. 254 is the number of cat-0x04 frames OVMX put on the wire in
 * 31.6 ms before VAX2 bugchecked; 255 is the op-0x06 count the reference
 * capture carries. Either way the point is that it is LARGE. */
#define BURST_N   255u

static struct scsh_node coordinator;
static struct scsh_node manager;
static struct scsh_sysap coord_sysap;
static struct scsh_sysap mgr_sysap;

/* The reference geometry: the manager ACCEPTS and extends VC_GRANT buffers, so
 * VC_GRANT is exactly what the coordinator may spend before it needs paying. */
static void rig(vms_conid_t *coord_conid, vms_conid_t *mgr_conid)
{
	scsh_wire_reset();
	scsh_node_init(&coordinator, 0x0101u, 0x3358u);
	scsh_node_init(&manager, 0x0202u, 0xe995u);
	scsh_sysap_init(&coord_sysap, &coordinator);
	scsh_sysap_init(&mgr_sysap, &manager);
	mgr_sysap.connect_decision = 0;
	mgr_sysap.return_credit_immediately = 1u;   /* E78: the SYSAP pays */
	/* E79: ...and carries it back on the quantum, as CNXMAN does. */
	mgr_sysap.carrier_on_credit_due = 1u;
	coord_sysap.return_credit_immediately = 1u;
	(void)scs_fsm_listen(&manager.fsm, scsh_name_b, &mgr_sysap.ops,
			     (uint16_t)VC_GRANT);
	scsh_link(&coordinator, &manager);
	(void)scsh_open_pair(&coordinator, &manager, 10u, coord_conid);
	*mgr_conid = mgr_sysap.last_opened_conid;
}

static void send_one(struct scsh_node *from, vms_conid_t conid, uint8_t v)
{
	uint8_t body[SCS_SYSAP_BODY_LEN];

	memset(body, v, sizeof(body));
	(void)scs_fsm_send_msg(&from->fsm, conid, body, SCS_SYSAP_BODY_LEN);
}

/* Offer `n` messages, pumping between each -- the coordinator's own burst
 * cadence (the reference's are 0.1 ms apart, i.e. faster than any answer). */
static void burst(vms_conid_t coord_conid, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		send_one(&coordinator, coord_conid, (uint8_t)i);
		(void)scsh_pump();
	}
}

/* ------------------------------------------------------------------ *
 * 1. THE FLOOD, bounded by the ledger and by nothing else
 * ------------------------------------------------------------------ */
static void t_burst_consumed_does_not_answer_per_record(void)
{
	vms_conid_t coord_conid, mgr_conid;
	struct scs_cdt *cm;
	uint32_t tx_before, tx_burst;

	printf("-- an unanswered burst returns its credit in O(N/grant) "
	       "frames, never O(N)\n");
	rig(&coord_conid, &mgr_conid);
	tx_before = manager.n_tx;

	/* The op-0x06 shape: every buffer released, not one answered. */
	burst(coord_conid, BURST_N);
	tx_burst = manager.n_tx - tx_before;

	ct_check_eq_u32(mgr_sysap.n_message, BURST_N,
			"every record of the burst arrived -- the coordinator "
			"was never starved (the E78 failure, on the other "
			"side of this same test)");

	/*
	 * THE COALESCING LAW, ASSERTED AS AN EQUALITY. The reference ratio is
	 * 254/84 = 3.02, i.e. floor(N / quantum) carriers with the sub-quantum
	 * remainder never returned. Asserted exactly, because the two wrong
	 * answers differ from it in opposite directions and a bound would let
	 * one of them through: 255 records must not draw 255 frames (the flood
	 * that halted VAX2), and must not draw ~26 either (one per exhausted
	 * credit window, which passes any "fewer than N" test).
	 */
	ct_check_eq_u32(tx_burst, BURST_N / SCS_CREDIT_COALESCE,
			"the burst drew exactly one carrier per THREE records "
			"-- the measured 254 -> 84 ratio, not 255 (the flood) "
			"and not one per credit window (an under-ack)");
	ct_check(tx_burst > 0u,
		 "...and it DID return the credit: silence here is the E78 "
		 "stall that left the coordinator mute");

	cm = scsh_cdt(&manager, mgr_conid);
	ct_check(scsh_ledger_balanced(cm),
		 "the ledger balances across the whole burst");
	ct_check_eq_u32(cm->credit_overruns, 0u,
			"and the coordinator never sent past its window");

	/*
	 * And the p. 2-44 special credit message never had to fire at all: a
	 * SYSAP carrying its own credit on the quantum keeps the Receive Credit
	 * well clear of the cushion. The op 8 is the BACKSTOP, not the
	 * mechanism -- and no reference node emits one on a VMS$VAXcluster VC
	 * (0 op-8 frames on the 204-byte class in the golden).
	 */
	ct_check_eq_u32(scsh_count_op(&manager, (uint16_t)SCS_MTYPE_CR_REQ),
			0u,
			"and not one op-8 special credit message was needed: "
			"the SYSAP's own carrier is the mechanism here");
}

/* ------------------------------------------------------------------ *
 * 1b. THE DEFERRAL, isolated -- a SYSAP that releases and carries NOTHING
 *
 * The pre-E79 shape, kept as a live scenario because it is the one that drives
 * the Receive Credit onto p. 2-44's cushion WHILE A DELIVERY IS RUNNING: the
 * SYSAP releases its buffer inside the callback, the trigger fires there, and
 * the flush must be held to the end of the delivery rather than racing the
 * answer that might still be coming. Nothing is dropped -- with no answer and
 * no carrier, it really does go out.
 * ------------------------------------------------------------------ */
static void t_the_flush_defers_for_the_length_of_a_delivery(void)
{
	vms_conid_t coord_conid, mgr_conid;

	printf("-- the p. 2-44 flush waits for the delivery to settle\n");
	rig(&coord_conid, &mgr_conid);
	mgr_sysap.carrier_on_credit_due = 0u;   /* release only: no carrier */

	burst(coord_conid, VC_GRANT * 3u);

	ct_check_eq_u32(mgr_sysap.n_message, VC_GRANT * 3u,
			"all thirty arrived");
	ct_check(manager.fsm.credit_msgs_deferred > 0u,
		 "the flush was reached INSIDE a delivery and held back -- "
		 "zero here would mean the trigger never fired and this "
		 "scenario proved nothing");
	ct_check(manager.fsm.credit_msgs_sent > 0u,
		 "...and then really went out once the delivery settled: "
		 "deferred is not dropped");
	ct_check(scsh_count_op(&manager, (uint16_t)SCS_MTYPE_CR_REQ) <
		 VC_GRANT * 3u,
		 "and even the backstop is nowhere near one frame per record");
}

/* ------------------------------------------------------------------ *
 * 2. Every frame the burst DID draw carried real credit
 * ------------------------------------------------------------------ */
static void t_every_carrier_carries_ledger_credit(void)
{
	vms_conid_t coord_conid, mgr_conid;
	uint32_t i, lim, carriers = 0u, below_quantum = 0u, total = 0u;

	printf("-- every frame the burst drew stamps a REAL pending count\n");
	rig(&coord_conid, &mgr_conid);
	burst(coord_conid, VC_GRANT * 3u);

	lim = manager.n_tx < SCSH_TRACE ? manager.n_tx : SCSH_TRACE;
	for (i = 0; i < lim; i++) {
		if (manager.tx_op[i] != (uint16_t)SCS_MTYPE_CR_REQ &&
		    manager.tx_op[i] != (uint16_t)SCS_MTYPE_APPL_MSG)
			continue;
		carriers++;
		total += manager.tx_credit[i];
		if (manager.tx_credit[i] < (uint16_t)SCS_CREDIT_COALESCE)
			below_quantum++;
	}
	ct_check(carriers > 0u, "the burst drew at least one carrier");
	/*
	 * THE OTHER HALF OF THE LAW, read off the frames the FSM really built.
	 * The reference never stamps 1 or 2 -- 85 of 86 carriers read exactly
	 * 3 -- and the credit stamped is the same quantity as the ack-word
	 * advance the corpus audit measured at >= 3 in 6549 of 6549.
	 */
	ct_check_eq_u32(below_quantum, 0u,
			"NOT ONE carrier stamped a credit below the quantum: "
			"no 0 (a frame with nothing to carry -- 254 of those "
			"crashed VAX2) and no 1 or 2 (which the reference "
			"corpus never emits in 6549 acks)");
	ct_check_eq_u32(total, (VC_GRANT * 3u / SCS_CREDIT_COALESCE) *
			SCS_CREDIT_COALESCE,
			"and what they returned is exactly the quantum times "
			"the carriers -- the sub-quantum remainder is never "
			"returned, which is the residue sec 4(h)(1c) measured "
			"at 131 of 131 and could not explain");
	ct_check(total <= VC_GRANT * 3u,
		 "...and never more than really arrived (INV-6: the ledger "
		 "cannot be over-paid)");
}

/* ------------------------------------------------------------------ *
 * 3. AN ANSWERED MESSAGE EMITS NO STANDALONE CREDIT MESSAGE
 *
 * p. 2-44 sends the special credit message "instead of waiting for a message
 * to ride on". While a SYSAP is being delivered to there may still BE one, so
 * the flush waits for the delivery to settle. Before E79 the release inside
 * the SYSAP callback tripped the trigger and put an op 8 on the wire one line
 * before the answer that was about to carry the same credit.
 * ------------------------------------------------------------------ */
static void t_an_answer_piggybacks_instead_of_flushing(void)
{
	vms_conid_t coord_conid, mgr_conid;
	uint32_t i;

	printf("-- a SYSAP that answers piggybacks: no standalone op 8\n");
	rig(&coord_conid, &mgr_conid);

	for (i = 0; i < VC_GRANT * 3u; i++) {
		uint32_t before = mgr_sysap.n_message;

		send_one(&coordinator, coord_conid, (uint8_t)i);
		(void)scsh_pump();
		if (mgr_sysap.n_message == before)
			continue;
		send_one(&manager, mgr_conid, (uint8_t)(i ^ 0x5au));
		(void)scsh_pump();
	}

	ct_check_eq_u32(mgr_sysap.n_message, VC_GRANT * 3u,
			"all thirty arrived through a ten-buffer window");
	ct_check_eq_u32(scsh_count_op(&manager,
				      (uint16_t)SCS_MTYPE_CR_REQ), 0u,
			"and NOT ONE op 8 went out: the answer carried the "
			"credit, which is what p. 2-44 prefers");
	ct_check_eq_u32(manager.fsm.credit_msgs_deferred, 0u,
			"and the flush was never even reached: an answered "
			"message keeps the Receive Credit off p. 2-44's "
			"cushion, so there is nothing to defer either");
}

/* ==========================================================================
 * 4. THE SHIPPING CODE, read out of src/kernel-core
 * ========================================================================== */
static char src[400000];

static int read_src(const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", OVMX_KCORE_DIR, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(src, 1u, sizeof(src) - 1u, f);
	fclose(f);
	src[n] = '\0';
	return 0;
}

static void check_has(const char *needle, const char *what)
{
	ct_check(strstr(src, needle) != NULL, what);
}

static void check_absent(const char *needle, const char *what)
{
	ct_check(strstr(src, needle) == NULL, what);
}

static void t_the_join_fsm_originates_nothing_for_op06(void)
{
	printf("\n-- the op-0x06 handler answers nothing --\n");
	if (read_src("vms_cnxman_join_fsm.c") != 0) {
		ct_check(0, "could not open vms_cnxman_join_fsm.c");
		return;
	}

	/* The handler, whole. Its body is three statements and none of them
	 * builds or emits: a re-added ack cannot hide inside this shape. */
	check_has("static enum cnxman_join_rx join_h_membership("
		  "struct cnxman_join *j,\n"
		  "\t\t\t\t\t     const struct join_ev *e)\n"
		  "{\n"
		  "\tj->membership_records++;\n"
		  "\tjoin_learn_csid_from_membership(j, e);\n"
		  "\treturn CNXMAN_JOIN_RX_CONSUMED;\n"
		  "}",
		  "join_h_membership() COUNTS the record, learns the CSID, and "
		  "originates nothing at all");
	check_absent("vms_cm_ack_build(j->scratch",
		     "and no cat-0x04 is built anywhere in this FSM: the "
		     "carrier belongs to the ledger, not to an opcode");
}

static void t_the_carrier_is_gated_on_the_real_ledger(void)
{
	printf("\n-- the carrier asks SCS, and SCS answers from the CDT --\n");
	if (read_src("vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}

	check_has("if (!scs_credit_return_due(cn->cl->scs, local_conid))\n"
		  "\t\treturn;",
		  "the DECISION to emit a carrier is a ledger read, and the "
		  "first thing the function does");
	check_has("cnxman_credit_carrier(cn, local_conid,\n"
		  "\t\t\t\t      csb_by_conid(&cn->cl->club, local_conid));",
		  "...and it runs AFTER the FSMs, so an answer that already "
		  "piggybacked the credit leaves nothing to carry");
	/*
	 * THE ACK WORD IS THE CSB'S, which is what makes the corpus audit's
	 * "advance >= 3" hold without anybody computing a 3. body[2:4] is
	 * stamped from the CSB's dialogue_heard -- the peer's highest send-msg#
	 * this node has really seen, recorded for EVERY inbound message before
	 * any FSM is offered it -- so three released buffers are three consumed
	 * peer messages are an advance of three. A carrier that stamped its own
	 * envelope would break that identity silently.
	 */
	check_has("cnxman_envelope_originate(csb, cn->carrier, 1);",
		  "and the carrier's ack word is the CSB's own dialogue "
		  "state, so its advance IS the records consumed");
	check_has("cnxman_csb_dialogue_heard(csb, env.send_msg);",
		  "...which every inbound record moves, before any FSM sees "
		  "it -- including the op-0x06 this handler now consumes");

	if (read_src("vms_scs_fsm.c") != 0) {
		ct_check(0, "could not open vms_scs_fsm.c");
		return;
	}
	check_has("return cdt->credit_pending >= (uint16_t)SCS_CREDIT_COALESCE;",
		  "and the predicate SCS answers with is the measured "
		  "COALESCING QUANTUM -- three released buffers, not \"is "
		  "anything pending\" (the flood) and not the cushion (an "
		  "under-ack by a factor of the grant)");
	check_has("if (f->delivery_depth != 0u) {\n"
		  "\t\tf->credit_msgs_deferred++;",
		  "the low-credit flush defers for the length of a delivery, "
		  "so a released buffer cannot outrun the answer that was "
		  "about to carry it");
}

int main(void)
{
	t_burst_consumed_does_not_answer_per_record();
	t_the_flush_defers_for_the_length_of_a_delivery();
	t_every_carrier_carries_ledger_credit();
	t_an_answer_piggybacks_instead_of_flushing();
	t_the_join_fsm_originates_nothing_for_op06();
	t_the_carrier_is_gated_on_the_real_ledger();
	return ct_summary("test_cnxman_op06_flood");
}
