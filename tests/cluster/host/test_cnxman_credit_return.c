/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_credit_return.c - E78: the connection manager pays the p. 2-43
 * receive-buffer ledger, and a live coordinator therefore keeps talking.
 *
 * ===========================================================================
 * THE DEFECT THIS FILE EXISTS TO KEEP DEAD
 *
 * On the live 2-node VAX cluster (join-e77refire-1788538657.pcap) OVMX reached
 * the membership transition and stopped dead, one message short of everything
 * that matters. The coordinator (VAX2) sent, on the `VMS$VAXcluster`
 * connection OVMX had accepted:
 *
 *     cat 0x01 op 0x01  PARAMS      (its send-msg 1)
 *     cat 0x01 op 0x03  COMMIT      (2)
 *     cat 0x01 op 0x05  LOCKRB      (3)
 *     cat 0x01 op 0x05  LOCKRB      (4)
 *
 * and then never spoke again -- no op 0x06 MEMBERSHIP burst, no op 0x09
 * transition open, no barrier -- while the OTHER member's CSB for this node
 * timed out in `State: 09 wait long_break`. Nothing OVMX sent was malformed:
 * it echoed the COMMIT and both rebuild records correctly, and VAX2 accepted
 * every frame.
 *
 * FOUR is the whole story. OVMX's op-2 ACCEPT extended four receive buffers
 * (credit 4 at abs 62, frame 197) and every frame OVMX sent afterwards carried
 * credit ZERO. *VAXcluster Principles* pp. 2-43..2-44: the receiver copies its
 * Pending Receive Credit count into the credit field and the sender ADDS it to
 * its Send Credit -- so a peer that is never paid back may transmit exactly as
 * many messages as it was granted and not one more. The coordinator was not
 * refusing the join. It had run out of permission to speak.
 *
 * The reference joiner does it the other way, byte-visibly
 * (vax3-2to3-established-join-20260730.pcap): its 0x81/0x03 echo (frame 292)
 * carries credit 2 -- the two messages it had just taken -- and each 0x81/0x05
 * echo (295, 296) carries 1, so the coordinator's Send Credit never falls, and
 * the op-0x06 burst follows 0.1 ms after the last echo.
 *
 * ===========================================================================
 * WHAT IS PROVED HERE, AND ON WHAT
 *
 * 1. THE MECHANISM, against the REAL `struct scs_fsm` through the FC-P2.2
 *    harness: a receiving SYSAP that never releases its buffers wedges its
 *    peer after exactly `grant` messages -- the live failure, reproduced from
 *    first principles -- and one that releases them keeps the peer sending
 *    past the grant indefinitely, with the credit read back OFF THE BUILT
 *    FRAME through the codec, never off the caller's intent.
 *
 * 2. THAT THE SHIPPING SYSAPs DO IT. `vms_cnxman.c` and `vms_mscp_srv.c` are
 *    not host-linkable (they name exec_kbackend.h and the FC-P0.5 fork API --
 *    test_cnxman_glue.c's own note), so their content is read and scanned, in
 *    exactly the shape test_cnxman_glue.c established. The scan is ordered:
 *    the release must come BEFORE the dispatch that answers, because the
 *    reference answer carries the credit for the message it is answering.
 *
 * INV-6: nothing here manufactures a credit. `scs_fsm_return_credit()` refuses
 * to move more than the CDT's own `credit_held`, which SCS incremented when a
 * frame really arrived; a test that returned two for one message would red on
 * the conservation assertion below.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "scs_test_harness.h"

/* The grant the live run used, and the number of messages it bought. */
#define LIVE_GRANT 4u

static struct scsh_node coordinator;   /* the SENDING peer (VAX2's role)   */
static struct scsh_node manager;       /* the RECEIVING SYSAP (CNXMAN)     */
static struct scsh_sysap coord_sysap;
static struct scsh_sysap mgr_sysap;

/*
 * The rig, in the live run's own geometry: `manager` accepts the connection
 * and extends `mgr_grant` buffers, so `mgr_grant` is exactly what the
 * coordinator may spend before it needs paying.
 */
static void rig(vms_conid_t *coord_conid, vms_conid_t *mgr_conid,
		uint16_t mgr_grant, uint16_t release)
{
	scsh_wire_reset();
	scsh_node_init(&coordinator, 0x0101u, 0x3358u);
	scsh_node_init(&manager, 0x0202u, 0xe995u);
	scsh_sysap_init(&coord_sysap, &coordinator);
	scsh_sysap_init(&mgr_sysap, &manager);
	mgr_sysap.connect_decision = 0;
	mgr_sysap.return_credit_immediately = release;
	coord_sysap.return_credit_immediately = 1u;
	(void)scs_fsm_listen(&manager.fsm, scsh_name_b, &mgr_sysap.ops,
			     mgr_grant);
	scsh_link(&coordinator, &manager);
	(void)scsh_open_pair(&coordinator, &manager, 10u, coord_conid);
	*mgr_conid = mgr_sysap.last_opened_conid;
}

static int send_one(struct scsh_node *from, vms_conid_t conid, uint8_t v)
{
	uint8_t body[SCS_SYSAP_BODY_LEN];

	memset(body, v, sizeof(body));
	return scs_fsm_send_msg(&from->fsm, conid, body, SCS_SYSAP_BODY_LEN);
}

/* How many application messages really reached the manager's SYSAP. */
static uint32_t delivered(void)
{
	return mgr_sysap.n_message;
}

/* ------------------------------------------------------------------ *
 * 1. THE LIVE FAILURE, reproduced: an unpaid ledger silences the peer
 * ------------------------------------------------------------------ */
static void t_unpaid_ledger_silences_the_coordinator(void)
{
	vms_conid_t coord_conid, mgr_conid;
	struct scs_cdt *cc, *cm;
	uint32_t i;

	printf("-- an unpaid receive-buffer ledger silences the coordinator "
	       "after exactly the grant\n");
	rig(&coord_conid, &mgr_conid, (uint16_t)LIVE_GRANT, 0u);

	cc = scsh_cdt(&coordinator, coord_conid);
	cm = scsh_cdt(&manager, mgr_conid);
	ct_check_eq_u32(cm->credit_grant, LIVE_GRANT,
			"the manager extended the live run's four buffers");
	ct_check_eq_u32(cc->credit_send, LIVE_GRANT,
			"so the coordinator may send four messages");

	/* The coordinator offers eight -- PARAMS, COMMIT, two LOCKRB and the
	 * four MEMBERSHIP records that never arrived. */
	for (i = 0; i < 8u; i++) {
		(void)send_one(&coordinator, coord_conid, (uint8_t)i);
		(void)scsh_pump();
	}

	ct_check_eq_u32(delivered(), LIVE_GRANT,
			"exactly FOUR messages arrived -- the live run's "
			"PARAMS, COMMIT and two LOCKRB, then silence");
	cc = scsh_cdt(&coordinator, coord_conid);
	cm = scsh_cdt(&manager, mgr_conid);
	ct_check_eq_u32(cc->credit_send, 0u,
			"the coordinator's Send Credit is spent (p. 2-43)");
	ct_check_eq_u32(cm->credit_receive, 0u,
			"and the manager sees its own mirror at zero");
	ct_check_eq_u32(cm->credit_held, LIVE_GRANT,
			"all four buffers are HELD: the SYSAP never released "
			"one, so nothing can be piggybacked");
	ct_check_eq_u32(cm->credit_pending, 0u,
			"...hence a Pending Receive Credit of zero, which is "
			"the credit 0 the live capture shows on every frame");
	ct_check(scsh_ledger_balanced(cm),
		 "the ledger still balances: nothing was lost, only unpaid");
	ct_check(cc->credit_stalls > 0u,
		 "the unsent messages went into Credit Wait, not the wire");
}

/* ------------------------------------------------------------------ *
 * 2. PAID: the peer keeps sending past the grant, and the wire says so
 * ------------------------------------------------------------------ */
static void t_paid_ledger_keeps_the_coordinator_talking(void)
{
	vms_conid_t coord_conid, mgr_conid;
	struct scs_cdt *cc, *cm;
	uint32_t i;
	uint32_t answers = 0u;
	int saw_credit_on_the_wire = 0;

	printf("-- a released buffer replenishes the peer: the burst runs "
	       "past the grant\n");
	rig(&coord_conid, &mgr_conid, (uint16_t)LIVE_GRANT, 1u);

	/*
	 * Twelve messages through a four-buffer window -- three times the
	 * grant, which the unpaid case above could not reach. After each
	 * arrival the manager answers, exactly as CNXMAN answers a COMMIT or a
	 * rebuild record, and that answer is what carries the credit home.
	 */
	for (i = 0; i < 12u; i++) {
		uint32_t before = delivered();

		(void)send_one(&coordinator, coord_conid, (uint8_t)i);
		(void)scsh_pump();
		if (delivered() == before)
			continue;
		if (send_one(&manager, mgr_conid, (uint8_t)(i ^ 0x5au)) ==
		    SCS_OK)
			answers++;
		(void)scsh_pump();
	}

	ct_check_eq_u32(delivered(), 12u,
			"all twelve arrived through a four-buffer window");
	ct_check(answers >= 12u,
		 "and the manager answered every one of them");

	cc = scsh_cdt(&coordinator, coord_conid);
	cm = scsh_cdt(&manager, mgr_conid);
	ct_check(cc->credit_send > 0u,
		 "the coordinator still has Send Credit: it can send op 0x06");
	ct_check(scsh_ledger_balanced(cm), "the manager's ledger balances");
	ct_check_eq_u32(cm->credit_overruns, 0u,
			"and the coordinator never sent past its window");

	/*
	 * THE WIRE, not the intent. `tx_credit` is recorded by the harness off
	 * the frame the FSM built, through the codec. The reference joiner's
	 * answers carry 1 and 2; what matters is that a nonzero credit really
	 * left this node, which is the byte the live run never emitted.
	 */
	for (i = 0; i < manager.n_tx; i++) {
		if (manager.tx_op[i] == (uint16_t)SCS_MTYPE_APPL_MSG &&
		    manager.tx_credit[i] > 0u)
			saw_credit_on_the_wire = 1;
	}
	ct_check(saw_credit_on_the_wire,
		 "a NONZERO credit really went out at abs 62 on an "
		 "application message -- the byte the E77 run never sent");
}

/* ------------------------------------------------------------------ *
 * 3. The ledger refuses to be over-paid (INV-6, from the other side)
 * ------------------------------------------------------------------ */
static void t_a_buffer_never_held_cannot_be_returned(void)
{
	vms_conid_t coord_conid, mgr_conid;
	struct scs_cdt *cm;

	printf("-- a credit for a message that never arrived is REFUSED\n");
	rig(&coord_conid, &mgr_conid, (uint16_t)LIVE_GRANT, 0u);

	cm = scsh_cdt(&manager, mgr_conid);
	ct_check_eq_u32(cm->credit_held, 0u, "nothing has arrived yet");
	ct_check(scs_fsm_return_credit(&manager.fsm, mgr_conid, 1u) != SCS_OK,
		 "returning a buffer that was never held is refused, not "
		 "silently granted (that would MANUFACTURE credit)");
	cm = scsh_cdt(&manager, mgr_conid);
	ct_check_eq_u32(cm->credit_pending, 0u,
			"and the pending count did not move");
}

/* ==========================================================================
 * 4. THE SHIPPING SYSAPs, read out of src/kernel-core
 * ========================================================================== */
static char src[300000];

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

static void t_cnxman_releases_before_it_answers(void)
{
	printf("\n-- the shipping VMS$VAXcluster SYSAP pays the ledger --\n");
	if (read_src("vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}

	check_has("scs_return_credit(cn->cl->scs, local_conid, 1u)",
		  "one buffer is returned to SCS for one message that "
		  "really arrived");

	/*
	 * ORDER IS THE POINT. The reference joiner's 0x81 echo carries the
	 * credit for the message it is answering, so the release has to
	 * precede the dispatch that emits the answer. This exact contiguous
	 * shape exists only at the top of cnxman_vc_message().
	 */
	check_has("cnxman_release_receive_buffer(cn, local_conid);\n"
		  "\tcnxman_note_peer_credit(cn, local_conid);\n"
		  "\n"
		  "\tcn->cur_conid = local_conid;",
		  "the VC SYSAP releases the buffer BEFORE it routes the "
		  "body to the FSM that answers it");

	check_has("cnxman_release_receive_buffer(cn, local_conid);\n"
		  "\n"
		  "\tcnxman_join_rx_mscp(",
		  "and the disk-client SYSAP does the same before its own "
		  "command/END dialogue");

	/* The starvation edge is observable, not merely fixed. */
	check_has("CNXMAN_DIAG_R_PEER_NOCREDIT",
		  "the peer's spent Send Credit is recorded in the "
		  "transition ring, so the next stall names itself");
	check_has("scs_cdt_view(cn->cl->scs, local_conid, &v)",
		  "...from the LIVE CDT projection, never a computed guess");

	/* The grant is SYSGEN's, not this layer's invention. */
	check_has("cluster_sysgen_credits(cl, &credits)",
		  "the VMS$VAXcluster grant is SYSGEN CLUSTER_CREDITS, read "
		  "from the real parameter record");
	check_has("scs_sysap_listen(cl->scs, cnxman_join_name_vaxcluster,\n"
		  "\t\t\t\t       &cn->vc_sysap, cnxman_vc_grant(cl))",
		  "...and that is what the SYSAP registration extends");
}

static void t_mscp_server_releases_before_it_answers(void)
{
	printf("\n-- the shipping MSCP$DISK server SYSAP pays it too --\n");
	if (read_src("vms_mscp_srv.c") != 0) {
		ct_check(0, "could not open vms_mscp_srv.c");
		return;
	}
	check_has("scs_return_credit(s->cl->scs, local_conid, 1u);\n"
		  "\treturn mscp_srv_fsm_command(",
		  "the server releases the command's buffer before the END "
		  "message that carries the credit is built");
}

int main(void)
{
	t_unpaid_ledger_silences_the_coordinator();
	t_paid_ledger_keeps_the_coordinator_talking();
	t_a_buffer_never_held_cannot_be_returned();
	t_cnxman_releases_before_it_answers();
	t_mscp_server_releases_before_it_answers();
	return ct_summary("test_cnxman_credit_return");
}
