/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_scs_fsm.c - R1: EVERY transition of the SCS CDT ladder (FC-P2.2).
 *
 * The plan row's done-condition is "R1: every ladder transition". Each
 * function below walks one row of the [cdt state][event] table and asserts the
 * STATE the executive holds afterwards, plus the verb it actually put on the
 * wire (read back off the built frame through the codec, never off the
 * caller's intent).
 *
 * Also here, because they are the ladder's preconditions rather than a
 * separate subject: the Con.ID allocator (spec SS4(t) + ch. 2's CDL rule) and
 * the CDL dispatch that a stale or foreign Con.ID must NOT survive.
 */

#include "cluster_test.h"
#include "scs_test_harness.h"

static struct scsh_node a_node;
static struct scsh_node b_node;
static struct scsh_sysap a_sysap;
static struct scsh_sysap b_sysap;

static void rig(int b_decision)
{
	scsh_wire_reset();
	scsh_node_init(&a_node, 0x0101u, 0x33a0u);
	scsh_node_init(&b_node, 0x0202u, 0x8fd2u);
	scsh_sysap_init(&a_sysap, &a_node);
	scsh_sysap_init(&b_sysap, &b_node);
	b_sysap.connect_decision = b_decision;
	(void)scs_fsm_listen(&b_node.fsm, scsh_name_b, &b_sysap.ops, 10u);
	scsh_link(&a_node, &b_node);
}

/* ------------------------------------------------------------------ *
 * 1. The Con.ID allocator -- vms_scs_fsm.h SS4
 * ------------------------------------------------------------------ */
static void t_conid_allocator(void)
{
	struct scsh_node n;
	struct scsh_sysap s;
	struct scs_connect_args args;
	vms_conid_t first = 0u, second = 0u;
	uint32_t i;
	uint8_t *p = (uint8_t *)&args;

	printf("-- Con.ID allocator (SS4(t) + ch. 2's CDL index)\n");
	scsh_wire_reset();
	scsh_node_init(&n, 0x0303u, 0u);
	scsh_sysap_init(&s, &n);
	n.fsm.conid.seeded = 0u;   /* undo the harness seed */

	for (i = 0; i < (uint32_t)sizeof(args); i++)
		p[i] = 0u;
	args.local_name = scsh_name_a;
	args.remote_name = scsh_name_b;
	args.sysap = &s.ops;
	args.dst = 0x0404u;
	args.initial_credits = 4u;

	ct_check_eq_u32((unsigned long)(-scs_fsm_connect(&n.fsm, &args, &first)),
			(unsigned long)(-SCS_ERR_NOCONID),
			"an UNSEEDED allocator refuses rather than minting 0");
	ct_check_eq_u32(n.n_tx, 0u, "and nothing went on the wire");

	scs_fsm_seed_conid(&n.fsm, 0x5b05u);
	ct_check(scs_fsm_connect(&n.fsm, &args, &first) == SCS_OK,
		 "a seeded allocator mints a Con.ID");
	/* ch. 2: the low 16 bits index the CDL. Slot 0 + 1 == 1. */
	ct_check_eq_u32(first & 0xffffu, 1u,
			"low half is the CDL index (+1: wire 0 means unbound)");
	ct_check_eq_u32(first >> 16, 0x5b05u, "high half is the boot seed");
	ct_check(scs_fsm_cdt_by_conid(&n.fsm, first) != (struct scs_cdt *)0,
		 "the CDL resolves it");

	/*
	 * Release the slot and go all the way round the CDL until the SAME
	 * slot comes up again: SS4(t)'s "a real node never repeats a Con.ID".
	 * (The allocator hands out slots round-robin, so a freed slot is NOT
	 * the next one taken -- which is itself the property that keeps a
	 * stale Con.ID from landing on a live connection while the ring is
	 * still turning.)
	 */
	scs_fsm_vc_down(&n.fsm, 0x0404u, 0u);
	ct_check(scs_fsm_cdt_by_conid(&n.fsm, first) == (struct scs_cdt *)0,
		 "a STALE Con.ID resolves to nothing after the slot is freed");

	for (i = 0; i < SCSH_CDL; i++) {
		vms_conid_t id = 0u;

		if (scs_fsm_connect(&n.fsm, &args, &id) != SCS_OK)
			break;
		if ((id & 0xffffu) == (first & 0xffffu))
			second = id;
	}
	ct_check(second != 0u, "the freed CDL slot comes round and is reused");
	ct_check_eq_u32(second & 0xffffu, first & 0xffffu, "same CDL slot");
	ct_check(second != first,
		 "but a DIFFERENT Con.ID: the reuse generation moved (SS4(t))");
	ct_check(scs_fsm_cdt_by_conid(&n.fsm, first) == (struct scs_cdt *)0,
		 "the stale value still resolves to nothing, not to its slot");
	ct_check(n.fsm.rx_conid_mismatch > 0u,
		 "and the stale-value refusal is COUNTED, never silent");
}

/* ------------------------------------------------------------------ *
 * 2. The initiator's rungs: CLOSED -> CONNECT_SENT -> ACCEPT_RCVD -> OPEN
 * ------------------------------------------------------------------ */
static void t_initiator_ladder(void)
{
	vms_conid_t a_conid = 0u;
	struct scs_cdt *cdt;

	printf("-- [CLOSED]LOCAL_CONNECT / [CONNECT_SENT]op1,op2 -> OPEN\n");
	rig(SCS_CONNECT_DEFER);

	{
		struct scs_connect_args args;
		uint32_t i;
		uint8_t *p = (uint8_t *)&args;

		for (i = 0; i < (uint32_t)sizeof(args); i++)
			p[i] = 0u;
		args.local_name = scsh_name_a;
		args.remote_name = scsh_name_b;
		args.sysap = &a_sysap.ops;
		args.dst = b_node.sysid;
		args.initial_credits = 6u;
		a_node.drop_tx = 1;    /* hold the op 0 so we can inspect */
		ct_check(scs_fsm_connect(&a_node.fsm, &args, &a_conid) == SCS_OK,
			 "scs_fsm_connect allocates a CDT and emits op 0");
	}
	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_CONNECT_SENT, "[CLOSED] -> CONNECT_SENT");
	ct_check_eq_u32(a_node.tx_op[0], SCS_MTYPE_CON_REQ, "verb 0 on the wire");
	ct_check_eq_u32(a_node.tx_credit[0], 6u,
			"op 0 carries the LEDGER's grant, not a constant");
	ct_check_eq_u32(a_node.armed[SCS_TIMER_CONNECT], 1u,
			"the connect timer is armed");

	/* op 1 CONNECT-ECHO: acknowledgement of receipt, NOT a state change. */
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_CON_RSP, a_conid,
			 0u, 0u, NULL, NULL);
	cdt = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)cdt->state, VMS_SCS_CDT_CONNECT_SENT,
			"[CONNECT_SENT]op1 stays CONNECT_SENT (SS4(m))");
	ct_check_eq_u32(cdt->echo_rcvd, 1u, "...and records the echo");

	/* op 2 CONNECT-RESPONSE binds the pair and carries the peer's grant. */
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_ACCP_REQ, a_conid,
			 0x8fd20001u, 8u, scsh_name_b, scsh_name_a);
	cdt = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)cdt->state, VMS_SCS_CDT_OPEN,
			"[CONNECT_SENT]op2 -> ACCEPT_RCVD -> OPEN");
	ct_check_eq_u32(cdt->remote_conid, 0x8fd20001u,
			"the peer's Con.ID was LEARNED off the wire");
	ct_check_eq_u32(cdt->remote_conid_valid, 1u, "and flagged valid");
	ct_check_eq_u32(cdt->credit_send, 8u,
			"Send Credit is the peer's grant -- the only source");
	ct_check_eq_u32(a_node.tx_op[a_node.n_tx - 1u], SCS_MTYPE_ACCP_RSP,
			"the LOAD-BEARING op-3 confirm went out (SS4(m))");
	ct_check_eq_u32(a_node.tx_credit[a_node.n_tx - 1u], 0u,
			"op 3 carries credit 0 (SS4(h)(1c), 100% of type 3)");
	ct_check_eq_u32(a_sysap.n_opened, 1u, "the SYSAP was told opened()");
	ct_check_eq_u32(a_node.cancelled[SCS_TIMER_CONNECT], 1u,
			"the connect timer was cancelled");
}

/* ------------------------------------------------------------------ *
 * 3. The acceptor's rungs: LISTEN -> CONNECT_RCVD -> (new CDT) ACCEPT_SENT
 * ------------------------------------------------------------------ */
static void t_acceptor_ladder(void)
{
	vms_conid_t listen_conid, conn_conid = 0u;
	struct scs_cdt *listen;

	printf("-- [LISTEN]op0 -> CONNECT_RCVD; LOCAL_ACCEPT -> ACCEPT_SENT\n");
	rig(SCS_CONNECT_DEFER);
	b_node.drop_tx = 1;

	scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_CON_REQ, 0u,
			 0x33a00001u, 6u, scsh_name_b, scsh_name_a);

	ct_check_eq_u32(b_sysap.n_connect_req, 1u,
			"the request reached the SYSAP through the SDIR queue");
	listen_conid = b_sysap.last_listen_conid;
	listen = scsh_cdt(&b_node, listen_conid);
	ct_check(listen != (struct scs_cdt *)0, "the LISTENING CDT exists");
	ct_check_eq_u32(listen->is_listening, 1u, "and is marked as one");
	ct_check_eq_u32((unsigned long)listen->state, VMS_SCS_CDT_CONNECT_RCVD,
			"[LISTEN]op0 -> CONNECT_RCVD (ch. 2)");
	ct_check_eq_u32(b_node.tx_op[0], SCS_MTYPE_CON_RSP,
			"EVERY accept emits the op-1 echo first (SS4(m))");
	ct_check_eq_u32(b_node.tx_credit[0], 0u, "the echo carries credit 0");

	/* A SECOND connect while the SYSAP is deciding: ch. 2's "busy". */
	scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_CON_REQ, 0u,
			 0x33a00002u, 6u, scsh_name_b, scsh_name_a);
	ct_check_eq_u32(b_node.fsm.connect_busy, 1u,
			"[CONNECT_RCVD]op0 is refused BUSY, not queued");
	ct_check_eq_u32(b_sysap.n_connect_req, 1u,
			"...and the SYSAP is not asked twice (ch. 2)");

	ct_check(scs_fsm_accept(&b_node.fsm, listen_conid, NULL,
				&conn_conid) == SCS_OK, "LOCAL_ACCEPT");
	ct_check(conn_conid != listen_conid,
		 "the connection gets its OWN CDT, not the listening one");
	ct_check_eq_u32((unsigned long)scsh_state(&b_node, conn_conid),
			VMS_SCS_CDT_ACCEPT_SENT, "-> ACCEPT_SENT");
	ct_check_eq_u32((unsigned long)listen->state, VMS_SCS_CDT_LISTEN,
			"the listening CDT is back in LISTEN (ch. 2)");
	ct_check_eq_u32(b_node.tx_op[b_node.n_tx - 1u], SCS_MTYPE_ACCP_REQ,
			"op 2 went out");
	ct_check_eq_u32(b_node.tx_credit[b_node.n_tx - 1u], 10u,
			"op 2 carries THIS SYSAP's grant from the ledger");

	/* op 3 confirm from the initiator completes it. */
	scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_ACCP_RSP, conn_conid,
			 0x33a00001u, 0u, NULL, NULL);
	ct_check_eq_u32((unsigned long)scsh_state(&b_node, conn_conid),
			VMS_SCS_CDT_OPEN, "[ACCEPT_SENT]op3 -> OPEN");
	ct_check_eq_u32(b_sysap.n_opened, 1u, "opened() fired");
}

/* ------------------------------------------------------------------ *
 * 4. The refusal rungs
 * ------------------------------------------------------------------ */
static void t_reject_ladder(void)
{
	vms_conid_t a_conid = 0u, listen_conid;

	printf("-- REJECT: [CONNECT_SENT]op4 and [CONNECT_RCVD]LOCAL_REJECT\n");
	rig(SCS_CONNECT_DEFER);
	a_node.drop_tx = 1;
	b_node.drop_tx = 1;

	(void)scsh_open_pair(&a_node, &b_node, 6u, &a_conid);
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_REJ_REQ, a_conid,
			 0u, 0u, NULL, NULL);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "[CONNECT_SENT]op4 closes the connection");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_REJECTED,
			"...with reason REJECTED");
	ct_check_eq_u32(a_node.tx_op[a_node.n_tx - 1u], SCS_MTYPE_REJ_RSP,
			"and answers op 5");

	/* The SYSAP's own refusal. */
	scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_CON_REQ, 0u,
			 0x33a00007u, 6u, scsh_name_b, scsh_name_a);
	listen_conid = b_sysap.last_listen_conid;
	ct_check(scs_fsm_reject(&b_node.fsm, listen_conid) == SCS_OK,
		 "[CONNECT_RCVD]LOCAL_REJECT");
	ct_check_eq_u32(b_node.tx_op[b_node.n_tx - 1u], SCS_MTYPE_REJ_REQ,
			"op 4 went out");
	ct_check_eq_u32((unsigned long)scsh_cdt(&b_node, listen_conid)->state,
			VMS_SCS_CDT_LISTEN, "the listener is back in LISTEN");
	ct_check_eq_u32(b_node.fsm.connects_rejected, 1u, "counted");

	/* A connect naming a SYSAP nobody listens on. */
	{
		static const uint8_t nobody[VMS_SCS_PROCNAME_LEN] =
			{ 'M','S','C','P','$','T','A','P','E',' ',' ',' ',
			  ' ',' ',' ',' ' };
		uint32_t before = b_node.fsm.connect_no_sysap;

		scsh_inject_ctrl(&b_node, a_node.sysid, SCS_MTYPE_CON_REQ, 0u,
				 0x33a00008u, 6u, nobody, scsh_name_a);
		ct_check_eq_u32(b_node.fsm.connect_no_sysap, before + 1u,
				"a connect to an unlistened name is counted");
	}
}

/* ------------------------------------------------------------------ *
 * 5. ACCEPT_RCVD is a REAL state: the confirm that could not be sent
 * ------------------------------------------------------------------ */
static void t_accept_rcvd_is_real(void)
{
	vms_conid_t a_conid = 0u;

	printf("-- [ACCEPT_RCVD] the confirm is load-bearing\n");
	rig(SCS_CONNECT_DEFER);
	a_node.drop_tx = 1;
	(void)scsh_open_pair(&a_node, &b_node, 6u, &a_conid);

	a_node.fail_ctrl = 1;    /* the op-3 confirm will not go out */
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_ACCP_REQ, a_conid,
			 0x8fd20001u, 8u, scsh_name_b, scsh_name_a);
	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_ACCEPT_RCVD,
			"a confirm that did not go out leaves ACCEPT_RCVD");
	ct_check_eq_u32(a_sysap.n_opened, 0u,
			"and the connection is NOT declared open (SS4(m))");

	/* The connect timer retries it; this time it succeeds. */
	scs_fsm_timer(&a_node.fsm, SCS_TIMER_CONNECT,
		      a_node.last_key[SCS_TIMER_CONNECT]);
	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_OPEN,
			"[ACCEPT_RCVD]TIMER_CONNECT retries and opens it");
	ct_check_eq_u32(a_sysap.n_opened, 1u, "opened() fired then");
}

static void t_accept_rcvd_gives_up(void)
{
	vms_conid_t a_conid = 0u;

	printf("-- [ACCEPT_RCVD] a confirm that never goes out closes\n");
	rig(SCS_CONNECT_DEFER);
	a_node.drop_tx = 1;
	(void)scsh_open_pair(&a_node, &b_node, 6u, &a_conid);

	a_node.fail_ctrl = 2;
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_ACCP_REQ, a_conid,
			 0x8fd20001u, 8u, scsh_name_b, scsh_name_a);
	scs_fsm_timer(&a_node.fsm, SCS_TIMER_CONNECT,
		      a_node.last_key[SCS_TIMER_CONNECT]);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "the CDT is closed, not left half-open");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_TIMEOUT,
			"...with reason TIMEOUT");
}

/* ------------------------------------------------------------------ *
 * 6. The connect timer on the outstanding op 0
 * ------------------------------------------------------------------ */
static void t_connect_timeout(void)
{
	vms_conid_t a_conid = 0u;

	printf("-- [CONNECT_SENT]TIMER_CONNECT\n");
	rig(SCS_CONNECT_DEFER);
	a_node.drop_tx = 1;
	(void)scsh_open_pair(&a_node, &b_node, 6u, &a_conid);

	scs_fsm_timer(&a_node.fsm, SCS_TIMER_CONNECT,
		      a_node.last_key[SCS_TIMER_CONNECT]);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "an unanswered connect closes rather than stalling");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_TIMEOUT,
			"reason TIMEOUT");
	ct_check_eq_u32(a_sysap.n_closed, 1u, "the SYSAP was told exactly once");
}

/* ------------------------------------------------------------------ *
 * 7. The full two-node ladder, both ends, end to end
 * ------------------------------------------------------------------ */
static void t_two_node_open(vms_conid_t *out_a, vms_conid_t *out_b)
{
	vms_conid_t a_conid = 0u;

	rig(0 /* accept immediately */);
	(void)scsh_open_pair(&a_node, &b_node, 6u, &a_conid);
	*out_a = a_conid;
	*out_b = b_sysap.last_opened_conid;
}

static void t_end_to_end(void)
{
	vms_conid_t a_conid, b_conid;

	printf("-- two live FSMs walk the whole connect ladder\n");
	t_two_node_open(&a_conid, &b_conid);

	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_OPEN, "initiator OPEN");
	ct_check_eq_u32((unsigned long)scsh_state(&b_node, b_conid),
			VMS_SCS_CDT_OPEN, "acceptor OPEN");
	/* SS4(m)'s own order: 0, 1, 2, 3. */
	ct_check_eq_u32(a_node.tx_op[0], SCS_MTYPE_CON_REQ, "A: op 0");
	ct_check_eq_u32(b_node.tx_op[0], SCS_MTYPE_CON_RSP, "B: op 1 first");
	ct_check_eq_u32(b_node.tx_op[1], SCS_MTYPE_ACCP_REQ, "B: op 2");
	ct_check_eq_u32(a_node.tx_op[1], SCS_MTYPE_ACCP_RSP, "A: op 3");
	{
		struct scs_cdt *ca = scsh_cdt(&a_node, a_conid);
		struct scs_cdt *cb = scsh_cdt(&b_node, b_conid);

		ct_check_eq_u32(ca->credit_send, cb->credit_grant,
				"A's Send Credit == B's extended grant");
		ct_check_eq_u32(cb->credit_send, ca->credit_grant,
				"B's Send Credit == A's extended grant");
		ct_check(ca->remote_conid == cb->local_conid,
			 "A's remote Con.ID IS B's local one (ch. 2 Fig 2-17)");
		ct_check(cb->remote_conid == ca->local_conid,
			 "and the mirror image holds");
	}
}

/* ------------------------------------------------------------------ *
 * 8. The teardown rungs: DISC_SENT / DISC_RCVD / DISC_MATCH / CLOSED
 * ------------------------------------------------------------------ */
static void t_teardown_states(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;

	printf("-- teardown: OPEN -> DISC_SENT -> DISC_MATCH -> CLOSED\n");
	t_two_node_open(&a_conid, &b_conid);
	a_node.drop_tx = 1;      /* drive A's side by injection */

	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) == SCS_OK,
		 "[OPEN]LOCAL_DISCONNECT");
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)ca->state, VMS_SCS_CDT_OPEN,
			"still OPEN: the op 8 goes first (SS4(h)(1f))");
	ct_check_eq_u32(ca->credit_msg_sent, 1u, "the op 8 is outstanding");

	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_CR_RSP, a_conid,
			 ca->remote_conid, 1u, NULL, NULL);
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)ca->state, VMS_SCS_CDT_DISC_SENT,
			"the op 9 releases the op 6 -> DISC_SENT");

	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_DISC_RSP, a_conid,
			 ca->remote_conid, 0u, NULL, NULL);
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)ca->state, VMS_SCS_CDT_DISC_MATCH,
			"[DISC_SENT]op7 -> DISC_MATCH (ours matched)");

	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_DISC_REQ, a_conid,
			 ca->remote_conid, 0u, NULL, NULL);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "[DISC_MATCH]op6 -> answered and CLOSED");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_LOCAL,
			"reason: we initiated it");
}

static void t_teardown_peer_first(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;

	printf("-- teardown the other way: OPEN -> DISC_RCVD -> CLOSED\n");
	t_two_node_open(&a_conid, &b_conid);
	a_node.drop_tx = 1;
	ca = scsh_cdt(&a_node, a_conid);

	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_DISC_REQ, a_conid,
			 ca->remote_conid, 0u, NULL, NULL);
	ca = scsh_cdt(&a_node, a_conid);
	ct_check_eq_u32((unsigned long)ca->state, VMS_SCS_CDT_DISC_RCVD,
			"[OPEN]op6 -> DISC_RCVD");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_DISC_RSP), 1u,
			"we answered with op 7");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_DISC_REQ), 1u,
			"and sent our OWN op 6 (SS4(m): bidirectional)");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_CR_REQ), 0u,
			"NO op 8: the type-8 sender is the teardown's opener");

	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_DISC_RSP, a_conid,
			 ca->remote_conid, 0u, NULL, NULL);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "[DISC_RCVD]op7 -> CLOSED");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_REMOTE,
			"reason: the peer initiated it");
}

static void t_teardown_timeout(void)
{
	vms_conid_t a_conid, b_conid;
	struct scs_cdt *ca;

	printf("-- [DISC_SENT]TIMER_DISCONNECT forces the close\n");
	t_two_node_open(&a_conid, &b_conid);
	a_node.drop_tx = 1;
	(void)scs_fsm_disconnect(&a_node.fsm, a_conid);
	ca = scsh_cdt(&a_node, a_conid);
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_CR_RSP, a_conid,
			 ca->remote_conid, 1u, NULL, NULL);
	scs_fsm_timer(&a_node.fsm, SCS_TIMER_DISCONNECT,
		      a_node.last_key[SCS_TIMER_DISCONNECT]);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "an unmatched teardown does not hang the connection open");
	ct_check_eq_u32(a_sysap.last_close_reason, SCS_CLOSE_TIMEOUT,
			"reason TIMEOUT");
}

/* ------------------------------------------------------------------ *
 * 9. Data on an open connection, and data DURING a teardown
 * ------------------------------------------------------------------ */
static void t_data_rungs(void)
{
	vms_conid_t a_conid, b_conid;
	uint8_t body[SCS_SYSAP_BODY_LEN];
	struct scs_cdt *ca;
	uint32_t i;

	printf("-- [OPEN]LOCAL_SEND / [OPEN]op10 / [DISC_SENT]op10\n");
	t_two_node_open(&a_conid, &b_conid);
	for (i = 0; i < SCS_SYSAP_BODY_LEN; i++)
		body[i] = (uint8_t)(i & 0xffu);

	ct_check(scs_fsm_send_msg(&a_node.fsm, a_conid, body,
				  SCS_SYSAP_BODY_LEN) == SCS_OK,
		 "[OPEN]LOCAL_SEND transmits");
	(void)scsh_pump();
	ct_check_eq_u32(b_sysap.n_message, 1u,
			"it reached the peer SYSAP through the CDL");
	ct_check_eq_u32(b_sysap.last_msg[7], 7u,
			"...with the SYSAP's OWN 132 bytes, unshifted");

	/* A message arriving mid-teardown is still real data. */
	a_node.drop_tx = 1;
	(void)scs_fsm_disconnect(&a_node.fsm, a_conid);
	ca = scsh_cdt(&a_node, a_conid);
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_CR_RSP, a_conid,
			 ca->remote_conid, 1u, NULL, NULL);
	ct_check_eq_u32((unsigned long)scsh_state(&a_node, a_conid),
			VMS_SCS_CDT_DISC_SENT, "we are in DISC_SENT");
	scsh_inject_msg(&a_node, b_node.sysid, a_conid, ca->remote_conid,
			1u, 0x5au);
	ct_check_eq_u32(a_sysap.n_message, 1u,
			"[DISC_SENT]op10 is delivered, not dropped");
}

/* ------------------------------------------------------------------ *
 * 10. A cell with no edge is COUNTED, and a foreign Con.ID is refused
 * ------------------------------------------------------------------ */
static void t_no_edge_and_foreign(void)
{
	vms_conid_t a_conid, b_conid;
	uint32_t before;

	printf("-- ignored cells and CDL refusals are counted, never silent\n");
	t_two_node_open(&a_conid, &b_conid);
	a_node.drop_tx = 1;

	before = a_node.fsm.ignored_events;
	/* op 2 on an OPEN connection: the table has no such edge. */
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_ACCP_REQ, a_conid,
			 0x8fd20001u, 4u, scsh_name_b, scsh_name_a);
	ct_check_eq_u32(a_node.fsm.ignored_events, before + 1u,
			"[OPEN]op2 has no edge -> ignored_events");

	before = a_node.fsm.rx_no_cdt;
	scsh_inject_ctrl(&a_node, b_node.sysid, SCS_MTYPE_DISC_RSP,
			 0xdeadbeefu, 0x8fd20001u, 0u, NULL, NULL);
	ct_check(a_node.fsm.rx_no_cdt > before || a_node.fsm.rx_conid_mismatch,
		 "a Con.ID this node never minted resolves to nothing");

	before = a_node.fsm.rx_conid_mismatch;
	/* Our own Con.ID, but arriving from a system it was never bound to. */
	scsh_inject_ctrl(&a_node, 0x0909u, SCS_MTYPE_DISC_RSP, a_conid,
			 0x8fd20001u, 0u, NULL, NULL);
	ct_check_eq_u32(a_node.fsm.rx_conid_mismatch, before + 1u,
			"a valid Con.ID from the WRONG system is refused");
}

int main(void)
{
	t_conid_allocator();
	t_initiator_ladder();
	t_acceptor_ladder();
	t_reject_ladder();
	t_accept_rcvd_is_real();
	t_accept_rcvd_gives_up();
	t_connect_timeout();
	t_end_to_end();
	t_teardown_states();
	t_teardown_peer_first();
	t_teardown_timeout();
	t_data_rungs();
	t_no_edge_and_foreign();
	return ct_summary("test_scs_fsm");
}
