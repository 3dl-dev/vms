/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_sim_vc.c - THE FC-P1.4 DONE-CONDITION, rung R2: three simulated `pe`
 * stacks form virtual circuits under 10 % loss, and the run is deterministic
 * by seed.
 *
 * Each node here is the SHIPPING src/kernel-core/vms_pe_fsm.c with the
 * simulator's ops injected. Nothing in this file models the protocol: every
 * assertion is read back out of a real `struct pe_vc` / `struct pe_channel` /
 * `struct pe_fsm` at the instant it runs (sim_metric.c).
 *
 * ===========================================================================
 * HOW THE LOSS PATH IS PROVED, AND WHY NOT WITH DICE
 * ===========================================================================
 *
 * A lossy run that happens to lose nothing important is a lossless happy path
 * wearing a hat. Worse, a probabilistic assertion ("at 10 % loss something
 * will surely be retransmitted") is a flaky test waiting to happen. So the
 * loss path is proved with CONSTRUCTIONS that contain no randomness at all --
 * a one-way 100 % loss window, a one-way duplication, a delay past the retry
 * deadline -- and the seeded 10 % scenario is then used for what it is
 * genuinely good at: breadth, and the plan's own done-condition.
 *
 *   test_formation_retry_without_randomness  a 5 s one-way delay past the 1 s
 *       retry interval: the 0x41 is re-sent before the answer can physically
 *       have arrived, and the circuit opens anyway.
 *   test_loss_forces_more_formation_frames   the same scenario at 0 % and at
 *       40 % loss: the LIFETIME 0x41 count is strictly higher when frames are
 *       lost, on every seed. Loss demonstrably drove the ladder.
 *   test_retransmit_reuses_the_sequence      a 100 % loss window on ONE
 *       direction: the message is re-sent, it re-uses its sequence (spec
 *       §4(L)), the ring holds it, and when the window closes the peer takes
 *       it and the cumulative acknowledgement drains the ring.
 *   test_duplicate_is_absorbed               100 % duplication on one link:
 *       the port absorbs the second copy (§4(h)(4a)) and delivers upward once.
 *   test_three_nodes_10pct_loss              the plan's condition, 8 seeds.
 *   test_determinism_by_seed                 same seed, identical run.
 *
 * ===========================================================================
 * A FINDING THIS FILE RECORDS RATHER THAN HIDES
 * ===========================================================================
 *
 * When a sender PIPELINES messages and one is lost, the receiver sees the
 * next sequence as a p. 2-31 GAP and breaks the circuit -- which is exactly
 * what vms_pe_fsm.h §3b(4) says it will do, citing "if ... the guarantee of
 * message sequentiality cannot be satisfied, the virtual circuit ... will be
 * explicitly broken". test_pipelined_under_loss therefore asserts RECOVERY
 * (every circuit re-forms and every ring drains) and PRINTS the gaps and
 * re-formations it observed, instead of asserting an absence that the FSM
 * does not promise. Spec §4(h)(4a)'s measurement -- 321 599 sequenced messages
 * across 47 captures, ZERO gaps from a real VAX -- is what makes that
 * behaviour tolerable on real wire, and it is a rung-5 question, not a rung-2
 * one.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_scenario.h"

/* Three simulated OVMX nodes. The SCSSYSTEMIDs are the harness's own; the
 * cluster-LOGICAL address is BUILT from each by the FSM (spec §4(a)), and each
 * node's hardware MAC is deliberately unrelated to it -- §4(a).0's lesson. */
static const struct sim_node_decl three_nodes[] = {
	SIM_NODE("OVMXA", 1030),
	SIM_NODE("OVMXB", 1031),
	SIM_NODE("OVMXC", 1032)
};

#define N_PAIRS 6u   /* 3 nodes, one circuit per ORDERED pair */

/* These hold a whole simulated cluster each: static, never a stack frame. */
static struct sim_scenario_out g_out;
static struct sim_scenario_out g_out2;

/* `-v` prints every scenario's SDA-like snapshots even when it passes, which
 * is how a developer reads the simulated cluster's state by eye. */
static int g_verbose;

/* Print the report only when something failed; a green run stays quiet. */
static int run_scenario(const struct sim_scenario *scn, uint64_t seed,
			struct sim_scenario_out *out)
{
	int failures = sim_scenario_run(scn, seed, out);

	if (failures != 0 || g_verbose)
		sim_scenario_report(scn, out, g_verbose);
	return failures;
}

static uint64_t metric(struct sim_scenario_out *out, enum sim_metric m)
{
	return sim_metric_read(&out->sim, m, NULL, NULL);
}

/* ==================================================================== *
 * 1. The baseline: a perfect wire
 * ==================================================================== */

static const struct sim_step steps_lossless[] = {
	SIM_UNTIL_ALL_VCS_OPEN(60000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS, "all six circuits OPEN"),
	SIM_EXPECT_VC("OVMXA", "OVMXB", VMS_PE_VC_OPEN),
	SIM_EXPECT_VC("OVMXC", "OVMXA", VMS_PE_VC_OPEN),
	/* A circuit can be OPEN before the channel under it has finished its
	 * OWN b3/b4 ladder -- answering the peer's START opens this end while
	 * our own b3 is still outstanding -- so the channel assertion gets its
	 * own wait rather than riding on the circuit one. */
	SIM_UNTIL_ALL_CHANNELS(60000),
	SIM_EXPECT_EQ(SIM_M_CHANNELS_VERIFIED, N_PAIRS, "all six channels b4"),
	SIM_EXPECT_CHANNEL("OVMXA", "OVMXB", VMS_PE_CH_B4),
	/* Spec §4(a).1: OVMX originates no b2 INIT, ever. */
	SIM_EXPECT_EQ(SIM_M_B2_TX, 0, "no b2 was originated (spec 4(a).1)"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0, "no circuit broke on a sequence gap"),
	SIM_EXPECT_EQ(SIM_M_TX_ERRORS, 0, "no transmit failed"),
	SIM_EXPECT_EQ(SIM_M_VC_NO_INCARNATION, 0,
		      "every START had a real 4(i).B echo to carry"),
	SIM_EXPECT_EQ(SIM_M_VC_NO_IDENTITY, 0,
		      "and a real boot incarnation and clock"),
	/* Design §5.3 is OPEN: no cluster credential exists, so every directed
	 * frame went out without one and the ABSENCE is counted. A zero here
	 * would mean a token was invented. */
	SIM_EXPECT_GE(SIM_M_NONCE_ABSENT, 1,
		      "the missing credential is counted, not faked"),
	SIM_EXPECT_EQ(SIM_M_LAN_LOST, 0, "a perfect wire lost nothing"),
	SIM_END
};

static const struct sim_scenario scn_lossless =
	SIM_SCENARIO("3 nodes, perfect wire", three_nodes, steps_lossless);

static void test_three_nodes_lossless(void)
{
	printf("-- baseline: 3 pure pe stacks on a perfect wire\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_lossless, 1u, &g_out),
			0, "every expectation held");
	printf("       (formed in %llu ms of virtual time, %llu events, "
	       "%llu 0x41 formation frames)\n",
	       (unsigned long long)g_out.sim.clock.now_ms,
	       (unsigned long long)g_out.sim.events,
	       (unsigned long long)(metric(&g_out, SIM_M_STARTS_TX) +
				    metric(&g_out, SIM_M_STACKS_TX)));
}

/* ==================================================================== *
 * 2. Formation retransmit, with NO randomness at all
 *
 * OVMXA -> OVMXB is given a 5-second one-way delay while B -> A stays instant,
 * and the formation retry interval is set to 1 s. A's 0x41 is therefore
 * re-sent several times before the peer's answer can physically have arrived,
 * and the circuit opens anyway.
 *
 * The retry count is read MID-FLIGHT, at T+10 s, because vc_open() clears
 * pe_vc.form_tries -- see the note on SIM_M_VC_FORM_TRIES in sim_scenario.h.
 * The timeline: the beat puts the first multicast HELLO out at T+2 s, A's
 * directed b3 reaches B at T+7 s, B's b4 comes straight back, A begins
 * formation at T+7 s and its answer cannot arrive before T+12 s.
 * ==================================================================== */

static const struct sim_node_decl slow_pair[] = {
	SIM_NODE_CFG("OVMXA", 1030, .timvcfail_ms = 40000,
		     .vc_retransmit_ms = 1000),
	SIM_NODE_CFG("OVMXB", 1031, .timvcfail_ms = 40000,
		     .vc_retransmit_ms = 1000)
};

static const struct sim_step steps_slow[] = {
	SIM_LINK("OVMXA", "OVMXB", .latency_ms = 5000),
	SIM_RUN(10000),
	SIM_EXPECT_GE(SIM_M_VC_FORM_TRIES, 1,
		      "the un-answered 0x41 is being retransmitted"),
	SIM_EXPECT_LE(SIM_M_VCS_OPEN, 1,
		      "and the far end cannot possibly be open yet"),
	SIM_UNTIL_ALL_VCS_OPEN(180000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2, "both ends of the circuit are OPEN"),
	SIM_EXPECT_GE(SIM_M_STARTS_TX, 3,
		      "more 0x41 STARTs went out than there are circuits"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0, "and nothing broke on a gap"),
	SIM_EXPECT_EQ(SIM_M_LAN_LOST, 0,
		      "with NOTHING lost: this proof owes nothing to the dice"),
	SIM_END
};

static const struct sim_scenario scn_slow =
	SIM_SCENARIO("formation retry forced by delay, not by loss",
		     slow_pair, steps_slow);

static void test_formation_retry_without_randomness(void)
{
	printf("-- retransmit -> OPEN, proved with no dice at all\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_slow, 1u, &g_out), 0,
			"every expectation held");
	printf("       (STARTs sent %llu, opened at T+%llu ms)\n",
	       (unsigned long long)metric(&g_out, SIM_M_STARTS_TX),
	       (unsigned long long)g_out.sim.clock.now_ms);
}

/* ==================================================================== *
 * 3. LOSS drives the formation ladder -- measured, not assumed
 *
 * The same three-node formation is run twice: once on a perfect wire and once
 * with 40 % loss on every directed link. `starts_tx` and `stacks_tx` are
 * LIFETIME counters on each circuit (unlike form_tries, which vc_open clears),
 * so the difference between the two runs is exactly the extra 0x41 frames the
 * loss forced onto the wire. Every circuit still reaches OPEN.
 * ==================================================================== */

static const struct sim_step steps_form_only[] = {
	SIM_UNTIL_ALL_VCS_OPEN(300000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS, "every circuit reached OPEN"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0, "loss is not a gap"),
	SIM_END
};

static const struct sim_step steps_form_lossy[] = {
	SIM_LINKS_ALL(.loss_pct = 40),
	SIM_UNTIL_ALL_VCS_OPEN(300000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS,
		      "every circuit reached OPEN despite 40 % loss"),
	SIM_EXPECT_GE(SIM_M_LAN_LOST, 1, "the wire really lost frames"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0, "loss is not a gap"),
	SIM_END
};

static const struct sim_scenario scn_form_clean =
	SIM_SCENARIO("formation only, perfect wire", three_nodes,
		     steps_form_only);
static const struct sim_scenario scn_form_lossy =
	SIM_SCENARIO("formation only, 40 % loss", three_nodes,
		     steps_form_lossy);

static uint64_t formation_frames(struct sim_scenario_out *out)
{
	return metric(out, SIM_M_STARTS_TX) + metric(out, SIM_M_STACKS_TX);
}

static void test_loss_forces_more_formation_frames(void)
{
	uint64_t seed, clean;
	int all_green = 1, all_higher = 1;

	printf("-- loss DRIVES the formation ladder (lifetime 0x41 counts)\n");
	if (run_scenario(&scn_form_clean, 1u, &g_out) != 0)
		all_green = 0;
	clean = formation_frames(&g_out);
	printf("       perfect wire: %llu 0x41 frames for %u circuits\n",
	       (unsigned long long)clean, N_PAIRS);

	for (seed = 1u; seed <= 4u; seed++) {
		uint64_t lossy;

		if (run_scenario(&scn_form_lossy, seed, &g_out2) != 0) {
			all_green = 0;
			continue;
		}
		lossy = formation_frames(&g_out2);
		if (lossy <= clean)
			all_higher = 0;
		printf("       40 %% loss, seed %llu: %llu 0x41 frames "
		       "(+%lld), %llu lost, open at T+%llu ms\n",
		       (unsigned long long)seed, (unsigned long long)lossy,
		       (long long)((long long)lossy - (long long)clean),
		       (unsigned long long)metric(&g_out2, SIM_M_LAN_LOST),
		       (unsigned long long)g_out2.sim.clock.now_ms);
	}
	ct_check(all_green, "every run formed all six circuits");
	ct_check(all_higher,
		 "loss forced strictly more 0x41 formation frames on every "
		 "seed -- the P1.2 retransmit path really ran");
}

/* ==================================================================== *
 * 4. A message retransmit REUSES its sequence, and the ack drains the ring
 *
 * Spec §4(L), the invariant three campaign stalls came down to: "a message
 * consumes the channel send_seq exactly once, and retransmissions REUSE that
 * same send_seq". Proved here with no randomness: one direction is taken to
 * 100 % loss, one message is sent into it, and the sequence, the ring and the
 * cumulative acknowledgement are read out of the real circuit at each stage.
 * ==================================================================== */

static const struct sim_node_decl pair_nodes[] = {
	SIM_NODE("OVMXA", 1030),
	SIM_NODE("OVMXB", 1031)
};

static const struct sim_step steps_retransmit[] = {
	SIM_UNTIL_ALL_VCS_OPEN(60000),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_SEND_SEQ, 1,
			"a fresh circuit's next sequence is 1"),

	/* The wire swallows everything A sends to B, and only that. */
	SIM_LINK("OVMXA", "OVMXB", .loss_pct = 100),
	SIM_SEND("OVMXA", "OVMXB", 1),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_SEND_SEQ, 2,
			"the message consumed sequence 1, exactly once"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_UNACKED, 1,
			"and it is held in the unacked ring"),
	SIM_RUN(9000),
	SIM_EXPECT_PAIR(SIM_CMP_GE, "OVMXA", "OVMXB", SIM_M_RETRANSMITS, 2,
			"the ladder re-sent it while the wire swallowed it"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_SEND_SEQ, 2,
			"and NOT ONE retransmit consumed a new sequence (4(L))"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_UNACKED, 1,
			"still exactly one message outstanding"),
	SIM_EXPECT_NODE(SIM_CMP_EQ, "OVMXB", SIM_M_UPPER_MESSAGES, 0,
			"and the peer has been given nothing"),
	SIM_EXPECT_VC("OVMXA", "OVMXB", VMS_PE_VC_OPEN),

	/* Heal, and the very next retransmission lands. */
	SIM_LINK("OVMXA", "OVMXB", .loss_pct = 0),
	SIM_RUN(9000),
	SIM_EXPECT_NODE(SIM_CMP_EQ, "OVMXB", SIM_M_UPPER_MESSAGES, 1,
			"delivered upward ONCE, after all those retries"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_UNACKED, 0,
			"the cumulative ack drained the ring"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_PEER_RECV_ACK, 1,
			"and the peer's recv_ack advanced to the sequence it "
			"took -- it never froze"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXB", "OVMXA", SIM_M_RECV_SEQ, 1,
			"which is the receiver's own recv_seq, read at its end"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0,
		      "nothing was pipelined behind it, so no gap ever arose"),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2, "and the circuit never broke"),
	SIM_DUMP("after the retransmit window"),
	SIM_END
};

static const struct sim_scenario scn_retransmit =
	SIM_SCENARIO("a retransmit reuses its sequence (spec 4(L))",
		     pair_nodes, steps_retransmit);

static void test_retransmit_reuses_the_sequence(void)
{
	printf("-- loss -> retransmit -> delivered, and the seq is consumed "
	       "ONCE\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_retransmit, 1u,
						    &g_out),
			0, "every expectation held");
	printf("       (%llu retransmissions of one message)\n",
	       (unsigned long long)metric(&g_out, SIM_M_RETRANSMITS));
}

/* ==================================================================== *
 * 5. A duplicate is absorbed, and is NOT a gap
 *
 * Spec §4(h)(4a) counted 506 duplicate sequenced messages across 47 captures
 * and ZERO gaps. One link is set to duplicate everything, one message is sent,
 * and the second copy must be absorbed and NOT delivered twice.
 * ==================================================================== */

static const struct sim_step steps_dup[] = {
	SIM_UNTIL_ALL_VCS_OPEN(60000),
	SIM_LINK("OVMXA", "OVMXB", .dup_pct = 100),
	SIM_SEND("OVMXA", "OVMXB", 1),
	SIM_RUN(5000),
	SIM_EXPECT_GE(SIM_M_LAN_DUPED, 1, "the wire really duplicated it"),
	SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXB", "OVMXA", SIM_M_RX_DUPS, 1,
			"the port scored the second copy as a DUPLICATE"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0, "and not as a p.2-31 gap"),
	SIM_EXPECT_NODE(SIM_CMP_EQ, "OVMXB", SIM_M_UPPER_MESSAGES, 1,
			"it was delivered upward exactly ONCE"),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2, "and the circuit survived"),
	SIM_END
};

static const struct sim_scenario scn_dup =
	SIM_SCENARIO("a duplicate is absorbed, never delivered twice",
		     pair_nodes, steps_dup);

static void test_duplicate_is_absorbed(void)
{
	printf("-- a duplicate is absorbed and delivered once (4(h)(4a))\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_dup, 1u, &g_out), 0,
			"every expectation held");
}

/* ==================================================================== *
 * 6. THE DONE-CONDITION: three stacks at 10 % loss, eight seeds
 * ==================================================================== */

static const struct sim_step steps_lossy[] = {
	SIM_LINKS_ALL(.loss_pct = 10),
	SIM_UNTIL_ALL_VCS_OPEN(180000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS,
		      "every circuit reached OPEN under 10 % loss"),
	SIM_UNTIL_ALL_CHANNELS(120000),
	SIM_EXPECT_EQ(SIM_M_CHANNELS_VERIFIED, N_PAIRS,
		      "every channel verified"),
	SIM_EXPECT_GE(SIM_M_LAN_LOST, 1,
		      "the wire really did lose frames on this run"),
	SIM_EXPECT_EQ(SIM_M_RX_GAPS, 0,
		      "loss during formation is not a gap"),
	SIM_EXPECT_EQ(SIM_M_TX_ERRORS, 0, "no transmit failed"),
	SIM_EXPECT_EQ(SIM_M_B2_TX, 0, "still no b2 originated"),
	SIM_EXPECT_EQ(SIM_M_LAN_QUEUE_FULL, 0, "the harness dropped nothing"),
	SIM_EXPECT_EQ(SIM_M_TIMER_OVERFLOWS, 0, "and armed every timer"),
	SIM_END
};

static const struct sim_scenario scn_lossy =
	SIM_SCENARIO("3 nodes, 10 % loss on every directed link",
		     three_nodes, steps_lossy);

static void test_three_nodes_10pct_loss(void)
{
	uint64_t seed, total_lost = 0u;
	int all_green = 1;

	printf("-- THE DONE-CONDITION: 3 pe stacks form VCs under 10 %% loss\n");
	for (seed = 1u; seed <= 8u; seed++) {
		if (run_scenario(&scn_lossy, seed, &g_out) != 0) {
			printf("       seed %llu FAILED\n",
			       (unsigned long long)seed);
			all_green = 0;
			continue;
		}
		total_lost += metric(&g_out, SIM_M_LAN_LOST);
		printf("       seed %llu: 6/6 circuits + 6/6 channels, "
		       "%llu frames lost, %llu 0x41 frames, T+%llu ms, "
		       "%llu events\n", (unsigned long long)seed,
		       (unsigned long long)metric(&g_out, SIM_M_LAN_LOST),
		       (unsigned long long)formation_frames(&g_out),
		       (unsigned long long)g_out.sim.clock.now_ms,
		       (unsigned long long)g_out.sim.events);
	}
	ct_check(all_green, "all eight seeds formed every circuit under loss");
	ct_check(total_lost > 0u, "and the loss condition really fired");
}

/* ==================================================================== *
 * 7. Pipelined traffic under loss: the p. 2-31 behaviour, and RECOVERY
 *
 * Eight messages per ordered pair, sent back to back into a 10 %-loss wire.
 * When one is lost the receiver sees the next sequence as a GAP and breaks the
 * circuit, exactly as vms_pe_fsm.h §3b(4) says it will. What this scenario
 * asserts is what the FSM actually promises: every broken circuit RE-FORMS,
 * every unacked ring drains, and nothing arrives that cannot be routed. The
 * gaps and re-formations are printed as observations.
 * ==================================================================== */

#define DATA_MSGS 8u

static const struct sim_step steps_pipelined[] = {
	SIM_LINKS_ALL(.loss_pct = 10),
	SIM_UNTIL_ALL_VCS_OPEN(180000),
	SIM_SEND("OVMXA", "OVMXB", DATA_MSGS),
	SIM_SEND("OVMXA", "OVMXC", DATA_MSGS),
	SIM_SEND("OVMXB", "OVMXA", DATA_MSGS),
	SIM_SEND("OVMXB", "OVMXC", DATA_MSGS),
	SIM_SEND("OVMXC", "OVMXA", DATA_MSGS),
	SIM_SEND("OVMXC", "OVMXB", DATA_MSGS),
	SIM_EXPECT_EQ(SIM_M_MSGS_TX, N_PAIRS * DATA_MSGS,
		      "every message consumed exactly one sequence"),
	SIM_EXPECT_EQ(SIM_M_SEND_REFUSED, 0,
		      "the credit window took all eight (the grant is 10)"),
	SIM_RUN(180000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS,
		      "every circuit is OPEN again at the end"),
	SIM_EXPECT_EQ(SIM_M_UNACKED, 0, "and every unacked ring drained"),
	SIM_EXPECT_EQ(SIM_M_VC_RX_UNDELIVERED, 0,
		      "nothing arrived that could not be routed"),
	SIM_EXPECT_EQ(SIM_M_TX_ERRORS, 0, "and no transmit failed"),
	SIM_DUMP("after the pipelined data phase"),
	SIM_END
};

static const struct sim_scenario scn_pipelined =
	SIM_SCENARIO("pipelined messages across a 10 % loss wire",
		     three_nodes, steps_pipelined);

static void test_pipelined_under_loss(void)
{
	uint64_t seed;
	int all_green = 1;

	printf("-- pipelined traffic under loss: gaps happen, and it "
	       "RECOVERS\n");
	for (seed = 1u; seed <= 4u; seed++) {
		if (run_scenario(&scn_pipelined, seed, &g_out) != 0) {
			printf("       seed %llu FAILED\n",
			       (unsigned long long)seed);
			all_green = 0;
			continue;
		}
		printf("       seed %llu: %llu sent, %llu delivered upward, "
		       "%llu retransmits, %llu gaps, %llu re-formations\n",
		       (unsigned long long)seed,
		       (unsigned long long)metric(&g_out, SIM_M_MSGS_TX),
		       (unsigned long long)metric(&g_out,
						  SIM_M_UPPER_MESSAGES),
		       (unsigned long long)metric(&g_out, SIM_M_RETRANSMITS),
		       (unsigned long long)metric(&g_out, SIM_M_RX_GAPS),
		       (unsigned long long)metric(&g_out,
						  SIM_M_VC_REFORMATIONS));
	}
	ct_check(all_green, "every seed recovered every circuit");
}

/* ==================================================================== *
 * 8. Determinism by seed -- the plan's other half
 * ==================================================================== */

static void test_determinism_by_seed(void)
{
	int same_trace, same_dump, diff_trace;

	printf("-- the same seed is the same run, byte for byte\n");

	(void)sim_scenario_run(&scn_pipelined, 7u, &g_out);
	(void)sim_scenario_run(&scn_pipelined, 7u, &g_out2);
	same_trace = g_out.trace == g_out2.trace;
	same_dump = g_out.dump.len == g_out2.dump.len &&
		    memcmp(g_out.dump.b, g_out2.dump.b, g_out.dump.len) == 0;

	ct_check(same_trace,
		 "two runs on seed 7 folded the identical event trace");
	ct_check(same_dump,
		 "and produced a byte-identical SDA-like snapshot");
	ct_check_eq_u32((unsigned long)g_out.failures,
			(unsigned long)g_out2.failures,
			"with the same result");
	ct_check(g_out.sim.events == g_out2.sim.events &&
		 g_out.sim.clock.now_ms == g_out2.sim.clock.now_ms,
		 "same event count, same elapsed virtual time");
	ct_check(g_out.dump.len > 1000u && !g_out.dump.overflow,
		 "and the snapshot compared is a whole one, not an empty "
		 "buffer matching another empty buffer");
	printf("       (trace %016llX, %llu events, T+%llu ms, %u dump "
	       "bytes)\n", (unsigned long long)g_out.trace,
	       (unsigned long long)g_out.sim.events,
	       (unsigned long long)g_out.sim.clock.now_ms,
	       (unsigned)g_out.dump.len);

	/* The negative control. A harness that returns the same digest for
	 * every seed is inert, and "deterministic" would mean nothing. */
	(void)sim_scenario_run(&scn_pipelined, 8u, &g_out2);
	diff_trace = g_out.trace != g_out2.trace;
	ct_check(diff_trace, "a DIFFERENT seed produced a different run");
	ct_check(g_out2.sim.lan.lost != g_out.sim.lan.lost ||
		 g_out2.sim.events != g_out.sim.events,
		 "and a materially different one, not just a different digest");
}

/* ==================================================================== *
 * 9. A partition, and the re-formation after it
 *
 * The link between two nodes is severed. The circuit goes down, the layer
 * above is told, and when the partition heals the port re-forms it without
 * anybody asking. The third node keeps running throughout, which is what makes
 * this a cluster test rather than a link test.
 * ==================================================================== */

static const struct sim_step steps_partition[] = {
	SIM_UNTIL_ALL_VCS_OPEN(60000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS, "all six up to begin with"),

	SIM_CUT("OVMXA", "OVMXB"),
	SIM_RUN(60000),
	SIM_EXPECT_VC("OVMXA", "OVMXB", VMS_PE_VC_CLOSED),
	SIM_EXPECT_VC("OVMXB", "OVMXA", VMS_PE_VC_CLOSED),
	SIM_EXPECT_VC("OVMXA", "OVMXC", VMS_PE_VC_OPEN),
	SIM_EXPECT_VC("OVMXC", "OVMXB", VMS_PE_VC_OPEN),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS - 2u,
		      "the other four are untouched"),
	SIM_EXPECT_GE(SIM_M_VC_DOWNS, 2, "both ends recorded the loss"),
	SIM_EXPECT_GE(SIM_M_UPPER_DOWNS, 2, "and told the layer above"),
	SIM_DUMP("partitioned"),

	SIM_HEAL("OVMXA", "OVMXB"),
	SIM_UNTIL_ALL_VCS_OPEN(120000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, N_PAIRS, "the circuit came back"),
	SIM_EXPECT_GE(SIM_M_VC_OPENS, N_PAIRS + 2u,
		      "and it OPENED a second time, not just stayed up"),
	SIM_END
};

static const struct sim_scenario scn_partition =
	SIM_SCENARIO("partition and re-formation", three_nodes,
		     steps_partition);

static void test_partition_and_reformation(void)
{
	printf("-- a partition takes the circuit down; healing brings it "
	       "back\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_partition, 3u,
						    &g_out),
			0, "every expectation held");
}

/* ==================================================================== *
 * 10. A clean leave: the §4(O.30) last gasp
 * ==================================================================== */

static const struct sim_step steps_halt[] = {
	SIM_UNTIL_ALL_VCS_OPEN(60000),
	SIM_HALT("OVMXC"),
	SIM_RUN(10000),
	SIM_EXPECT_VC("OVMXA", "OVMXC", VMS_PE_VC_CLOSED),
	SIM_EXPECT_VC("OVMXB", "OVMXC", VMS_PE_VC_CLOSED),
	SIM_EXPECT_VC("OVMXA", "OVMXB", VMS_PE_VC_OPEN),
	SIM_EXPECT_VC("OVMXB", "OVMXA", VMS_PE_VC_OPEN),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2,
		      "the two survivors keep their circuit"),
	SIM_END
};

static const struct sim_scenario scn_halt =
	SIM_SCENARIO("an announced departure (spec 4(O.30))", three_nodes,
		     steps_halt);

static void test_announced_departure(void)
{
	printf("-- a last gasp closes the departing node's circuits at once\n");
	ct_check_eq_u32((unsigned long)run_scenario(&scn_halt, 1u, &g_out), 0,
			"every expectation held");
}

int main(int argc, char **argv)
{
	g_verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
	printf("test_sim_vc: R2 -- N pure pe stacks on a virtual LAN "
	       "(FC-P1.4)\n");

	test_three_nodes_lossless();
	test_formation_retry_without_randomness();
	test_loss_forces_more_formation_frames();
	test_retransmit_reuses_the_sequence();
	test_duplicate_is_absorbed();
	test_three_nodes_10pct_loss();
	test_pipelined_under_loss();
	test_determinism_by_seed();
	test_partition_and_reformation();
	test_announced_departure();

	return ct_summary("test_sim_vc");
}
