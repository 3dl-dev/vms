/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_scs_fsm_depart.c - R1: the CLEAN CLUSTER DEPARTURE (rd vms-abd).
 *
 * ---------------------------------------------------------------------------
 * WHAT REGRESSED, AND WHAT THIS LOCKS
 * ---------------------------------------------------------------------------
 * A real VMS node leaving through SHUTDOWN.COM announces its departure BOTH
 * ways: the port's "last gasp" datagram (VAXcluster Principles p. 7-29) and a
 * SYMMETRIC DISCONNECT_REQUEST on every open connection (p. 2-26/27). The
 * retired scsd.c daemon did both. The executive-resident stack that replaced it
 * did NEITHER -- not because vms_cnxman_stop() forgot a call, but because
 * nothing ever called vms_cnxman_stop(): there was no clean shutdown sequence
 * at all. A departing OVMX node simply vanished, and every survivor carried a
 * dead CSB until RECNXINTERVAL expired.
 *
 * VMS_IOCTL_CLUSTER_STOP -> vms_cnxman_depart() is the fix. This is its R1
 * rung, in the TWO-PROOF shape tests/cluster/host/CMakeLists.txt established
 * for a glue TU that is not host-linkable (vms_cnxman.c names exec_kbackend.h
 * and the FC-P0.5 fork API, test_cnxman_glue.c's own precedent):
 *
 *   PROOF 1 -- THE ALGORITHM, against the REAL scs_fsm through the FC-P2.2
 *   harness. The departure's whole wire contract is the SCS FSM's: which
 *   connections may be disconnected at all, what the teardown puts on the wire,
 *   what the drain's predicate reads, and -- the never-crash edge -- what
 *   happens when the peer never answers. All four are exercised here against
 *   the shipping state machine, byte-shaped through the codec.
 *
 *   PROOF 2 -- THE WIRING, source-scanned out of the shipping vms_cnxman.c,
 *   vms_devtab.c and ovmx_init.c (OVMX_KCORE_DIR / OVMX_INIT_DIR), because a
 *   green algorithm that nothing calls is exactly the defect this item started
 *   as.
 *
 * ---------------------------------------------------------------------------
 * WHY THE DEAD-PEER CASE IS THE POINT OF THE FILE
 * ---------------------------------------------------------------------------
 * scs_disconnect() only INITIATES: it emits the type 8 and waits for the peer's
 * type 9 to release the op 6 (spec SS4(h)(1f)'s 8->9->6, 131 of 131). A peer
 * that never answers would leave the departing node holding an unfinished
 * handshake -- and the executive's answer must not be to wait. It is the FSM's
 * own disconnect timer, armed with a value the departure LOWERS (400 ms) so the
 * guard fires INSIDE the 500 ms drain instead of at the 5 s default. That is
 * asserted here on the armed value itself, not merely on "a timer was armed":
 * a timer armed with the 5000 ms default would miss the window entirely and
 * read identically to a correct one.
 *
 * Nothing in this file composes a frame. Every byte asserted is read back OFF
 * THE BUILT FRAME through the codec (scsh_record_ctrl), the same discipline
 * test_scs_fsm_credit.c's t_eight_before_disconnect uses, whose assertions this
 * file deliberately reuses.
 */

#include <string.h>
#include <stdio.h>

#include "cluster_test.h"
#include "scs_test_harness.h"
#include "vms_cluster_sysgen.h"   /* the REAL kill-switch predicate */

/*
 * The two budget numbers the shipping departure uses (vms_cnxman.c's
 * CNXMAN_DEPART_DISC_MS / _DRAIN_MS). Restated here rather than included --
 * vms_cnxman.h is not includable in this TU -- and the SOURCE SCAN below
 * asserts the shipping file really carries these values, so the restatement
 * cannot silently drift from the thing under test.
 */
#define DEPART_DISC_MS   400u
#define DEPART_DRAIN_MS  500u

static struct scsh_node a_node;
static struct scsh_node b_node;
static struct scsh_sysap a_sysap;
static struct scsh_sysap b_sysap;

/* One OPEN connection between A and B, with A's disconnect timeout lowered the
 * way vms_cnxman_depart() lowers it for the duration of a departure. */
static void rig(vms_conid_t *a_conid, vms_conid_t *b_conid)
{
	scsh_wire_reset();
	scsh_node_init(&a_node, 0x0101u, 0x3358u);
	scsh_node_init(&b_node, 0x0202u, 0xe995u);
	scsh_sysap_init(&a_sysap, &a_node);
	scsh_sysap_init(&b_sysap, &b_node);
	b_sysap.connect_decision = 0;
	(void)scs_fsm_listen(&b_node.fsm, scsh_name_b, &b_sysap.ops, 10u);
	scsh_link(&a_node, &b_node);
	(void)scsh_open_pair(&a_node, &b_node, 6u, a_conid);
	*b_conid = b_sysap.last_opened_conid;
}

/* What scs_set_disconnect_timeout() does, at the pure layer it does it on. */
static uint32_t depart_lower_timeout(struct scsh_node *n, uint32_t ms)
{
	struct scs_fsm_cfg cfg = n->fsm.cfg;
	uint32_t was = cfg.disconnect_timeout_ms;

	cfg.disconnect_timeout_ms = ms;
	scs_fsm_set_cfg(&n->fsm, &cfg);
	return was;
}

/* ------------------------------------------------------------------ *
 * 1. HAPPY PATH: the departure puts a real 8 -> 9 -> 6 on the wire
 * ------------------------------------------------------------------ */
static void t_departure_emits_the_grounded_sequence(void)
{
	vms_conid_t a_conid, b_conid;
	uint32_t was;
	int i8, i6, i9;

	printf("-- a clean departure emits 8 -> 9 -> 6 per open connection\n");
	rig(&a_conid, &b_conid);
	a_sysap.return_credit_immediately = 1u;

	/* Give the ledger something real to return, so the op 8's credit field
	 * is a genuine ledger read and not an accidental zero (the t_eight_
	 * before_disconnect setup, for the same reason). */
	{
		uint8_t body[SCS_SYSAP_BODY_LEN];
		uint32_t i, k;

		for (k = 0; k < 3u; k++) {
			for (i = 0; i < SCS_SYSAP_BODY_LEN; i++)
				body[i] = (uint8_t)(0x11u + k);
			(void)scs_fsm_send_msg(&b_node.fsm, b_conid, body,
					       SCS_SYSAP_BODY_LEN);
			(void)scsh_pump();
		}
	}
	ct_check_eq_u32(scsh_cdt(&a_node, a_conid)->credit_pending, 3u,
			"A holds three Pending Receive Credits before it leaves");

	was = depart_lower_timeout(&a_node, DEPART_DISC_MS);
	ct_check_eq_u32(was, SCS_DISCONNECT_TIMEOUT_MS_DEFAULT,
			"the departure lowers the timeout FROM the documented "
			"5 s default -- the value that would miss the drain");

	a_node.n_tx = 0u;   /* trace only the departure */
	b_node.n_tx = 0u;

	/* THE DEPARTURE'S OWN STEP: the ordinary teardown verb, per open
	 * connection. vms_cnxman_depart() calls scs_disconnect(), which is this
	 * with the glue's one dereference around it. */
	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) == SCS_OK,
		 "the departure initiates the teardown on the open connection");

	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 1u,
			"the DRAIN PREDICATE sees exactly one teardown "
			"outstanding -- its op 6 is not out yet");
	ct_check_eq_u32(a_node.last_ms[SCS_TIMER_DISCONNECT], DEPART_DISC_MS,
			"...and the dead-peer guard behind it was armed with "
			"the LOWERED timeout, inside the drain budget");
	ct_check(a_node.last_ms[SCS_TIMER_DISCONNECT] < DEPART_DRAIN_MS,
			"...which is the whole guarantee: the guard fires "
			"while the departing thread is still draining");

	/* The peer answers -- in production this is the frame the FORK THREAD
	 * dispatches while vms_cnxman_depart() is yielding in exec_wait_ms(). */
	(void)scsh_pump();
	(void)scsh_pump();

	i8 = scsh_first_op(&a_node, SCS_MTYPE_CR_REQ);
	i6 = scsh_first_op(&a_node, SCS_MTYPE_DISC_REQ);
	i9 = scsh_first_op(&b_node, SCS_MTYPE_CR_RSP);
	ct_check(i8 >= 0, "the departing node sent its type 8");
	ct_check(i6 >= 0, "...and the DISCONNECT_REQUEST the members act on");
	ct_check(i8 < i6, "...in that order (SS4(h)(1f), 131/131)");
	ct_check_eq_u32((unsigned long)(i6 - i8), 1u,
			"...with NOTHING between them but the peer's 9");
	ct_check(i9 >= 0, "the member answered the 8 with its 9");
	ct_check_eq_u32(a_node.tx_credit[i8], 3u,
			"the op 8 carried the LEDGER's real pending count (3) "
			"-- read from the CDT, not templated by the departure");
	ct_check_eq_u32(a_node.tx_credit[i6], 0u,
			"the op 6 carried credit 0 (131/131: the last credit "
			"on a connection is never returned)");

	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 0u,
			"THE DRAIN ENDS: nothing outstanding, so the departing "
			"thread stops waiting and the shutdown proceeds");
	ct_check(scsh_count_op(&b_node, SCS_MTYPE_DISC_RSP) >= 1u,
		 "the member answered the departure with its op 7 -- it was "
		 "TOLD, which is the whole point of the item");

	/* NON-VACUITY: real counters moved on the real FSM. */
	ct_check_eq_u32(a_node.fsm.credit_msgs_sent, 1u,
			"exactly one type 8 was originated");
	ct_check_eq_u32(a_node.fsm.credit_msgs_answered, 1u,
			"...and exactly one type 9 matched it");
	ct_check_eq_u32(a_node.fsm.disc_without_credit_msg, 0u,
			"...so no teardown had to skip the credit message: the "
			"happy path really was the grounded 8->9->6");
}

/* ------------------------------------------------------------------ *
 * 2. THE NEVER-CRASH EDGE: the member never answers
 *
 * The departing node must still put its DISCONNECT_REQUEST on the wire, must
 * still close, must not hang, and must not crash. The mechanism is the FSM's
 * own disconnect timer -- fired here exactly as vms_scs.c's work handler fires
 * it (scs_fsm_timer(f, SCS_TIMER_DISCONNECT, key)), from the key and the
 * timeout the FSM itself armed.
 * ------------------------------------------------------------------ */
static void t_dead_peer_still_departs_and_closes(void)
{
	vms_conid_t a_conid, b_conid;
	uint32_t key;
	int i8, i6;

	printf("-- DEAD PEER: no op 9 ever arrives, and the departure still "
	       "lands and closes\n");
	rig(&a_conid, &b_conid);
	(void)depart_lower_timeout(&a_node, DEPART_DISC_MS);

	/* The member is gone: nothing this node sends is delivered or answered.
	 * The frame is still BUILT and recorded, so what A put on the wire is
	 * still asserted on. */
	a_node.drop_tx = 1;
	a_node.n_tx = 0u;

	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) == SCS_OK,
		 "the departure initiates the teardown as usual");
	i8 = scsh_first_op(&a_node, SCS_MTYPE_CR_REQ);
	ct_check(i8 >= 0, "the type 8 went out");
	ct_check(scsh_first_op(&a_node, SCS_MTYPE_DISC_REQ) < 0,
		 "...and the op 6 is correctly NOT out yet: it waits on the 9 "
		 "that will never come");
	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 1u,
			"so the drain predicate says one teardown is "
			"outstanding, and the departing thread waits");

	key = a_node.last_key[SCS_TIMER_DISCONNECT];
	ct_check_eq_u32(a_node.last_ms[SCS_TIMER_DISCONNECT], DEPART_DISC_MS,
			"the guard is armed for 400 ms -- INSIDE the 500 ms "
			"drain, which is why the next step happens while the "
			"departing thread is still there to see it");

	/* 400 ms later the substrate expiry arrives. This is vms_scs.c's
	 * scs_work_handler case SCS_TIMER_DISCONNECT, verbatim. */
	scs_fsm_timer(&a_node.fsm, SCS_TIMER_DISCONNECT, key);

	i6 = scsh_first_op(&a_node, SCS_MTYPE_DISC_REQ);
	ct_check(i6 >= 0,
		 "THE DEPARTURE STILL LANDS: the FSM's own guard emitted the "
		 "DISCONNECT_REQUEST without the credit exchange");
	ct_check(i8 < i6, "...still after the 8 this node really sent");
	ct_check_eq_u32(a_node.tx_credit[i6], 0u,
			"...still carrying credit 0, the same frame shape the "
			"answered path builds");
	ct_check_eq_u32(a_node.fsm.disc_without_credit_msg, 1u,
			"...and the shortfall is COUNTED, not silent");
	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 0u,
			"THE DRAIN ENDS ANYWAY: the predicate clears, so a "
			"dead member cannot hold the shutdown open");

	/* The connection must also go away. The second expiry is the FSM's own
	 * close, and it is reached because disc_emit_req re-armed the timer --
	 * again with the lowered value. */
	ct_check_eq_u32(a_node.last_ms[SCS_TIMER_DISCONNECT], DEPART_DISC_MS,
			"the guard re-armed with the lowered timeout too");
	scs_fsm_timer(&a_node.fsm, SCS_TIMER_DISCONNECT,
		      a_node.last_key[SCS_TIMER_DISCONNECT]);
	ct_check(scsh_cdt(&a_node, a_conid) == (struct scs_cdt *)0,
		 "the connection is CLOSED -- nothing leaks behind a member "
		 "that went silent");
	ct_check_eq_u32(a_sysap.n_closed, 1u,
			"...and the SYSAP was told exactly once");
}

/* ------------------------------------------------------------------ *
 * 3. IDEMPOTENCE: only an OPEN peer connection is ever disconnected
 *
 * The departure enumerates Con.IDs out of three live executive structures and
 * the same connection can appear in more than one of them. Its safety net is
 * not a predicate in vms_cnxman.c but the dispatch table itself:
 * SCS_EV_LOCAL_DISCONNECT exists in exactly ONE row, [OPEN]. This asserts that
 * -- so the enumeration end can stay simple and still be safe.
 * ------------------------------------------------------------------ */
static void t_only_open_connections_are_disconnected(void)
{
	vms_conid_t a_conid, b_conid;
	uint32_t ignored_before;

	printf("-- only an OPEN connection is disconnected (the table is the "
	       "guard)\n");
	rig(&a_conid, &b_conid);
	(void)depart_lower_timeout(&a_node, DEPART_DISC_MS);

	/* (a) A LISTENING CDT. B's `scsh_name_b` registration owns one; it has
	 * no peer and no teardown to walk, and a departure must not touch it. */
	{
		struct scs_sysap_info info;

		ct_check(scs_fsm_sysap_lookup(&b_node.fsm, scsh_name_b, &info)
			 == SCS_OK, "B really holds a listening registration");
		b_node.n_tx = 0u;
		ignored_before = b_node.fsm.ignored_events;
		ct_check(scs_fsm_disconnect(&b_node.fsm, info.listen_conid) !=
			 SCS_OK,
			 "a LISTENING CDT refuses the disconnect verb");
		ct_check_eq_u32(b_node.fsm.ignored_events, ignored_before + 1u,
				"...and the refusal is COUNTED by the table, "
				"not silently ignored");
		ct_check_eq_u32(scsh_count_op(&b_node, SCS_MTYPE_CR_REQ), 0u,
				"...so no type 8 went out for it");
		ct_check_eq_u32(scsh_count_op(&b_node, SCS_MTYPE_DISC_REQ), 0u,
				"...and no DISCONNECT_REQUEST either");
	}

	/* (b) AN ALREADY-CLOSING CONNECTION: the second disconnect of the same
	 * Con.ID -- exactly what a duplicate in the enumeration would produce. */
	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) == SCS_OK,
		 "the first disconnect is accepted");
	(void)scsh_pump();
	(void)scsh_pump();
	a_node.n_tx = 0u;
	ignored_before = a_node.fsm.ignored_events;
	ct_check(scs_fsm_disconnect(&a_node.fsm, a_conid) != SCS_OK,
		 "a SECOND disconnect of the same connection is refused");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_CR_REQ), 0u,
			"...emitting no second type 8");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_DISC_REQ), 0u,
			"...and no second DISCONNECT_REQUEST: a member cannot "
			"be told twice that this node is leaving");

	/* (c) A Con.ID the executive does not hold is refused outright, never
	 * answered as though it named something (INV-6). */
	ct_check(scs_fsm_disconnect(&a_node.fsm, 0xdeadbeefu) ==
		 SCS_ERR_NOCONN,
		 "an unheld Con.ID is refused, not acted on");
	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 0u,
			"and none of the three refusals left a phantom "
			"teardown for the drain to wait on");
}

/* ------------------------------------------------------------------ *
 * 4. THE KILL SWITCH: OVMX_CLEAN_DEPART = 0 emits nothing at all
 *
 * A change to what this node puts on a live cluster's wire gets a switch that
 * turns it off in the field without a rebuild. This asserts that the switch is
 * a REAL control and not a hollow one, which for this executive means one
 * specific thing:
 *
 *   THE EXECUTIVE CANNOT READ OVMXVMSSYS.PAR. sysgen_read_param() is a libvms
 *   FILE read; kernel-core has no filesystem and no getenv. A switch that only
 *   lived in the .PAR would be unreadable at the exact moment it is consulted --
 *   green in a test that greps for it, a no-op at shutdown. So the parameter is
 *   ROUTED: STARTUP.EXE reads the .PAR, VMS_IOCTL_SYSGEN_LOAD carries the value
 *   down, cluster_sysgen_load() COMMITS it into the executive's own
 *   struct vms_cluster.params, and the departure reads it from THERE.
 *
 * AND THERE IS A SECOND, SHARPER TRAP UNDER THE FIRST. sysgen_read_param()
 * reads the PERSISTED store: a parameter this system knows but OVMXVMSSYS.PAR
 * has never carried reads ABSENT, and an absent read leaves the caller's memset
 * zero standing. For a switch whose OFF value is 0, that would turn the
 * faithful departure OFF on every .PAR written before the parameter existed --
 * on every install that already exists. "Default ON" would then be true in the
 * header and false on every real system. Two things stop it, and BOTH are
 * asserted below: the userland reader falls back to SYSGEN_DEFAULT_OVMX_CLEAN_
 * DEPART when the store has no record, and the WIRE FIELD CARRIES THE NEGATION,
 * so even a caller that never touches the field at all sends the faithful
 * default.
 *
 * The chain below is driven end to end through the SHIPPING functions -- the
 * real cluster_sysgen_depart_from_wire() polarity, the real
 * cluster_sysgen_load() commit and the real cluster_sysgen_clean_depart() read
 * -- and then the real scs_fsm is asked to depart. If any link stopped carrying
 * the value, the op-8 count at the end is what changes.
 * ------------------------------------------------------------------ */
static uint32_t depart_if_permitted(const struct vms_cluster *cl,
				    struct scsh_node *n, vms_conid_t conid)
{
	if (!cluster_sysgen_clean_depart(cl))
		return 0u;
	return scs_fsm_disconnect(&n->fsm, conid) == SCS_OK ? 1u : 0u;
}

/*
 * The parameter record a boot really commits, built THE WAY vms_devtab.c BUILDS
 * IT: from the ioctl's negated wire byte, through the shipping polarity
 * function. `clean_depart_off` is therefore the value a test controls, and it
 * is exactly the byte STARTUP.EXE puts on the wire -- 0 when the parameter is
 * absent or configured on, 1 only when an operator configured it off.
 *
 * SCSNODE is present because cluster_sysgen_load() refuses a clustered record
 * without one.
 */
static void depart_params(struct vms_cluster_params *p, uint8_t clean_depart_off)
{
	memset(p, 0, sizeof(*p));
	p->vaxcluster = 2u;
	p->scsnode[0] = 'O';
	p->scsnode[1] = 'V';
	p->scsnode[2] = 'M';
	p->scsnode[3] = 'X';
	p->scsnode_len = 4u;
	p->clean_depart = cluster_sysgen_depart_from_wire(clean_depart_off);
}

static void t_kill_switch_suppresses_the_departure(void)
{
	static struct vms_cluster cl;   /* big; not on the stack */
	struct vms_cluster_params params;
	vms_conid_t a_conid, b_conid;

	printf("-- OVMX_CLEAN_DEPART: the switch the EXECUTIVE reads, not the "
	       ".PAR it cannot\n");

	/* ---- (0) THE POLARITY ITSELF, at the one function that owns it.
	 * The zero byte is what an ABSENT parameter really produces: memset
	 * leaves it, and STARTUP.EXE's fallback never sets it. It must resolve
	 * to ON, or every pre-existing install silently loses the departure. */
	ct_check_eq_u32(cluster_sysgen_depart_from_wire(0u), 1u,
			"THE ABSENT CASE IS ON: the zero byte a .PAR that "
			"predates the parameter produces means DEPART");
	ct_check_eq_u32(cluster_sysgen_depart_from_wire(1u), 0u,
			"...and only the explicit off-byte means do not");

	/* ---- (i) A BOOT WHOSE .PAR NEVER MENTIONED THE PARAMETER.
	 * STARTUP.EXE memsets the args struct and its fallback answers the
	 * default, so the wire byte is 0. Nothing else about this record says
	 * anything about departing -- and the node must still depart. ---- */
	memset(&cl, 0, sizeof(cl));
	depart_params(&params, 0u);
	ct_check(cluster_sysgen_load(&cl, &params) == 1,
		 "SYSGEN_LOAD commits the record a pre-existing .PAR produces");
	ct_check_eq_u32(cl.params.clean_depart, 1u,
			"THE DEFAULT SURVIVED THE ABSENT-READS-0 TRAP: the "
			"executive holds the switch ON");
	ct_check(cluster_sysgen_clean_depart(&cl),
		 "...so the departure's gate reads ON");

	rig(&a_conid, &b_conid);
	(void)depart_lower_timeout(&a_node, DEPART_DISC_MS);
	a_node.n_tx = 0u;

	ct_check_eq_u32(depart_if_permitted(&cl, &a_node, a_conid), 1u,
			"...and the departure really initiates");
	(void)scsh_pump();
	(void)scsh_pump();
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_CR_REQ), 1u,
			"A TYPE 8 REALLY GOES OUT on a node whose .PAR never "
			"heard of the parameter -- the default is ON in "
			"BEHAVIOUR, not just in a header comment");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_DISC_REQ), 1u,
			"...and so does the DISCONNECT_REQUEST the members "
			"act on");

	/* ---- (ii) A BOOT THAT CONFIGURED THE SWITCH OFF. Same rig, same
	 * call, same everything -- only the operator's byte differs. ---- */
	memset(&cl, 0, sizeof(cl));
	depart_params(&params, 1u);
	ct_check(cluster_sysgen_load(&cl, &params) == 1,
		 "SYSGEN_LOAD commits a record with OVMX_CLEAN_DEPART = 0");
	ct_check_eq_u32(cl.params.clean_depart, 0u,
			"THE ROUTING HOLDS: the operator's choice reached the "
			"executive's OWN params, the only place the shutdown "
			"path can read it from");
	ct_check(!cluster_sysgen_clean_depart(&cl),
		 "...so the departure's gate reads OFF");

	rig(&a_conid, &b_conid);
	(void)depart_lower_timeout(&a_node, DEPART_DISC_MS);
	a_node.n_tx = 0u;

	ct_check_eq_u32(depart_if_permitted(&cl, &a_node, a_conid), 0u,
			"...so the departure initiates nothing");
	(void)scsh_pump();
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_CR_REQ), 0u,
			"ZERO type 8 on the wire");
	ct_check_eq_u32(scsh_count_op(&a_node, SCS_MTYPE_DISC_REQ), 0u,
			"ZERO DISCONNECT_REQUEST on the wire -- THE CONTROL "
			"REALLY CONTROLS, and this node leaves exactly as it "
			"did before the change");
	ct_check_eq_u32(scs_fsm_disc_pending(&a_node.fsm), 0u,
			"...and nothing for the drain to wait on");
	ct_check(scsh_cdt(&a_node, a_conid) != (struct scs_cdt *)0,
		 "the connection is still OPEN: with the switch off the "
		 "teardown is the local close vms_scs_stop() does later");

	/* ---- (iii) A NODE THAT NEVER COMMITTED PARAMETERS answers 0 -- not
	 * "switch off", but "this executive holds no parameter record", the same
	 * test cluster_sysgen_credits() applies (INV-6). ---- */
	memset(&cl, 0, sizeof(cl));
	cl.params.clean_depart = 1u;   /* a byte in an UNCOMMITTED record */
	ct_check(!cluster_sysgen_clean_depart(&cl),
		 "a node that never loaded SYSGEN parameters answers 0, "
		 "whatever byte happens to sit in the unloaded record");
}

/* ==========================================================================
 * PROOF 2 -- the WIRING, read out of the SHIPPING files
 * ========================================================================== */
static char src_buf[500000];

static int read_src(const char *dir, const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(src_buf, 1u, sizeof(src_buf) - 1u, f);
	fclose(f);
	src_buf[n] = '\0';
	return 0;
}

static void has(const char *needle, const char *what)
{
	ct_check(strstr(src_buf, needle) != NULL, what);
}

static void absent(const char *needle, const char *what)
{
	ct_check(strstr(src_buf, needle) == NULL, what);
}

static void t_the_departure_is_wired(void)
{
	printf("\n-- the WIRING, out of src/kernel-core/vms_cnxman.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}
	has("#define CNXMAN_DEPART_DRAIN_MS   500u",
	    "the drain budget is the 500 ms the design fixed");
	has("#define CNXMAN_DEPART_DISC_MS    400u",
	    "...and the lowered disconnect timeout is INSIDE it, which is what "
	    "makes the dead-peer guard fire while the drain is still running");
	has("prev_timeout = scs_set_disconnect_timeout(cl->scs,",
	    "the departure really lowers the FSM's own timer, rather than "
	    "forging a timer event of its own");
	has("(void)scs_set_disconnect_timeout(cl->scs, prev_timeout);",
	    "...and puts the documented default back, so it leaves no trace in "
	    "the executive beyond what it put on the wire");
	has("scs_disconnect(cn->cl->scs, list[i], 0u)",
	    "every DISCONNECT_REQUEST goes out through the ORDINARY teardown "
	    "path -- no second encoder, every field from the live CDT");
	has("club->csb[i].cdt_conid",
	    "the connections are enumerated from the CSBs the executive holds");
	has("cn->join.cm_conid",
	    "...plus the join's own VMS$VAXcluster connection");
	has("cn->join.mscp_conid",
	    "...and its VMS$DISK_CL_DRVR connection");
	has("cn->cur_conid",
	    "...and the request in flight, which no CSB may record yet");
	has("if (!cluster_sysgen_clean_depart(cl))",
	    "the departure asks the OVMX_CLEAN_DEPART kill switch -- the same "
	    "predicate section 4 above drove -- and asks it exactly once");
	absent("sysgen_read_param",
	       "THE HOLLOW-CONTROL TRAP IS SHUT: the executive never tries to "
	       "read OVMXVMSSYS.PAR -- it cannot, and a gate that did would be "
	       "a no-op at the one moment it is consulted");
	has("pending = scs_disc_pending(cl->scs)",
	    "the drain's predicate is the executive's own outstanding count");
	has("exec_wait_ms(CNXMAN_DEPART_POLL_MS)",
	    "...and it YIELDS between re-tests");
	has("vms_cluster_fork_leave(cl);\n\n\t\tif (pending == 0u)",
	    "...with the fork mutex RELEASED while it waits -- the fork thread "
	    "has to be able to deliver the peer's answer, and it is the only "
	    "context that can");

	printf("\n-- the WIRING, out of src/kernel-core/vms_devtab.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_devtab.c") != 0) {
		ct_check(0, "could not open vms_devtab.c");
		return;
	}
	has("long vms_ioctl_cluster_stop(",
	    "VMS_IOCTL_CLUSTER_STOP exists: the clean departure has a caller "
	    "at all, which is what this item's first attempt found missing");
	has("vms_cnxman_depart(cl, &initiated, &drained)",
	    "...and it ANNOUNCES first");
	has("vms_cnxman_depart(cl, &initiated, &drained);\n\n    /* 2.",
	    "...before anything else is torn down");
	has("vms_cluster_fork_stop(cl);\n\n    /* 4-6.",
	    "the fork thread is JOINED before any layer is freed -- the "
	    "use-after-free the teardown order exists to prevent");
	has("args.connections_drained = drained",
	    "the readback is the executive's own count of what really "
	    "completed, never composed from the status (INV-6)");
	has("out->clean_depart = cluster_sysgen_depart_from_wire(args->clean_depart_off)",
	    "SYSGEN_LOAD ROUTES the kill switch into the executive's own params "
	    "-- the executive cannot read OVMXVMSSYS.PAR, so a switch that did "
	    "not come down this path would be a no-op at shutdown");
	absent("sysgen_read_param",
	       "...and the ioctl handler does not read the .PAR either: the "
	       "value only ever arrives as ioctl payload");
	has("static void sysgen_load_args_to_params(",
	    "...through the ONE field-by-field copy, where a dropped field is a "
	    "compile error rather than a silent layout drift");

	printf("\n-- the WIRING, out of the boot path (ovmx_init.c) --\n");
	if (read_src(OVMX_INIT_DIR, "ovmx_init.c") != 0) {
		ct_check(0, "could not open ovmx_init.c");
		return;
	}
	has("stop_cluster_port();",
	    "STARTUP.EXE issues the departure when the system goes down -- the "
	    "mirror of the CLUSTER_START it issued at boot");
	has("vms_kif_cluster_stop(&departed, &connections)",
	    "...and takes the executive's own departure counts back");
	has("sysgen_read_param(\"OVMX_CLEAN_DEPART\", &u32) == 0",
	    "the .PAR is read HERE, in userland, where a file can be read");
	has("return (uint8_t)SYSGEN_DEFAULT_OVMX_CLEAN_DEPART;",
	    "...and an ABSENT parameter falls back to the SSOT default rather "
	    "than to the zero an absent read leaves standing -- the same reason "
	    "cluster_credits_requested() has a fallback, and the difference "
	    "between a default and a hollow one");
	has("args.clean_depart_off = clean_depart_requested() ? 0 : 1;",
	    "...and the answer is carried DOWN on the SYSGEN_LOAD ioctl, which "
	    "is the only way the executive ever learns it");
}

int main(void)
{
	t_departure_emits_the_grounded_sequence();
	t_dead_peer_still_departs_and_closes();
	t_only_open_connections_are_disconnected();
	t_kill_switch_suppresses_the_departure();
	t_the_departure_is_wired();
	return ct_summary("test_scs_fsm_depart");
}
