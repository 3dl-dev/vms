/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_scs_fsm_vcdown.c - R1: THE VC-BREAK CONTRACT (FC-P2.2).
 *
 * Design SS3.2.5, the FC-P2.2 half of the E10 ruling, and
 * docs/cluster-integration-notes.md E10, word for word:
 *
 *   "On vc_down(sysid, reason): every CDT on that SB -> CLOSED (path-lost),
 *    ledgers discarded, pending sends fail SS$_PATHLOST, each SYSAP's
 *    disconnected() called. SCS never retries across a break or re-opens
 *    itself -- CNXMAN's recnx_fsm reconnects."
 *
 * Each clause is a check below, on an SB carrying N connections at once, so
 * "every CDT" is a real quantifier and not a sample of one. The hardest clause
 * to fake is the last: the test asserts that SCS put ZERO frames on the wire
 * during and after the break -- no retry, no re-open, no farewell.
 *
 * This is also where the Credit Wait queue earns its keep: a send that was
 * held for credit when the path died is a REAL pending send, and it must fail
 * with path-lost and be reported, not silently evaporate and not be retried
 * onto a circuit that no longer exists.
 */

#include "cluster_test.h"
#include "scs_test_harness.h"

#define N_CONN 4u

static struct scsh_node a_node;
static struct scsh_node b_node;
static struct scsh_sysap a_sysap;
static struct scsh_sysap b_sysap;

/* Open N connections from A to B's one listening SYSAP, all on the same SB.
 * B's SYSAP extends exactly one buffer per connection, which lets the test put
 * a real send into Credit Wait on each of them. */
static uint32_t open_n(vms_conid_t *ids, uint16_t b_grant)
{
	uint32_t opened = 0u;
	uint32_t i;

	scsh_wire_reset();
	scsh_node_init(&a_node, 0x0101u, 0x33a0u);
	scsh_node_init(&b_node, 0x0202u, 0x8fd2u);
	scsh_sysap_init(&a_sysap, &a_node);
	scsh_sysap_init(&b_sysap, &b_node);
	b_sysap.connect_decision = 0;
	(void)scs_fsm_listen(&b_node.fsm, scsh_name_b, &b_sysap.ops, b_grant);
	scsh_link(&a_node, &b_node);

	for (i = 0; i < N_CONN; i++) {
		struct scs_connect_args args;
		uint32_t k;
		uint8_t *p = (uint8_t *)&args;

		for (k = 0; k < (uint32_t)sizeof(args); k++)
			p[k] = 0u;
		args.local_name = scsh_name_a;
		args.remote_name = scsh_name_b;
		args.sysap = &a_sysap.ops;
		args.dst = b_node.sysid;
		args.initial_credits = 4u;
		if (scs_fsm_connect(&a_node.fsm, &args, &ids[i]) != SCS_OK)
			continue;
		(void)scsh_pump();
		if (scsh_state(&a_node, ids[i]) == VMS_SCS_CDT_OPEN)
			opened++;
	}
	return opened;
}

static void t_all_cdts_close(void)
{
	vms_conid_t ids[N_CONN];
	struct scs_sb *sb;
	uint32_t i, still_open = 0u;
	uint32_t tx_before;

	printf("-- vc_down closes EVERY CDT on the SB, path-lost\n");
	ct_check_eq_u32(open_n(ids, 4u), N_CONN, "N connections are OPEN");

	sb = scs_fsm_sb_by_sysid(&a_node.fsm, b_node.sysid);
	ct_check(sb != (struct scs_sb *)0, "the SB exists");
	ct_check_eq_u32(sb->n_cdts, N_CONN,
			"and all N CDTs are queued to it (ch. 2's PB queue)");

	tx_before = a_node.n_tx;
	scs_fsm_vc_down(&a_node.fsm, b_node.sysid, 7u /* EXHAUSTED */);

	for (i = 0; i < N_CONN; i++) {
		if (scsh_cdt(&a_node, ids[i]) != (struct scs_cdt *)0)
			still_open++;
	}
	ct_check_eq_u32(still_open, 0u, "every one of the N is CLOSED");
	ct_check_eq_u32(sb->n_cdts, 0u, "the SB's CDT queue is empty");
	ct_check_eq_u32(sb->vc_up, 0u, "the SB records the circuit as down");
	ct_check_eq_u32(sb->cdts_pathlost, N_CONN, "and counts what it killed");

	/* Clause: each SYSAP's disconnected() called. */
	ct_check_eq_u32(a_sysap.n_closed, N_CONN,
			"the SYSAP was told closed() once per connection");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_PATHLOST,
			"with reason PATH-LOST -> the glue's SS$_PATHLOST");

	/* Clause: SCS never retries across a break or re-opens itself. */
	ct_check_eq_u32(a_node.n_tx - tx_before, 0u,
			"NOT ONE frame was sent during the break: no retry, "
			"no re-open, no farewell (design SS3.2.5)");

	/* And nothing re-opens later, either: the SYSAP reconnects, or nobody
	 * does. Drive the wire again and prove SCS stays quiet. */
	(void)scsh_pump();
	ct_check_eq_u32(a_node.n_tx - tx_before, 0u,
			"and still nothing after the queue is drained");
	{
		struct vms_scs_view v;

		scs_fsm_view_project(&a_node.fsm, &v);
		ct_check_eq_u32(v.n_cdts, 0u,
				"the SDA-shaped view shows no live connection");
	}
}

static void t_ledgers_discarded(void)
{
	vms_conid_t ids[N_CONN];
	struct scs_cdt *raw;
	uint32_t i, residue = 0u;

	printf("-- vc_down discards the credit ledgers\n");
	(void)open_n(ids, 4u);
	scs_fsm_vc_down(&a_node.fsm, b_node.sysid, 2u /* TIMVCFAIL */);

	/* Read the raw CDL slots, not through the Con.ID (which no longer
	 * resolves): a discarded ledger must be discarded in the storage, so a
	 * re-formed circuit cannot inherit a stale window. */
	for (i = 0; i < SCSH_CDL; i++) {
		raw = scs_fsm_cdt_at(&a_node.fsm, i);
		if (raw == (struct scs_cdt *)0)
			continue;
		residue += raw->credit_send + raw->credit_receive +
			   raw->credit_held + raw->credit_pending +
			   raw->credit_grant;
	}
	ct_check_eq_u32(residue, 0u,
			"no credit survives the break in ANY CDL slot "
			"(a re-formed circuit starts fresh, SS3.2.5)");
}

static void t_pending_sends_fail_pathlost(void)
{
	vms_conid_t ids[N_CONN];
	uint8_t body[SCS_SYSAP_BODY_LEN];
	uint32_t i;
	uint32_t queued = 0u;

	printf("-- pending sends fail PATH-LOST, and are never retried\n");
	/* One buffer per connection: the first send goes, the second waits. */
	ct_check_eq_u32(open_n(ids, 1u), N_CONN, "N connections are OPEN");
	b_sysap.return_credit_immediately = 0u;

	for (i = 0; i < SCS_SYSAP_BODY_LEN; i++)
		body[i] = 0xa5u;
	for (i = 0; i < N_CONN; i++) {
		(void)scs_fsm_send_msg(&a_node.fsm, ids[i], body,
				       SCS_SYSAP_BODY_LEN);
		(void)scsh_pump();
		/* the second one has no credit left and must be queued */
		if (scs_fsm_send_msg(&a_node.fsm, ids[i], body,
				     SCS_SYSAP_BODY_LEN) == SCS_OK &&
		    scsh_cdt(&a_node, ids[i])->sw_count > 0u)
			queued++;
	}
	ct_check(queued > 0u, "at least one send is really in Credit Wait");

	scs_fsm_vc_down(&a_node.fsm, b_node.sysid, 7u);

	ct_check_eq_u32(a_sysap.n_send_failed, queued,
			"every queued send was reported failed");
	ct_check_eq_u32(a_sysap.last_send_failed_reason, SCS_CLOSE_PATHLOST,
			"with reason PATH-LOST");
	for (i = 0; i < SCSH_SW; i++)
		ct_check_eq_u32(a_node.sw[i].in_use, 0u,
				"the Credit Wait pool entry was released");
	ct_check_eq_u32(scsh_wire.dropped, 0u,
			"and nothing was pushed at a dead wire");

	/* A send attempted AFTER the break must say PATH-LOST, not "no such
	 * connection" and certainly not succeed. */
	{
		struct scs_connect_args args;
		vms_conid_t fresh = 0u;
		uint32_t k;
		uint8_t *p = (uint8_t *)&args;

		for (k = 0; k < (uint32_t)sizeof(args); k++)
			p[k] = 0u;
		args.local_name = scsh_name_a;
		args.remote_name = scsh_name_b;
		args.sysap = &a_sysap.ops;
		args.dst = b_node.sysid;
		args.initial_credits = 4u;
		/* The SYSAP may still CONNECT again -- that is its decision,
		 * not SCS's. The point is that SCS did not do it by itself. */
		a_node.drop_tx = 1;
		(void)scs_fsm_connect(&a_node.fsm, &args, &fresh);
		k = (uint32_t)(-scs_fsm_send_msg(&a_node.fsm, fresh, body,
						 SCS_SYSAP_BODY_LEN));
		ct_check_eq_u32(k, (uint32_t)(-SCS_ERR_PATHLOST),
				"a send on a connection whose circuit is down "
				"fails PATH-LOST, distinctly");
	}
}

/* The break must reach a connection in ANY state, not just OPEN -- p. 2-31:
 * the VC breaks and every connection it supported breaks with it. */
static void t_breaks_from_any_state(void)
{
	vms_conid_t a_conid = 0u;
	struct scs_connect_args args;
	uint32_t k;
	uint8_t *p = (uint8_t *)&args;

	printf("-- a half-formed connection breaks too (p. 2-31)\n");
	scsh_wire_reset();
	scsh_node_init(&a_node, 0x0101u, 0x33a0u);
	scsh_node_init(&b_node, 0x0202u, 0x8fd2u);
	scsh_sysap_init(&a_sysap, &a_node);
	scsh_sysap_init(&b_sysap, &b_node);
	b_sysap.connect_decision = SCS_CONNECT_DEFER;
	(void)scs_fsm_listen(&b_node.fsm, scsh_name_b, &b_sysap.ops, 4u);
	scsh_link(&a_node, &b_node);

	for (k = 0; k < (uint32_t)sizeof(args); k++)
		p[k] = 0u;
	args.local_name = scsh_name_a;
	args.remote_name = scsh_name_b;
	args.sysap = &a_sysap.ops;
	args.dst = b_node.sysid;
	args.initial_credits = 4u;
	a_node.drop_tx = 1;
	(void)scs_fsm_connect(&a_node.fsm, &args, &a_conid);
	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_CONNECT_SENT, "a connect is outstanding");

	scs_fsm_vc_down(&a_node.fsm, b_node.sysid, 3u /* CHANNEL */);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "[CONNECT_SENT]VC_DOWN closes it");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_PATHLOST,
			"with path-lost");

	/* The acceptor's listening CDT holding a deferred request also breaks:
	 * the requester is gone, and holding the request would leave the SYSAP
	 * unable to accept the next one (ch. 2's one-at-a-time rule). */
	scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_CON_REQ, 0u,
			 0x33a00009u, 4u, scsh_name_b, scsh_name_a);
	ct_check_eq_u32((unsigned long)
			scsh_cdt(&b_node, b_sysap.last_listen_conid)->state,
			VMS_SCS_CDT_CONNECT_RCVD, "B holds a deferred request");
	{
		uint32_t closed_before = b_sysap.n_closed;
		struct scs_cdt *lc;

		scs_fsm_vc_down(&b_node.fsm, a_node.sysid, 3u);
		lc = scsh_cdt(&b_node, b_sysap.last_listen_conid);
		ct_check(lc != (struct scs_cdt *)0,
			 "the LISTENING CDT survives the break -- destroying "
			 "it would deregister the SYSAP");
		ct_check_eq_u32((unsigned long)lc->state, VMS_SCS_CDT_LISTEN,
				"...and is back in LISTEN, able to take the "
				"next connect (ch. 2's one-at-a-time rule)");
		ct_check_eq_u32(b_sysap.n_closed, closed_before + 1u,
				"the SYSAP was told the request it held is "
				"dead, rather than waiting forever");
		ct_check_eq_u32(b_sysap.last_close_reason, SCS_CLOSE_PATHLOST,
				"with path-lost");
	}
}

/* A break on a DIFFERENT system's SB must not touch this one's connections --
 * the scan is per Path Block, not global. */
static void t_break_is_scoped_to_the_sb(void)
{
	vms_conid_t ids[N_CONN];
	uint32_t i, alive = 0u;

	printf("-- the scan is per SB: another system's break is not ours\n");
	(void)open_n(ids, 4u);
	scs_fsm_vc_down(&a_node.fsm, 0x0f0fu /* a system with no SB */, 2u);
	for (i = 0; i < N_CONN; i++) {
		if (scsh_cdt(&a_node, ids[i]) != (struct scs_cdt *)0)
			alive++;
	}
	ct_check_eq_u32(alive, N_CONN, "connections on the live SB survive");
	ct_check_eq_u32(a_sysap.n_closed, 0u, "and no SYSAP was told anything");
}

int main(void)
{
	t_all_cdts_close();
	t_ledgers_discarded();
	t_pending_sends_fail_pathlost();
	t_breaks_from_any_state();
	t_break_is_scoped_to_the_sb();
	return ct_summary("test_scs_fsm_vcdown");
}
