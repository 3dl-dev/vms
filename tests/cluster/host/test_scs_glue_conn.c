// SPDX-License-Identifier: GPL-2.0
/*
 * test_scs_glue_conn.c - FC-P2.4's R1: the CDT ROW PROJECTION
 * CLUSTER_DIAG_CONN hands userland, and the VC-break teardown behind it.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS TEST IS, AND WHAT IT IS NOT
 *
 * FC-P2.4's done-condition is R4 (two booted OVMX nodes open directory
 * connections and look each other up, both substrates), which needs a boot.
 * This is the R1 rung underneath it, and it covers the two things that CAN be
 * proved on a host in milliseconds:
 *
 *   1. THE PROJECTION. vms_scs.c's vms_scs_snapshot() / vms_scs_cdt_snapshot()
 *      are `fork_enter -> scs_fsm_view_project / scs_fsm_cdt_project ->
 *      fork_leave` and NOTHING else, exactly as vms_pe.c's snapshots are. So
 *      the projection is where every CLUSTER_DIAG_CONN value comes from, and
 *      it is pure -- testable here against a REAL connection driven to OPEN
 *      through the real FSM. Column for column against the SDA
 *      `SHOW CONNECTIONS` decoder ring (wire spec SS3,
 *      `sda-scs-extract-vax1.txt`): Local SYSAP, Remote, the Con.ID pair,
 *      Credit (Send/Recv), State, and the MTYPE phase byte.
 *   2. THE COMPOSITION. The rig below (scs_dir_test_harness.h, FC-P2.3) binds
 *      `struct scs_dir_ops` to the scs_fsm_* services with the SAME table
 *      vms_scs.c binds -- its own header says so ("the glue FC-P2.4 will
 *      write, in one place"). Section 6 then reads the SHIPPING
 *      src/kernel-core/vms_scs.c and asserts the three DOWNWARD bindings, the
 *      four UPWARD ones and the SCS$DIRECTORY registration are really there
 *      and are really one-line dereferences -- the same source-scan proof
 *      test_cnxman_body_level.c uses, and the reason a "the glue is not
 *      host-linkable" note is not the end of the story.
 *
 * vms_scs.c itself is NOT linked here: it names exec_kbackend.h and the FC-P0.5
 * fork API, so it belongs to rung R3 (substrate contract) and R4 (booted), the
 * same place vms_pe.c's glue lives (test_pe_view.c's own header note).
 */

#include <string.h>
#include <stdio.h>

#include "cluster_test.h"
#include "scs_dir_test_harness.h"

static uint8_t name_mscp_disk[VMS_SCS_PROCNAME_LEN];

static struct scsdh_node node_a;
static struct scsdh_node node_b;

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* The projection vms_scs_cdt_snapshot() performs, minus the fork mutex: fetch
 * the CDT by CDL index, and project ONLY if the executive really holds it.
 * Returns 0 like the ioctl's SS$_NORMAL, non-zero for its SS$_NOSUCHDEV. */
static int cdt_row(struct scsdh_node *n, uint32_t index,
		   struct vms_scs_cdt_view *out)
{
	struct scs_cdt *cdt = scs_fsm_cdt_at(&n->n.fsm, index);

	memset(out, 0, sizeof(*out));
	if (cdt == (struct scs_cdt *)0 || !cdt->in_use)
		return -1;
	scs_fsm_cdt_project(&n->n.fsm, cdt, out);
	return 0;
}

/* The same row, found by Con.ID rather than by index (what a reader that
 * already knows the handle does). */
static int cdt_row_by_conid(struct scsdh_node *n, vms_conid_t conid,
			    struct vms_scs_cdt_view *out)
{
	struct scs_cdt *cdt = scs_fsm_cdt_by_conid(&n->n.fsm, conid);

	memset(out, 0, sizeof(*out));
	if (cdt == (struct scs_cdt *)0 || !cdt->in_use)
		return -1;
	scs_fsm_cdt_project(&n->n.fsm, cdt, out);
	return 0;
}

/* How many CDL slots project a row at all (the executive's own count of live
 * connection descriptors, listening ones included). */
static uint32_t live_rows(struct scsdh_node *n)
{
	struct vms_scs_cdt_view v;
	uint32_t i, c = 0u;

	for (i = 0; i < SCSH_CDL; i++) {
		if (cdt_row(n, i, &v) == 0)
			c++;
	}
	return c;
}

/* The row for the one CDT in a given state, or -1. */
static int row_in_state(struct scsdh_node *n, enum vms_scs_cdt_state want,
			struct vms_scs_cdt_view *out)
{
	struct vms_scs_cdt_view v;
	uint32_t i;

	for (i = 0; i < SCSH_CDL; i++) {
		if (cdt_row(n, i, &v) != 0)
			continue;
		if (v.state == (uint8_t)want) {
			*out = v;
			return 0;
		}
	}
	return -1;
}

/* ==========================================================================
 * 1. A CONNECT THAT HAS NOT BOUND ITS PAIR YET
 *
 * The reference SDA extract's own `Remote Con. ID 00000000` (spec SS4(O.25))
 * is what a connection looks like before the peer's echo binds the pair. The
 * projection must show that as an ABSENT remote Con.ID (flag clear), not as a
 * handle whose value happens to be zero -- rule 2 of vms_cluster_snapshot.h,
 * and the whole reason `remote_conid_valid` exists.
 * ========================================================================== */
static void test_unbound_half_is_absent_not_zero(void)
{
	struct vms_scs_cdt_view v;

	printf("a CONNECT still in flight: Remote Con. ID is ABSENT, not 0\n");
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0101u, 0x4e63u);
	scsdh_node_init(&node_b, 0x0202u, 0xe2dcu);
	scsh_link(&node_a.n, &node_b.n);
	(void)scsdh_listen_directory(&node_a);
	(void)scsdh_listen_directory(&node_b);

	/* Build and record the connect, deliver nothing: the peer never
	 * answers, so the pair never binds. */
	node_a.n.drop_tx = 1;
	ct_check(scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
				 scsdh_result, &node_a) == SCS_OK,
		 "the client's inquiry opens a connection");
	(void)scsh_pump();

	ct_check(row_in_state(&node_a, VMS_SCS_CDT_CONNECT_SENT, &v) == 0,
		 "the client's CDT projects a row in CONNECT SENT");
	ct_check_eq_u32(v.state, (unsigned long)VMS_SCS_CDT_CONNECT_SENT,
			"State ordinal");
	ct_check(strcmp(scs_cdt_state_name((enum vms_scs_cdt_state)v.state),
			"connect sent") == 0,
		 "... whose NAME is what a reader prints");
	ct_check(v.local_conid != 0u,
		 "Local Con. ID is a real minted handle");
	ct_check_eq_u32(v.remote_conid_valid, 0u,
			"Remote Con. ID is flagged ABSENT");
	ct_check_eq_u32(v.remote_conid, 0u,
			"... and reads 0, which the flag is what disambiguates");
	ct_check(bytes_eq(v.local_name, scs_dir_name_lookup,
			  VMS_SCS_PROCNAME_LEN),
		 "Local SYSAP = SCS$DIR_LOOKUP (p. 2-51's Process Poller)");
	ct_check(bytes_eq(v.remote_name, scs_dir_name_directory,
			  VMS_SCS_PROCNAME_LEN),
		 "Remote SYSAP = SCS$DIRECTORY");
	ct_check_eq_u32(v.peer_sysid_lo, 0x0202u, "Remote system (low half)");
	ct_check_eq_u32(v.msgtype, (unsigned long)VMS_SCS_MT_SETUP,
			"MTYPE is 0x5b: the connection is still being "
			"established (spec SS4(m))");
}

/* ==========================================================================
 * 2. AN OPEN CONNECTION, BOTH ROWS, COLUMN FOR COLUMN
 *
 * Node B registers `SCS$DIRECTORY` with a SYSAP that accepts and then says
 * nothing, which is exactly what a node whose directory has not answered YET
 * looks like on the wire -- and it holds the transient connection OPEN so both
 * halves can be read. Both registrations are REAL entries in the one SDIR
 * queue; nothing here is a directory-side fake.
 * ========================================================================== */
/* A SYSAP that ACCEPTS and then says nothing. It is a genuine registration in
 * the one SDIR queue -- it extends the same credits the real SCS$DIRECTORY
 * does, so the ledger below is the real service's ledger and not the harness's
 * one-credit stub. */
static int mute_connect_req(void *ctx, vms_conid_t local_conid,
			    vms_scs_sysid_t peer, vms_conid_t peer_conid,
			    const uint8_t *conndata, uint32_t len)
{
	(void)ctx; (void)local_conid; (void)peer; (void)peer_conid;
	(void)conndata; (void)len;
	return 0;   /* accept */
}

static struct scs_sysap_ops mute_dir_ops = {
	mute_connect_req,
	(void (*)(void *, vms_conid_t))0,
	(int (*)(void *, vms_conid_t, const uint8_t *, uint32_t))0,
	(void (*)(void *, vms_conid_t, uint32_t))0,
	(void (*)(void *, vms_conid_t, uint32_t))0,
	(void *)0
};

static void open_pair(void)
{
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0101u, 0x4e63u);
	scsdh_node_init(&node_b, 0x0202u, 0xe2dcu);
	scsh_link(&node_a.n, &node_b.n);
	(void)scsdh_listen_directory(&node_a);
	/* a REAL registration of the real name, with a mute SYSAP behind it */
	(void)scs_fsm_listen(&node_b.n.fsm, scs_dir_name_directory,
			     &mute_dir_ops, (uint16_t)SCS_DIR_CREDITS_DEFAULT);
	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
			      scsdh_result, &node_a);
	(void)scsh_pump();
}

static void test_open_rows_are_the_sda_decoder_ring(void)
{
	struct vms_scs_cdt_view a, b;
	struct scs_dir_peer *p;

	printf("an OPEN directory connection: both CDT rows\n");
	open_pair();

	p = scs_dir_peer_by_sysid(&node_a.dir, node_b.n.sysid);
	ct_check(p != (struct scs_dir_peer *)0 &&
		 p->state == (uint8_t)SCS_DIR_OPEN,
		 "the transient connection is OPEN and the inquiry is out");

	ct_check(cdt_row_by_conid(&node_a, p->conid, &a) == 0,
		 "the CLIENT's CDT projects a row");
	ct_check(row_in_state(&node_b, VMS_SCS_CDT_OPEN, &b) == 0,
		 "the SERVER's connection CDT projects a row");

	/* ---- Local SYSAP / Remote SYSAP: the two halves mirror ---- */
	ct_check(bytes_eq(a.local_name, scs_dir_name_lookup,
			  VMS_SCS_PROCNAME_LEN) &&
		 bytes_eq(a.remote_name, scs_dir_name_directory,
			  VMS_SCS_PROCNAME_LEN),
		 "client row: SCS$DIR_LOOKUP -> SCS$DIRECTORY");
	ct_check(bytes_eq(b.local_name, scs_dir_name_directory,
			  VMS_SCS_PROCNAME_LEN) &&
		 bytes_eq(b.remote_name, scs_dir_name_lookup,
			  VMS_SCS_PROCNAME_LEN),
		 "server row: SCS$DIRECTORY <- SCS$DIR_LOOKUP");

	/* ---- Remote: the system, from the SB the CDT rides ---- */
	ct_check_eq_u32(a.peer_sysid_lo, 0x0202u, "client row: Remote = B");
	ct_check_eq_u32(b.peer_sysid_lo, 0x0101u, "server row: Remote = A");

	/* ---- the Con.ID PAIR, which is the whole point of the extract ---- */
	ct_check_eq_u32(a.remote_conid_valid, 1u,
			"client row: Remote Con. ID is bound");
	ct_check_eq_u32(b.remote_conid_valid, 1u,
			"server row: Remote Con. ID is bound");
	ct_check(a.local_conid == b.remote_conid,
		 "A's Local Con. ID IS B's Remote Con. ID (the pair swaps, "
		 "spec SS4(h) phase 4)");
	ct_check(b.local_conid == a.remote_conid,
		 "... and B's Local Con. ID is A's Remote Con. ID");
	ct_check(a.local_conid != b.local_conid,
		 "the two allocators minted DIFFERENT handles");

	/* ---- Credit (Send/Recv): the real ledger, not a template ----
	 *
	 * Both halves extended SCS_DIR_CREDITS_DEFAULT (3, spec SS4(h)(2a)'s
	 * byte-exact `[48:50]=3`), and exactly ONE application message has
	 * moved -- the client's inquiry. So the two rows are ASYMMETRIC in
	 * precisely the way the ledger says they must be, which is what makes
	 * this a read of executive state rather than of a template. */
	ct_check_eq_u32(a.credit_receive, (unsigned long)SCS_DIR_CREDITS_DEFAULT,
			"client row: Receive Credit is the full 3 it extended "
			"-- it has RECEIVED nothing");
	ct_check_eq_u32(a.credit_send,
			(unsigned long)(SCS_DIR_CREDITS_DEFAULT - 1u),
			"client row: Send Credit is one LOWER -- the inquiry "
			"on the wire spent exactly one");
	ct_check_eq_u32(b.credit_send, (unsigned long)SCS_DIR_CREDITS_DEFAULT,
			"server row: the mute server has spent none");
	ct_check_eq_u32(b.credit_receive,
			(unsigned long)(SCS_DIR_CREDITS_DEFAULT - 1u),
			"server row: Receive Credit is one lower -- the "
			"buffer the inquiry landed in is HELD, not free");
	ct_check_eq_u32(b.credit_pending, 0u,
			"server row: nothing to give back yet (the mute SYSAP "
			"never returned the credit)");

	/* ---- State ---- */
	ct_check(strcmp(scs_cdt_state_name((enum vms_scs_cdt_state)a.state),
			"open") == 0 &&
		 strcmp(scs_cdt_state_name((enum vms_scs_cdt_state)b.state),
			"open") == 0,
		 "both States print `open`");

	/* ---- MTYPE: spec SS4(m)'s phase rule, PER CDT ---- */
	ct_check_eq_u32(a.msgtype, (unsigned long)VMS_SCS_MT_MSG,
			"client MTYPE flipped to 0x4b: it has transmitted an "
			"application message");
	ct_check_eq_u32(b.msgtype, (unsigned long)VMS_SCS_MT_SETUP,
			"server MTYPE is still 0x5b: it has transmitted none");

	/* ---- the message counters are the CDT's own ---- */
	ct_check_eq_u32(a.msgs_sent, 1u, "client row: one message sent");
	ct_check_eq_u32(b.msgs_received, 1u, "server row: one received");
}

/* ==========================================================================
 * 3. THE SCS-WIDE ROW (CLUSTER_DIAG_CONN's row 0)
 * ========================================================================== */
static void test_scs_view(void)
{
	struct vms_scs_view va, vb;

	printf("the SCS-wide view after a completed directory round\n");
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0101u, 0x4e63u);
	scsdh_node_init(&node_b, 0x0202u, 0xe2dcu);
	scsh_link(&node_a.n, &node_b.n);
	/* Both nodes register SCS$DIRECTORY, which is what vms_scs_start()
	 * does on every node at CLUSTER_START. */
	(void)scsdh_listen_directory(&node_a);
	(void)scsdh_listen_directory(&node_b);
	(void)scsdh_listen_name(&node_b, name_mscp_disk);

	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
			      scsdh_result, &node_a);
	(void)scsh_pump();

	scs_fsm_view_project(&node_a.n.fsm, &va);
	scs_fsm_view_project(&node_b.n.fsm, &vb);

	ct_check_eq_u32(va.n_sysaps, 1u,
			"A has ONE registered SYSAP: SCS$DIRECTORY");
	ct_check_eq_u32(vb.n_sysaps, 2u,
			"B has two: SCS$DIRECTORY and MSCP$DISK");
	ct_check_eq_u32(va.n_sbs, 1u, "A holds one SB (one remote system)");
	ct_check_eq_u32(vb.n_sbs, 1u, "B holds one SB");
	ct_check_eq_u32(va.dir_lookups_sent, 1u, "A ISSUED one lookup");
	ct_check_eq_u32(vb.dir_lookups_served, 1u, "B ANSWERED one");
	ct_check_eq_u32(va.dir_lookups_served, 0u,
			"A answered none -- the two tallies are not one "
			"counter read twice");
	ct_check_eq_u32(va.conid_epoch, 0x4e63u,
			"conid_epoch is the seed the glue supplied, reported "
			"as the allocator holds it");
	ct_check(va.conid_seq >= 2u,
		 "conid_seq counts what this boot really minted (the "
		 "listening CDT and the connection)");
	ct_check_eq_u32(va.credit_stalls, 0u,
			"no send waited for credit at 3 extended");

	/* The round is over; p. 2-51's transient connection did not survive
	 * it, so only the listening CDTs remain. */
	ct_check_eq_u32(va.n_cdts, 1u,
			"A is left with its listening CDT alone");
	ct_check_eq_u32(vb.n_cdts, 2u,
			"B with its two listening CDTs");
}

/* ==========================================================================
 * 4. THE VC-BREAK (design SS3.2.5 / integration note E10)
 *
 * vms_scs.c binds pe_upper_ops.vc_down straight to scs_fsm_vc_down. This is
 * that contract read through the DIAGNOSTIC: after the break, every connection
 * row on that SB is gone, the listening CDT is NOT (destroying it would
 * silently deregister the SYSAP), and NOT ONE frame went out.
 * ========================================================================== */
static void test_vc_down_takes_every_cdt(void)
{
	struct vms_scs_cdt_view v;
	struct vms_scs_view before_a, after_a, after_b;
	uint32_t tx_a, tx_b;

	printf("a VC break, seen through CLUSTER_DIAG_CONN\n");
	open_pair();

	scs_fsm_view_project(&node_a.n.fsm, &before_a);
	ct_check_eq_u32(before_a.n_cdts, 2u,
			"A holds two CDTs: its listener and the connection");
	ct_check_eq_u32(live_rows(&node_b), 2u,
			"B holds two: its listener and the accepted "
			"connection");
	tx_a = node_a.n.n_tx;
	tx_b = node_b.n.n_tx;

	/* The port raises it; the glue's only additions are the count and the
	 * SS$_ status. Both ends lose the circuit, as they would. */
	scs_fsm_vc_down(&node_a.n.fsm, node_b.n.sysid,
			(uint32_t)SCS_CLOSE_PATHLOST);
	scs_fsm_vc_down(&node_b.n.fsm, node_a.n.sysid,
			(uint32_t)SCS_CLOSE_PATHLOST);

	scs_fsm_view_project(&node_a.n.fsm, &after_a);
	scs_fsm_view_project(&node_b.n.fsm, &after_b);

	ct_check_eq_u32(after_a.n_cdts, 1u,
			"A: the connection row is GONE, the listener remains");
	ct_check_eq_u32(after_b.n_cdts, 1u,
			"B: likewise");
	ct_check(row_in_state(&node_a, VMS_SCS_CDT_OPEN, &v) != 0,
		 "A has no `open` row left to project");
	ct_check(row_in_state(&node_b, VMS_SCS_CDT_OPEN, &v) != 0,
		 "B has no `open` row left to project");
	ct_check(row_in_state(&node_a, VMS_SCS_CDT_LISTEN, &v) == 0,
		 "A's LISTENING CDT survives the break (deregistering the "
		 "SYSAP is not what a lost path means)");
	ct_check(v.local_conid != 0u,
		 "... and still projects its real Con.ID");
	ct_check_eq_u32(v.credit_send, 0u,
			"a listening CDT carries no ledger, and says so");

	ct_check_eq_u32(node_a.n.n_tx - tx_a, 0u,
			"A put NOTHING on the wire across the break");
	ct_check_eq_u32(node_b.n.n_tx - tx_b, 0u,
			"B put nothing on the wire either -- SCS never "
			"retries and never re-opens (design SS3.2.5)");

	/* The SB survives the break as an object; what it lost is its CDTs. */
	ct_check_eq_u32(after_a.n_sbs, 1u, "the SB itself is still there");
}

/* ==========================================================================
 * 5. THE PROJECTION REFUSES WHAT THE EXECUTIVE DOES NOT HOLD (INV-6)
 * ========================================================================== */
static void test_absent_rows_are_refused(void)
{
	struct vms_scs_cdt_view v;
	uint32_t i, nonzero = 0u;

	printf("a CDL slot holding nothing projects NO row\n");
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0101u, 0x4e63u);
	scsdh_node_init(&node_b, 0x0202u, 0xe2dcu);
	scsh_link(&node_a.n, &node_b.n);
	(void)scsdh_listen_directory(&node_a);

	ct_check_eq_u32(live_rows(&node_a), 1u,
			"exactly one live row: the listening CDT");

	/* Every other slot refuses (the ioctl's SS$_NOSUCHDEV) and leaves the
	 * caller's row all-zero -- never a placeholder connection. */
	for (i = 0; i < SCSH_CDL; i++) {
		uint32_t k;
		const uint8_t *p;

		memset(&v, 0xa5, sizeof(v));
		if (cdt_row(&node_a, i, &v) == 0)
			continue;
		p = (const uint8_t *)&v;
		for (k = 0; k < (uint32_t)sizeof(v); k++) {
			if (p[k] != 0u)
				nonzero++;
		}
	}
	ct_check_eq_u32(nonzero, 0u,
			"every refused row was zeroed, not left as the "
			"caller's fill");

	/* Past the end of the CDL is the same answer, not a wrap. */
	ct_check(cdt_row(&node_a, SCSH_CDL + 4u, &v) != 0,
		 "an index past the CDL refuses");
	ct_check(cdt_row_by_conid(&node_a, 0xdeadbeefu, &v) != 0,
		 "a Con.ID this node never minted refuses");
}

/* ==========================================================================
 * 6. THE BINDINGS THEMSELVES, read out of the SHIPPING vms_scs.c
 *
 * The glue is not host-linkable (it names exec_kbackend.h), but its CONTENT is
 * readable, and the whole item is four bindings. The same source-scan proof
 * test_cnxman_body_level.c uses against the CNXMAN emitters: if a future edit
 * replaces a one-line dereference with a second implementation, or unbinds
 * vc_down, this reddens.
 * ========================================================================== */
static char scs_glue_src[240000];

static int read_glue(void)
{
	FILE *f = fopen(OVMX_KCORE_DIR "/vms_scs.c", "rb");
	size_t n;

	if (f == NULL)
		return -1;
	n = fread(scs_glue_src, 1u, sizeof(scs_glue_src) - 1u, f);
	fclose(f);
	scs_glue_src[n] = '\0';
	return 0;
}

static void check_glue_has(const char *needle, const char *what)
{
	ct_check(strstr(scs_glue_src, needle) != NULL, what);
}

static void test_glue_bindings(void)
{
	printf("the four bindings, read out of src/kernel-core/vms_scs.c\n");
	if (read_glue() != 0) {
		ct_check(0, "could not open vms_scs.c");
		return;
	}

	/* DOWNWARD: the three E1 one-liners onto the port's glue surface. */
	check_glue_has("pe_send_frame(scs->cl->pe",
		       "send_ctrl -> pe_send_frame (the FRAME primitive)");
	check_glue_has("pe_send_msg(scs->cl->pe",
		       "send_msg -> pe_send_msg (the BODY primitive)");
	check_glue_has("pe_addr(scs->cl->pe",
		       "addr -> pe_addr (REAL addressing, never invented)");
	check_glue_has("scs->ops.send_ctrl = scs_ops_send_ctrl",
		       "... and send_ctrl is really in the ops table");
	check_glue_has("scs->ops.send_msg = scs_ops_send_msg",
		       "... send_msg too");
	check_glue_has("scs->ops.addr = scs_ops_addr", "... and addr");

	/* UPWARD: pe_upper_ops, including the E10 vc_down seam. */
	check_glue_has("scs_fsm_rx_message(&((struct vms_scs *)ctx)->fsm",
		       "pe_upper_ops.message -> scs_fsm_rx_message");
	check_glue_has("scs_fsm_rx_datagram(&((struct vms_scs *)ctx)->fsm",
		       "pe_upper_ops.datagram -> scs_fsm_rx_datagram");
	check_glue_has("scs_fsm_vc_up(&((struct vms_scs *)ctx)->fsm",
		       "pe_upper_ops.vc_up -> scs_fsm_vc_up");
	check_glue_has("scs_fsm_vc_down(&scs->fsm, peer, reason)",
		       "pe_upper_ops.vc_down -> scs_fsm_vc_down (E10)");
	check_glue_has("scs->upper.vc_down = scs_upper_vc_down",
		       "... and vc_down is really in the upper ops table");
	check_glue_has("pe_set_upper(cl->pe, &scs->upper)",
		       "the port is really told about that table at start");

	/* The path-lost status the break is REPORTED with. */
	check_glue_has("case SCS_CLOSE_PATHLOST: return SS__DEVOFFLINE",
		       "SCS_CLOSE_PATHLOST maps to an SS$_ this tree can cite");

	/* The SYSAP the layer owns, registered at start. */
	check_glue_has("scs_fsm_listen(&scs->fsm, scs_dir_name_directory",
		       "SCS$DIRECTORY is registered at vms_scs_start()");
	check_glue_has("scs_dir_server_ops(&scs->dir)",
		       "... with the directory's OWN server table");

	/* The Con.ID seed is a LIVE per-boot value from the port, not a
	 * constant (vms_scs_fsm.h SS4 / INV-6). */
	check_glue_has("pe_incarnation(scs->cl->pe",
		       "the Con.ID allocator is seeded from the PORT's real "
		       "incarnation");
	ct_check(strstr(scs_glue_src, "scs_fsm_seed_conid(&scs->fsm, 0") == NULL,
		 "... and never from a literal zero");

	/* The snapshots are projections and nothing else. */
	check_glue_has("scs_fsm_view_project(&cl->scs->fsm, out)",
		       "vms_scs_snapshot is scs_fsm_view_project under the "
		       "fork mutex");
	check_glue_has("scs_fsm_cdt_project(&cl->scs->fsm, cdt, out)",
		       "vms_scs_cdt_snapshot is scs_fsm_cdt_project, and only "
		       "for a CDT that is in_use");
}

int main(void)
{
	(void)scs_dir_name_pad(name_mscp_disk, "MSCP$DISK");

	printf("FC-P2.4 R1: the CLUSTER_DIAG_CONN projection + the glue's "
	       "bindings\n\n");
	test_unbound_half_is_absent_not_zero();
	printf("\n");
	test_open_rows_are_the_sda_decoder_ring();
	printf("\n");
	test_scs_view();
	printf("\n");
	test_vc_down_takes_every_cdt();
	printf("\n");
	test_absent_rows_are_refused();
	printf("\n");
	test_glue_bindings();
	printf("\n");
	return ct_summary("test_scs_glue_conn");
}
