// SPDX-License-Identifier: GPL-2.0
/*
 * test_scs_dir_join.c - FC-P2.3's second done-condition: THE ESTABLISHED-JOIN
 * SERVER-HALF SEQUENCE (wire spec sec 4(L)) REPLAYS.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS BEING REPLAYED, AND WHERE IT COMES FROM
 *
 * Against an ESTABLISHED cluster the member drives admission and the joiner is
 * a SERVER first (sec 4(L)(7) and the byte-decoded af2 table in
 * docs/design-cluster-join-choreography.md, "The established-join is
 * MEMBER-DRIVEN"). Its directory phase, in order, is:
 *
 *   M -> J   op 0    SCS$DIRECTORY connect          (the member opens it)
 *   J -> M   op 1    echo
 *   J -> M   op 2    accept, supplying J's handle
 *   M -> J   op 3    confirm                         (the CONNECTOR confirms)
 *   M -> J   op 10   lookup MSCP$TAPE       -> MISS
 *   M -> J   op 10   lookup MSCP$DISK       -> HIT
 *   M -> J   op 10   lookup VMS$VAXcluster  -> HIT
 *
 * and the choreography note's own conclusion is the reason this sequence is a
 * done-condition at all: "the joiner must SERVE the member's drive -- answer
 * the member's directory connect, serve MSCP$DISK and VMS$VAXcluster lookups
 * as HITs -- all BEFORE it opens its own reciprocal client half." The retired
 * strawman failed exactly here: it answered `MSCP$DISK` "NOT PRESENT HERE"
 * because its responder hard-coded one affirmative name, so the member never
 * opened its disk connection.
 *
 * OVMX cannot fail that way now, and this test is the proof: J answers HIT for
 * precisely the names REGISTERED on J, and the last case below flips MSCP$TAPE
 * from miss to hit by registering it -- the answer follows the registry, which
 * is the whole of INV-6 for this SYSAP.
 *
 * SCOPE. This is the DIRECTORY phase of that join, which is what FC-P2.3 owns.
 * The MSCP$DISK connect (ss=7) and the VMS$VAXcluster VC connect (ss=8) that
 * follow it in the same capture belong to FC-P6.x and FC-P3.x.
 *
 * The member half is driven by OVMX's own SCS$DIR_LOOKUP client, so what is
 * asserted is that BOTH halves of the reference exchange are reproduced, on
 * one wire, by executive-resident state -- the joiner's answers coming out of
 * its SDIR queue and the member's inquiries out of its inquiry table.
 */

#include "cluster_test.h"
#include "scs_dir_test_harness.h"

/* J = OVMX, the joining node that must SERVE. M = the established member. */
static struct scsdh_node node_j;
static struct scsdh_node node_m;

static uint8_t nm_tape[VMS_SCS_PROCNAME_LEN];
static uint8_t nm_disk[VMS_SCS_PROCNAME_LEN];
static uint8_t nm_vc[VMS_SCS_PROCNAME_LEN];

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ *
 * The joiner as the reference capture found it: SCS$DIRECTORY plus the
 * two names the member goes on to connect to. MSCP$TAPE is deliberately
 * NOT registered -- that is why the reference answer is a miss.
 * ------------------------------------------------------------------ */
static void stage_established_join(void)
{
	scsh_wire_reset();
	scsdh_node_init(&node_j, 0x08fdu, 0x8fd1u);   /* the joiner   */
	scsdh_node_init(&node_m, 0x3556u, 0x356bu);   /* the member   */
	scsh_link(&node_j.n, &node_m.n);

	(void)scsdh_listen_directory(&node_j);
	(void)scsdh_listen_name(&node_j, nm_disk);
	(void)scsdh_listen_name(&node_j, nm_vc);

	/* The member serves a directory too -- every node does (p. 2-50) --
	 * and in the capture it opens its own probe later. Registering it here
	 * makes the rig symmetric without driving that half. */
	(void)scsdh_listen_directory(&node_m);
}

/* The member drives its three inquiries, in the captured order. */
static void member_drives(void)
{
	(void)scs_dir_inquire(&node_m.dir, node_j.n.sysid, nm_tape,
			     scsdh_result, &node_m);
	(void)scs_dir_inquire(&node_m.dir, node_j.n.sysid, nm_disk,
			     scsdh_result, &node_m);
	(void)scs_dir_inquire(&node_m.dir, node_j.n.sysid, nm_vc,
			     scsdh_result, &node_m);
	(void)scsh_pump();
}

/* ==========================================================================
 * 1. THE VERB SEQUENCE the joiner emits
 * ========================================================================== */
static void check_joiner_verbs(void)
{
	static const uint16_t want[5] = {
		SCS_MTYPE_CON_RSP,    /* op 1  echo                       */
		SCS_MTYPE_ACCP_REQ,   /* op 2  accept, J's handle         */
		SCS_MTYPE_APPL_MSG,   /* op 10 answer: MSCP$TAPE          */
		SCS_MTYPE_APPL_MSG,   /* op 10 answer: MSCP$DISK          */
		SCS_MTYPE_APPL_MSG    /* op 10 answer: VMS$VAXcluster     */
	};
	uint32_t i;
	int ok = 1;

	ct_check(node_j.n_tap >= 5u, "the joiner emitted at least 5 frames");
	for (i = 0; i < 5u && i < node_j.n_tap; i++) {
		if (scsdh_tap_op(&node_j, i) != want[i])
			ok = 0;
	}
	ct_check(ok, "joiner sequence = echo, accept, answer, answer, answer "
		     "(sec 4(L), af2)");
}

static void check_member_verbs(void)
{
	uint32_t i, n_req = 0u;

	ct_check(scsdh_tap_op(&node_m, 0u) == SCS_MTYPE_CON_REQ,
		 "the MEMBER opens the directory connection (op 0)");
	ct_check(scsdh_tap_op(&node_m, 1u) == SCS_MTYPE_ACCP_RSP,
		 "... and the CONNECTOR sends the op-3 confirm");
	for (i = 0; i < node_m.n_tap; i++) {
		struct vms_scs_dir_msg m;

		if (scsdh_tap_dir(&node_m, i, &m) != 0)
			continue;
		if (m.marker == VMS_SCS_DIR_MARKER_REQUEST)
			n_req++;
	}
	ct_check_eq_u32(n_req, 3u, "the member asked exactly three names");
}

/* ==========================================================================
 * 2. THE ANSWERS -- content, in order, off the wire
 * ========================================================================== */
static int nth_answer(const struct scsdh_node *d, uint32_t nth,
		      struct vms_scs_dir_msg *out)
{
	uint32_t i, seen = 0u;

	for (i = 0; i < d->n_tap; i++) {
		if (scsdh_tap_dir(d, i, out) != 0)
			continue;
		if (out->marker != VMS_SCS_DIR_MARKER_RESPONSE)
			continue;
		if (seen == nth)
			return 0;
		seen++;
	}
	return -1;
}

static void check_answers(void)
{
	struct vms_scs_dir_msg a;

	ct_check(nth_answer(&node_j, 0u, &a) == 0, "answer 1 is on the wire");
	ct_check(bytes_eq(a.lookup.queried_name, nm_tape,
			  VMS_SCSCTRL_NAME_LEN), "... it is about MSCP$TAPE");
	ct_check(bytes_eq(a.lookup.result, vms_scs_dir_not_present_here,
			  VMS_SCSCTRL_NAME_LEN),
		 "... and says NOT PRESENT HERE, byte for byte");

	ct_check(nth_answer(&node_j, 1u, &a) == 0, "answer 2 is on the wire");
	ct_check(bytes_eq(a.lookup.queried_name, nm_disk,
			  VMS_SCSCTRL_NAME_LEN), "... it is about MSCP$DISK");
	ct_check_eq_u32(a.lookup.result_kind, VMS_SCS_DIR_RESULT_AFFIRMATIVE,
			"... and it is a HIT (the strawman's failure point)");
	ct_check(bytes_eq(a.lookup.result, nm_disk, VMS_SCSCTRL_NAME_LEN),
		 "... result = the name echoed blank-padded (af2 byte-exact)");

	ct_check(nth_answer(&node_j, 2u, &a) == 0, "answer 3 is on the wire");
	ct_check(bytes_eq(a.lookup.queried_name, nm_vc, VMS_SCSCTRL_NAME_LEN),
		 "... it is about VMS$VAXcluster");
	ct_check_eq_u32(a.lookup.result_kind, VMS_SCS_DIR_RESULT_AFFIRMATIVE,
			"... and it is a HIT");

	ct_check_eq_u32(node_m.n_results, 3u, "the member was told all three");
	ct_check(node_m.results_present[0] == 0, "MSCP$TAPE      -> absent");
	ct_check(node_m.results_present[1] != 0, "MSCP$DISK      -> present");
	ct_check(node_m.results_present[2] != 0, "VMS$VAXcluster -> present");
}

/* ==========================================================================
 * 3. THE INTERLEAVE -- request, answer, request, answer, ... (sec 4(h)(2a))
 *
 * The reference census of one directory connection lists its lookup frames
 * strictly alternating (37, 39, 41, 42, 43, 44, 45, 46). Reconstructed here
 * from the global arrival ordinal, across BOTH nodes' frames.
 * ========================================================================== */
static void check_interleave(void)
{
	uint32_t order[16];
	uint32_t marker[16];
	uint32_t n = 0u, i, j, k;
	int ok = 1;

	for (k = 0; k < 2u; k++) {
		const struct scsdh_node *d = (k == 0u) ? &node_m : &node_j;

		for (i = 0; i < d->n_tap && n < 16u; i++) {
			struct vms_scs_dir_msg m;

			if (scsdh_tap_dir(d, i, &m) != 0)
				continue;
			order[n] = d->tap[i].seq;
			marker[n] = m.marker;
			n++;
		}
	}
	/* sort by arrival ordinal (n <= 6 here) */
	for (i = 0; i + 1u < n; i++) {
		for (j = i + 1u; j < n; j++) {
			uint32_t t;

			if (order[j] >= order[i])
				continue;
			t = order[i];
			order[i] = order[j];
			order[j] = t;
			t = marker[i];
			marker[i] = marker[j];
			marker[j] = t;
		}
	}
	ct_check_eq_u32(n, 6u, "six directory messages crossed the wire");
	for (i = 0; i < n; i++) {
		uint32_t want = (i % 2u) == 0u ? VMS_SCS_DIR_MARKER_REQUEST
					       : VMS_SCS_DIR_MARKER_RESPONSE;

		if (marker[i] != want)
			ok = 0;
	}
	ct_check(ok, "they strictly alternate request/response (sec 4(h)(2a))");
}

/* ==========================================================================
 * 3b. NO SPECIAL CREDIT MESSAGE INTERRUPTS THE ROUND
 *
 * Spec sec 4(h)(1g) measured ZERO type-8 frames in 440 367 at the published
 * SCSFLOWCUSH default, so a directory round that needed one would be OVMX
 * inventing wire traffic the reference never shows. It does not need one:
 * each answer piggybacks the buffer the previous exchange released (p. 2-43),
 * which is why the grant this connection carries is load-bearing rather than
 * cosmetic. The op 8 at the END is the GROUNDED teardown one (sec 4(h)(1f),
 * 131 of 131), and this checks it comes after the last inquiry, not among them.
 * ========================================================================== */
static void check_no_credit_message_mid_round(void)
{
	int first8 = scsh_first_op(&node_m.n, SCS_MTYPE_CR_REQ);
	int last_dir = -1;
	uint32_t i;

	ct_check_eq_u32(node_j.n.fsm.credit_msgs_sent, 0u,
			"the ANSWERING node never needed a special credit "
			"message");
	for (i = 0; i < node_m.n.n_tx && i < SCSH_TRACE; i++) {
		if (node_m.n.tx_op[i] == (uint16_t)SCS_MTYPE_APPL_MSG)
			last_dir = (int)i;
	}
	ct_check(last_dir >= 0, "the member's inquiries are in its trace");
	ct_check(first8 < 0 || first8 > last_dir,
		 "no op 8 interrupts the round -- only the teardown's");
}

/* ==========================================================================
 * 4. THE CONNECTION DOES NOT SURVIVE THE ROUND (p. 2-51)
 * ========================================================================== */
static void check_transient(void)
{
	struct scs_dir_peer *p = scs_dir_peer_by_sysid(&node_m.dir,
						       node_j.n.sysid);

	ct_check(p != (struct scs_dir_peer *)0, "the member has a peer row");
	ct_check_eq_u32(p->state, SCS_DIR_IDLE,
			"the directory connection is gone once the round ended");
	ct_check_eq_u32(node_m.dir.cli_rounds, 1u, "exactly one round");
	ct_check_eq_u32(node_m.dir.cli_timeouts, 0u, "nothing timed out");
	ct_check_eq_u32(node_j.dir.srv_hits, 2u, "the joiner served two hits");
	ct_check_eq_u32(node_j.dir.srv_misses, 1u, "... and one honest miss");
}

/* ==========================================================================
 * 5. THE ANSWER FOLLOWS THE REGISTRY (INV-6)
 *
 * Same drive, same code, one difference: MSCP$TAPE is registered. If the miss
 * above were a hard-coded name list rather than a registry read, this would
 * still answer NOT PRESENT HERE.
 * ========================================================================== */
static void check_answer_follows_registry(void)
{
	struct vms_scs_dir_msg a;

	stage_established_join();
	(void)scsdh_listen_name(&node_j, nm_tape);
	member_drives();

	ct_check(nth_answer(&node_j, 0u, &a) == 0, "the MSCP$TAPE answer");
	ct_check_eq_u32(a.lookup.result_kind, VMS_SCS_DIR_RESULT_AFFIRMATIVE,
			"... is now a HIT, because the SYSAP is now listening");
	ct_check(node_m.results_present[0] != 0,
		 "... and the member is told it is present");
	ct_check_eq_u32(node_j.dir.srv_misses, 0u, "no miss this time");
}

int main(void)
{
	(void)scs_dir_name_pad(nm_tape, "MSCP$TAPE");
	(void)scs_dir_name_pad(nm_disk, "MSCP$DISK");
	(void)scs_dir_name_pad(nm_vc, "VMS$VAXcluster");

	printf("established-join server half (sec 4(L), af2)\n");
	stage_established_join();
	member_drives();
	check_joiner_verbs();
	check_member_verbs();
	check_answers();
	check_interleave();
	check_no_credit_message_mid_round();
	check_transient();

	printf("the answer follows the registry\n");
	check_answer_follows_registry();

	return ct_summary("scs_dir_join");
}
