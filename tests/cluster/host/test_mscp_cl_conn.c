/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_mscp_cl_conn.c - the MSCP disk class driver's CONNECT-ADMISSION FSM
 * (src/kernel-core/vms_mscp_cl_conn_fsm.{c,h}), test-ladder rung R1.
 *
 * *** WHAT THIS FILE IS FOR: E64, AND KEEPING IT FIXED ***
 *
 * Against the live 2-node VAX cluster, OVMX presented 711 `MSCP$DISK`
 * CONNECT_REQ frames and not one directory inquiry of its own -- because this
 * driver's beat swept every system SCS had a circuit to and connected, with no
 * name resolution and from CLUSTER_START, while the connection manager's join
 * was still driving. The FIRST sequenced frame OVMX ever sent one of the two
 * VAXes was an unresolvable `MSCP$DISK` connect; that member's `recv_ack`
 * stayed 0 for the whole 1620-second run and the join never advanced past its
 * own directory round.
 *
 * The two rules that fixes it are asserted here as ORDERING, so a regression
 * to either shape reds this file rather than a real cluster:
 *
 *   1. NOTHING is originated while this node is still joining -- not a
 *      connect and not even a directory inquiry, because an inquiry opens a
 *      real SCS$DIRECTORY connection and takes a slot in the same shared
 *      per-channel send sequence (spec sec 4(L), "Shared-sequence deadlock").
 *   2. A connect is issued ONLY after that member really answered a directory
 *      inquiry about `MSCP$DISK` affirmatively. Not on assumption, and not on
 *      silence.
 *
 * GROUNDING. *VAXcluster Principles* (Davis 1993) SS2.11, pp. 2-48..2-51: the
 * SCS Directory Service, the Process Poller, the transient connection, and
 * "SYSAP_B is not notified about the absence of SYSAP_W" -- which is why an
 * unanswered inquiry here is silence and never absence. Host-only transcript,
 * page cites only (clean-room rule 8). Wire: docs/cluster-protocol-spec.md
 * SS4(L)(1) and its shared-sequence deadlock; the 2->3 established-join
 * reference in docs/design-cluster-join-choreography.md.
 *
 * The FSM is PURE, so every one of these drives a thirty-second retry window
 * in microseconds off an injected clock. No wire, no daemon, no boot.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_mscp_cl_conn_fsm.h"

#define VAX1_SYSID 1025u
#define VAX2_SYSID 1026u

#define MAX_LOG 32u

/* ------------------------------------------------------------------ *
 * The bed: the FSM plus a recorder for every act it takes. The recorder
 * NEVER answers on the FSM's behalf -- an inquiry is recorded and left
 * unanswered unless a test explicitly delivers a peer's answer.
 * ------------------------------------------------------------------ */
struct bed {
	struct mscp_cl_conn      c;
	struct mscp_cl_conn_ops  ops;
	struct mscp_cl_conn_peer peers[4];

	int      joined;
	uint32_t now_ms;

	/* what the FSM asked for, in the order it asked */
	uint32_t        n_acts;
	char            act[MAX_LOG];         /* 'I' inquire, 'C' connect */
	vms_scs_sysid_t act_sysid[MAX_LOG];

	/* what the bed answers with */
	int         refuse_connect;
	int         refuse_inquiry;
	vms_conid_t next_conid;
	uint32_t    join_owns;                /* sysid the join holds, or 0 */
};

static struct bed g;

static const uint8_t name_mscp_disk[VMS_SCS_PROCNAME_LEN] = {
	'M', 'S', 'C', 'P', '$', 'D', 'I', 'S', 'K',
	' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const uint8_t name_vaxcluster[VMS_SCS_PROCNAME_LEN] = {
	'V', 'M', 'S', '$', 'V', 'A', 'X', 'c', 'l', 'u', 's', 't', 'e', 'r',
	' ', ' '
};

static void act(char what, vms_scs_sysid_t sysid)
{
	if (g.n_acts >= MAX_LOG)
		return;
	g.act[g.n_acts] = what;
	g.act_sysid[g.n_acts] = sysid;
	g.n_acts++;
}

static int op_joined(void *ctx)
{
	(void)ctx;
	return g.joined;
}

static int op_join_holds(void *ctx, vms_scs_sysid_t dst)
{
	(void)ctx;
	return g.join_owns != 0u && g.join_owns == dst;
}

static int op_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx;
	ct_check(memcmp(name, name_mscp_disk, VMS_SCS_PROCNAME_LEN) == 0,
		 "the sweep only ever asks about MSCP$DISK");
	act('I', dst);
	return g.refuse_inquiry ? -1 : 0;
}

static int op_connect(void *ctx, vms_scs_sysid_t dst, vms_conid_t *out)
{
	(void)ctx;
	act('C', dst);
	if (g.refuse_connect)
		return -1;
	*out = g.next_conid++;
	return 0;
}

static uint32_t op_now_ms(void *ctx)
{
	(void)ctx;
	return g.now_ms;
}

static void op_log(void *ctx, const char *msg)
{
	(void)ctx; (void)msg;
}

static void bed_init(int joined)
{
	memset(&g, 0, sizeof(g));
	g.joined = joined;
	g.now_ms = 100000u;
	g.next_conid = 0x39e50005u;

	g.ops.joined = op_joined;
	g.ops.join_holds = op_join_holds;
	g.ops.dir_inquire = op_dir_inquire;
	g.ops.connect = op_connect;
	g.ops.now_ms = op_now_ms;
	g.ops.log = op_log;
	g.ops.ctx = &g;

	ct_check_eq_u32((unsigned long)mscp_cl_conn_init(&g.c, &g.ops,
							name_mscp_disk),
			0u, "the admission FSM binds");
	ct_check_eq_u32((unsigned long)mscp_cl_conn_bind_peers(&g.c, g.peers,
							       4u),
			0u, "and its peer table binds");
}

static uint32_t sweep2(void)
{
	vms_scs_sysid_t peers[2] = { VAX1_SYSID, VAX2_SYSID };

	return mscp_cl_conn_sweep(&g.c, peers, 2u);
}

static const struct mscp_cl_conn_peer *row(vms_scs_sysid_t s)
{
	return mscp_cl_conn_by_sysid(&g.c, s);
}

/* ==========================================================================
 * 1. RULE 2 -- while this node is JOINING, this driver originates NOTHING
 * ========================================================================== */
static void test_joining_originates_nothing(void)
{
	uint32_t i;

	printf("\n-- E64 rule 2: the join drives alone (spec sec 4(L)(1)) --\n");
	bed_init(0);

	/* Twenty beats -- twenty seconds of a real join window. */
	for (i = 0; i < 20u; i++) {
		ct_check_eq_u32(sweep2(), 0u,
				"a beat during the join sweeps nobody");
		g.now_ms += 1000u;
	}
	ct_check_eq_u32(g.n_acts, 0u,
			"NOT ONE MSCP$DISK connect and NOT ONE directory "
			"inquiry while this node is still joining -- the "
			"exact 711-frame flood E64 measured");
	ct_check_eq_u32(g.c.connects, 0u, "no connect was counted either");
	ct_check_eq_u32(g.c.inquiries, 0u, "and no inquiry was");
	ct_check_eq_u32(g.c.deferred_joining, 40u,
			"every deferral is COUNTED, never silent");
	ct_check(row(VAX1_SYSID) == NULL,
		 "and no member row is created behind the join's back");
}

/* ==========================================================================
 * 2. RULE 1 -- LOOKUP BEFORE CONNECT, in that order, per member
 * ========================================================================== */
static void test_lookup_precedes_connect(void)
{
	printf("\n-- E64 rule 1: the name is RESOLVED before the connect --\n");
	bed_init(1);

	ct_check_eq_u32(sweep2(), 2u, "both members are swept once a member");
	ct_check_eq_u32(g.n_acts, 2u, "two acts on the first beat");
	ct_check(g.act[0] == 'I' && g.act[1] == 'I',
		 "and BOTH are directory inquiries -- nothing is connected "
		 "before a name is resolved (spec sec 4(L))");
	ct_check_eq_u32(g.c.inquiries, 2u, "both inquiries are counted");
	ct_check_eq_u32(g.c.connects, 0u, "and no connect has gone out");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_ASKING,
			"VAX1's leg is ASKING");

	/* Beats while the answer is outstanding must not re-ask or connect. */
	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.n_acts, 2u,
			"a beat with an inquiry outstanding originates nothing");

	/* VAX1 really answers YES. */
	mscp_cl_conn_dir_result(&g.c, VAX1_SYSID, name_mscp_disk, 1);
	ct_check_eq_u32(g.c.hits, 1u, "the HIT is recorded");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_PRESENT,
			"and that member's leg is PRESENT");
	ct_check_eq_u32(g.n_acts, 2u,
			"an answer alone connects nothing -- the beat does");

	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.n_acts, 3u, "the next beat acts once");
	ct_check(g.act[2] == 'C' && g.act_sysid[2] == VAX1_SYSID,
		 "and it is the MSCP$DISK connect to the member that said YES");
	ct_check_eq_u32(g.c.connects, 1u, "one connect, counted");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_CONNECTING,
			"VAX1's leg is CONNECTING");
	ct_check(row(VAX1_SYSID)->conid != 0u,
		 "carrying the Con.ID the ALLOCATOR minted, not a made-up one");
	ct_check_eq_u32(row(VAX2_SYSID)->state, MSCP_CL_CONN_ASKING,
			"the member that has NOT answered is still only ASKING");
}

/* ==========================================================================
 * 3. "NOT PRESENT HERE" is an answer; SILENCE is not
 * ========================================================================== */
static void test_miss_is_an_answer(void)
{
	printf("\n-- a real NOT PRESENT HERE suppresses the connect --\n");
	bed_init(1);
	(void)sweep2();

	mscp_cl_conn_dir_result(&g.c, VAX1_SYSID, name_mscp_disk, 0);
	ct_check_eq_u32(g.c.misses, 1u, "the member's literal NO is recorded");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_ABSENT,
			"that leg is ABSENT (MSCP_LOAD=0: a real config)");

	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.c.connects, 0u,
			"and NO connect is opened to a member that said no");

	/* It is asked again after the retry period -- a member may mount its
	 * first served volume later. */
	g.now_ms += MSCP_CL_CONN_RETRY_MS;
	(void)sweep2();
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_IDLE,
			"the retry period returns it to IDLE");
	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.c.inquiries, 3u,
			"and it is ASKED again -- never connected on a guess");
}

static void test_silence_is_not_absence(void)
{
	printf("\n-- an unanswered inquiry is silence, not a No --\n");
	bed_init(1);
	(void)sweep2();
	ct_check_eq_u32(g.c.inquiries, 2u, "two inquiries went out");

	g.now_ms += MSCP_CL_CONN_ASK_TIMEOUT_MS;
	(void)sweep2();
	ct_check_eq_u32(g.c.unanswered, 2u,
			"both time out and are COUNTED (p. 2-51)");
	ct_check_eq_u32(g.c.misses, 0u,
			"and NEITHER is recorded as the member saying no");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_IDLE,
			"the leg is back to IDLE, not ABSENT");
	ct_check_eq_u32(g.c.connects, 0u, "nothing was connected on silence");
}

/* ==========================================================================
 * 4. One disk-client connection per member
 * ========================================================================== */
static void test_join_owned_leg_is_not_duplicated(void)
{
	printf("\n-- the join's own MSCP$DISK leg is not connected twice --\n");
	bed_init(1);
	g.join_owns = VAX1_SYSID;

	ct_check_eq_u32(sweep2(), 1u, "only the member the join does NOT hold");
	ct_check_eq_u32(g.n_acts, 1u, "one act");
	ct_check(g.act_sysid[0] == VAX2_SYSID, "and it is about VAX2");
	ct_check_eq_u32(g.c.deferred_join_owned, 1u,
			"declining to double-connect is COUNTED");
	ct_check(row(VAX1_SYSID) == NULL,
		 "and no second row is opened for the join's own member");
}

/* ==========================================================================
 * 5. Refusals, closes and the retry clock
 * ========================================================================== */
static void test_refused_connect_backs_off(void)
{
	printf("\n-- a refused connect backs off, it does not hammer --\n");
	bed_init(1);
	g.refuse_connect = 1;
	(void)sweep2();
	mscp_cl_conn_dir_result(&g.c, VAX1_SYSID, name_mscp_disk, 1);

	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.c.connect_refusals, 1u, "the refusal is counted");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_IDLE,
			"and the leg is back to IDLE");

	g.now_ms += 1000u;
	(void)sweep2();
	ct_check_eq_u32(g.c.connect_refusals, 1u,
			"the next beat does NOT retry a second later");

	g.now_ms += MSCP_CL_CONN_RETRY_MS;
	(void)sweep2();
	ct_check_eq_u32(g.c.inquiries, 3u,
			"after the retry period it RE-RESOLVES the name first");
}

static void test_open_then_close_re_resolves(void)
{
	vms_conid_t conid;

	printf("\n-- a connection that goes away is re-resolved, not reopened --\n");
	bed_init(1);
	(void)sweep2();
	mscp_cl_conn_dir_result(&g.c, VAX1_SYSID, name_mscp_disk, 1);
	g.now_ms += 1000u;
	(void)sweep2();
	conid = row(VAX1_SYSID)->conid;

	mscp_cl_conn_opened(&g.c, conid);
	ct_check_eq_u32(g.c.opens, 1u, "the OPEN is counted");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_OPEN,
			"and the leg is OPEN");

	/* A second OPEN for the same Con.ID has no edge: counted, never a
	 * second conn_open into the driver FSM. */
	mscp_cl_conn_opened(&g.c, conid);
	ct_check_eq_u32(g.c.opens, 1u, "a repeated OPEN opens nothing twice");
	ct_check_eq_u32(g.c.ignored_events, 1u, "... and is COUNTED");

	g.now_ms += 10000u;
	(void)sweep2();
	ct_check_eq_u32(g.c.connects, 1u,
			"beats over an OPEN leg originate nothing");
	ct_check_eq_u32(g.c.ignored_events, 1u,
			"and an OPEN member is not an ignored event once a "
			"second for the life of the cluster");

	mscp_cl_conn_closed(&g.c, conid);
	ct_check_eq_u32(g.c.closes, 1u, "the close is counted");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_IDLE,
			"and the leg is IDLE -- the HIT is NOT reused");
	ct_check_eq_u32(row(VAX1_SYSID)->conid, 0u, "the Con.ID is dropped");

	g.now_ms += MSCP_CL_CONN_RETRY_MS;
	(void)sweep2();
	ct_check(g.act[g.n_acts - 1u] == 'I',
		 "the next thing this node does is ASK again, not connect");
}

/* ==========================================================================
 * 6. Discipline: a lost circuit, a stray answer, a full table
 * ========================================================================== */
static void test_lost_circuit_drops_the_row(void)
{
	vms_scs_sysid_t one[1] = { VAX1_SYSID };

	printf("\n-- a member the port lost keeps no row --\n");
	bed_init(1);
	(void)sweep2();
	ct_check(row(VAX2_SYSID) != NULL, "VAX2 had a row");

	(void)mscp_cl_conn_sweep(&g.c, one, 1u);
	ct_check(row(VAX2_SYSID) == NULL,
		 "and it is gone with the circuit -- a row that outlives the "
		 "circuit is a claim about a system this node cannot reach");
}

static void test_stray_answers_advance_nothing(void)
{
	printf("\n-- an answer nobody asked for advances nothing --\n");
	bed_init(1);
	(void)sweep2();

	mscp_cl_conn_dir_result(&g.c, VAX1_SYSID, name_vaxcluster, 1);
	ct_check_eq_u32(g.c.hits, 0u,
			"an answer about a DIFFERENT name is not our HIT");
	ct_check_eq_u32(row(VAX1_SYSID)->state, MSCP_CL_CONN_ASKING,
			"and the leg has not moved");

	mscp_cl_conn_dir_result(&g.c, 9999u, name_mscp_disk, 1);
	ct_check_eq_u32(g.c.hits, 0u,
			"an answer from a system with no row is not ours");
	ct_check_eq_u32(g.c.ignored_events, 2u, "both are COUNTED");

	mscp_cl_conn_opened(&g.c, 0x12345678u);
	ct_check_eq_u32(g.c.ignored_events, 3u,
			"and so is an OPEN for a Con.ID this driver never minted");
}

static void test_table_full_and_null_safety(void)
{
	vms_scs_sysid_t many[6] = { 1025u, 1026u, 1027u, 1028u, 1029u, 1030u };

	printf("\n-- a full table refuses honestly; NULLs do not crash --\n");
	bed_init(1);
	(void)mscp_cl_conn_sweep(&g.c, many, 6u);
	ct_check_eq_u32(g.c.no_peer_slot, 2u,
			"the two members past the table are COUNTED, not lost");
	ct_check_eq_u32(g.c.inquiries, 4u, "the four that fit were asked");

	ct_check_eq_u32(mscp_cl_conn_sweep(NULL, many, 6u), 0u,
			"a NULL context sweeps nothing");
	ct_check_eq_u32(mscp_cl_conn_sweep(&g.c, NULL, 6u), 0u,
			"and a NULL peer list sweeps nobody");
	mscp_cl_conn_dir_result(NULL, 1025u, name_mscp_disk, 1);
	mscp_cl_conn_closed(NULL, 1u);
	ct_check(mscp_cl_conn_by_sysid(&g.c, 0u) == NULL,
		 "sysid 0 is not a member");
	ct_check(mscp_cl_conn_by_conid(&g.c, 0u) == NULL,
		 "and Con.ID 0 is not a connection");
	ct_check(mscp_cl_conn_init(&g.c, &g.ops, NULL) != 0,
		 "and this FSM refuses to run with no SYSAP name to ask about");
}

/* An unbound `joined` op reads as NOT joined: the safe direction, because a
 * wiring with no membership source must not originate on a live channel. */
static void test_unbound_membership_reads_as_not_joined(void)
{
	printf("\n-- an unbound membership read defers, it does not assume --\n");
	bed_init(1);
	g.ops.joined = NULL;
	ct_check_eq_u32(sweep2(), 0u, "nothing is swept");
	ct_check_eq_u32(g.n_acts, 0u, "and nothing is originated");
}

static void test_state_names(void)
{
	printf("\n-- every state has a name for the diagnostics --\n");
	ct_check(strcmp(mscp_cl_conn_state_name(MSCP_CL_CONN_IDLE),
			"IDLE") == 0, "IDLE");
	ct_check(strcmp(mscp_cl_conn_state_name(MSCP_CL_CONN_OPEN),
			"OPEN") == 0, "OPEN");
	ct_check(strcmp(mscp_cl_conn_state_name(
				 (enum mscp_cl_conn_state)99), "?") == 0,
		 "and an out-of-range state is '?', never an overread");
}

int main(void)
{
	printf("=== test_mscp_cl_conn: E64 connect admission "
	       "(lookup-before-connect; the join drives alone) ===\n");
	test_joining_originates_nothing();
	test_lookup_precedes_connect();
	test_miss_is_an_answer();
	test_silence_is_not_absence();
	test_join_owned_leg_is_not_duplicated();
	test_refused_connect_backs_off();
	test_open_then_close_re_resolves();
	test_lost_circuit_drops_the_row();
	test_stray_answers_advance_nothing();
	test_table_full_and_null_safety();
	test_unbound_membership_reads_as_not_joined();
	test_state_names();
	return ct_summary("test_mscp_cl_conn");
}
