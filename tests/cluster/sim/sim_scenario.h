/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_scenario.h - THE SCENARIO DSL. This is the file a later phase's author
 * reads; everything else in tests/cluster/sim/ is machinery underneath it
 * (FC-P1.4).
 *
 * ===========================================================================
 * A WHOLE SCENARIO, TOP TO BOTTOM
 * ===========================================================================
 *
 *   #include "sim_scenario.h"
 *
 *   static const struct sim_node_decl nodes[] = {
 *           SIM_NODE("OVMXA", 1030),
 *           SIM_NODE("OVMXB", 1031),
 *           SIM_NODE("OVMXC", 1032),
 *   };
 *
 *   static const struct sim_step steps[] = {
 *           SIM_LINKS_ALL(.loss_pct = 10),          // every directed link
 *           SIM_LINK("OVMXA", "OVMXB", .loss_pct = 30, .latency_ms = 4),
 *           SIM_UNTIL_ALL_VCS_OPEN(120000),
 *           SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 6, "all six circuits OPEN"),
 *           SIM_EXPECT_VC("OVMXA", "OVMXB", VMS_PE_VC_OPEN),
 *           SIM_SEND("OVMXA", "OVMXB", 6),
 *           SIM_RUN(30000),
 *           SIM_EXPECT_PAIR(SIM_CMP_EQ, "OVMXA", "OVMXB", SIM_M_UNACKED, 0,
 *                           "the cumulative ack drained the ring"),
 *           SIM_EXPECT_NODE(SIM_CMP_EQ, "OVMXB", SIM_M_UPPER_MESSAGES, 6,
 *                           "and SCS's stand-in was given all six"),
 *           SIM_DUMP("after the data phase"),
 *           SIM_END
 *   };
 *
 *   static const struct sim_scenario scn =
 *           SIM_SCENARIO("3 nodes at 10 % loss", nodes, steps);
 *
 *   ... sim_scenario_run(&scn, /- seed -/ 1, &out);
 *
 * ===========================================================================
 * FOUR RULES THE DSL ENFORCES, AND WHY
 * ===========================================================================
 *
 * 1. AN EXPECTATION READS REAL FSM STATE. Every `SIM_M_*` metric is a value
 *    read out of `struct pe_fsm` / `struct pe_vc` / the virtual LAN's own
 *    counters at the moment the step runs. There is no scenario-side model of
 *    the protocol to compare against, and there is no metric that a step could
 *    have SET. If a number cannot be traced to a real read, it is not a metric
 *    (INV-6).
 *
 * 2. TIME IS ALWAYS EXPLICIT. A step either advances the clock (SIM_RUN,
 *    SIM_UNTIL_*) or it does not; nothing happens "in the background". A
 *    scenario reads as a timeline.
 *
 * 3. A FAILING EXPECTATION DOES NOT STOP THE RUN. Every step executes and
 *    every failure is reported with the metric, the operator, what was wanted
 *    and what was read -- because the second failure usually explains the
 *    first, and re-running to find it costs a cycle.
 *
 * 4. THE SAME SEED IS THE SAME RUN. `sim_scenario_run` takes the seed
 *    explicitly, and hands back the trace digest and the final snapshot so a
 *    test can compare two runs byte for byte.
 */
#ifndef OVMX_SIM_SCENARIO_H
#define OVMX_SIM_SCENARIO_H

#include <stdint.h>

#include "sim.h"
#include "sim_dump.h"
#include "sim_msg.h"

/* ==========================================================================
 * 1. Declaring nodes
 *
 * A declaration is a name and an SCSSYSTEMID; everything else takes the
 * harness default (`sim_node_cfg_default`) unless the scenario overrides it
 * with the designated initialisers of `struct sim_node_cfg`.
 * ========================================================================== */
struct sim_node_decl {
	const char         *name;
	uint16_t            sysid;
	uint8_t             have_override;
	struct sim_node_cfg override;   /* used only when have_override */
};

#define SIM_NODE(nm, sid)     { (nm), (uint16_t)(sid), 0u, { 0 } }

/* A node that differs from the default in some way. `...` are designated
 * initialisers of `struct sim_node_cfg`; `name` and `sysid` are filled in for
 * you, and any field left out keeps the harness default. */
#define SIM_NODE_CFG(nm, sid, ...) \
	{ (nm), (uint16_t)(sid), 1u, { __VA_ARGS__ } }

/* ==========================================================================
 * 2. Metrics -- what an expectation may read
 *
 * Grouped by where the number comes FROM, because that is the question to ask
 * of any assertion: which real object did this come out of?
 * ========================================================================== */
enum sim_metric {
	/* --- from the circuits (struct pe_vc), summed over the scope ---- */
	SIM_M_VCS_OPEN = 0,     /* circuits in state OPEN                  */
	/* The scoped circuit's own state (enum vms_pe_vc_state). Meant for a
	 * PAIR scope; with no such circuit it reads CLOSED, which is what "no
	 * circuit" means and is not a value anybody claimed. */
	SIM_M_VC_STATE,
	/* The scoped channel's state (enum vms_pe_channel_state), PAIR scope:
	 * the channel node->peer, found by the peer's real hardware address. */
	SIM_M_CHANNEL_STATE,
	SIM_M_VC_OPENS,         /* times a circuit REACHED open            */
	SIM_M_VC_DOWNS,         /* times one was torn down                 */
	/*
	 * Formation retries OF THE ATTEMPT IN PROGRESS. Read this MID-FLIGHT:
	 * vms_pe_fsm.c's vc_open() / vc_close() / vc_reset_sequence() all
	 * clear pe_vc.form_tries, because it is the current attempt's retry
	 * count and not a lifetime total. A scenario that asserts on it AFTER
	 * SIM_UNTIL_ALL_VCS_OPEN will read 0 on a circuit that retried ten
	 * times. To prove a formation was retransmitted, either
	 *   - SIM_RUN past the first retry deadline and read it while the
	 *     circuit is still forming (what test_sim_vc.c does), or
	 *   - assert on SIM_M_STARTS_TX / SIM_M_STACKS_TX, which are lifetime
	 *     counters and are never cleared.
	 * This is the FSM's real shape, not a harness limitation, and it is
	 * documented here rather than worked around.
	 */
	SIM_M_VC_FORM_TRIES,
	SIM_M_STARTS_TX,        /* 0x41 STARTs sent                        */
	SIM_M_STACKS_TX,        /* 0x41 STACKs sent                        */
	SIM_M_ACKS_TX,          /* round-2 ACKs sent                       */
	SIM_M_MSGS_TX,          /* sequenced messages sent                 */
	SIM_M_MSGS_RX,          /* sequenced messages received             */
	SIM_M_CREDIT_TX,        /* 0x48 credit-returns sent                */
	SIM_M_CREDIT_RX,
	SIM_M_RETRANSMITS,      /* SAME seq re-sent (spec 4(L))            */
	SIM_M_RX_DUPS,          /* peer retransmits absorbed               */
	SIM_M_RX_GAPS,          /* p. 2-31 breaks: normally asserted 0     */
	SIM_M_IMPLIED_ACKS,     /* p. 2-16 implied opens                   */
	SIM_M_SEND_SEQ,         /* the circuit's next sequence             */
	SIM_M_RECV_SEQ,         /* highest sequence taken in order         */
	SIM_M_PEER_RECV_ACK,    /* the cumulative ack the PEER last sent   */
	SIM_M_UNACKED,          /* messages in the unacked ring            */
	SIM_M_SEND_REFUSED,     /* sends refused for credit or ring        */

	/* --- from the channels (struct pe_channel) ---------------------- */
	SIM_M_CHANNELS_VERIFIED,/* channels at b4                          */
	SIM_M_CHANNEL_RESETS,   /* peer re-formed the channel (4(i).B)     */
	SIM_M_B3_TX,
	SIM_M_B4_TX,
	SIM_M_B2_TX,            /* GROUNDED ZERO: OVMX never originates b2 */

	/* --- from the port (struct pe_fsm) ------------------------------ */
	SIM_M_TX_ERRORS,
	SIM_M_IGNORED_EVENTS,   /* channel-table cells with no edge        */
	SIM_M_VC_IGNORED_EVENTS,
	SIM_M_VC_NO_INCARNATION,/* 4(i).B echo absent: NO START built      */
	SIM_M_VC_NO_IDENTITY,
	SIM_M_VC_REFORMATIONS,
	SIM_M_VC_RX_NO_CIRCUIT,
	SIM_M_VC_RX_UNDELIVERED,
	SIM_M_NONCE_ABSENT,     /* design 5.3 open: counted, never faked   */
	SIM_M_MCAST_HELLO_TX,

	/* --- from the layer above (the recorder) ------------------------ */
	SIM_M_UPPER_MESSAGES,
	SIM_M_UPPER_UPS,
	SIM_M_UPPER_DOWNS,
	/*
	 * Added by FC-P1.9. A delivery whose send_seq -- read out of the
	 * delivered frame through the codec -- was not exactly one more than
	 * the previous delivery from that peer (or not 1 on a freshly formed
	 * circuit, §4(i).A). This is the "all delivered IN ORDER" half of
	 * design §3.2.5's R2 acceptance; UPPER_MESSAGES alone would pass a
	 * stack that delivered everything shuffled.
	 */
	SIM_M_UPPER_OUT_OF_ORDER,
	/* The `reason` the port raised vc_down() with, most recent first (an
	 * `enum pe_vc_down_reason`). 0 means the hook never fired. */
	SIM_M_UPPER_DOWN_REASON,

	/* --- from the virtual LAN (whole-cluster only) ------------------ */
	SIM_M_LAN_TX,
	SIM_M_LAN_DELIVERED,
	SIM_M_LAN_LOST,
	SIM_M_LAN_DUPED,
	SIM_M_LAN_REORDERED,
	SIM_M_LAN_QUEUE_FULL,   /* harness overflow: asserted 0            */

	/* --- from the engine -------------------------------------------- */
	SIM_M_TIMER_OVERFLOWS,  /* harness overflow: asserted 0            */
	SIM_M_NOW_MS,           /* elapsed virtual milliseconds            */

	SIM_M__COUNT
};

const char *sim_metric_name(enum sim_metric m);

/* Read a metric. `node` NULL means every node; `peer` NULL means every
 * circuit of the scoped node(s). LAN and engine metrics ignore both. */
uint64_t sim_metric_read(struct sim *s, enum sim_metric m, const char *node,
			 const char *peer);

enum sim_cmp { SIM_CMP_EQ = 0, SIM_CMP_GE, SIM_CMP_LE, SIM_CMP_GT,
	       SIM_CMP_LT, SIM_CMP_NE };

/* ==========================================================================
 * 3. Steps
 * ========================================================================== */
enum sim_step_kind {
	SIM_STEP_END = 0,
	SIM_STEP_LINK,          /* one directed link's conditions          */
	SIM_STEP_LINK_PAIR,     /* both directions of one pair             */
	SIM_STEP_LINK_ALL,      /* every directed link                     */
	SIM_STEP_CUT,           /* partition / heal one pair (both ways)   */
	SIM_STEP_NODE_LINK,     /* a node's NIC up / down                  */
	SIM_STEP_HALT,          /* clean leave: last gasp, then shutdown   */
	SIM_STEP_BOOT,          /* boot a node that is not booted          */
	SIM_STEP_RUN,           /* advance the clock                       */
	SIM_STEP_UNTIL_VCS,     /* run until every circuit is OPEN         */
	SIM_STEP_UNTIL_CHANNELS,/* run until every channel is verified     */
	SIM_STEP_SEND,          /* offer N sequenced messages              */
	SIM_STEP_EXPECT,        /* assert on a metric                      */
	SIM_STEP_DUMP,          /* SDA-like snapshot into the report       */
	SIM_STEP__COUNT
};

struct sim_step {
	uint8_t          kind;
	uint8_t          cmp;        /* enum sim_cmp, EXPECT only          */
	uint8_t          metric;     /* enum sim_metric, EXPECT only       */
	uint8_t          flag;       /* up/down, cut/heal                  */
	uint32_t         ms;         /* RUN / UNTIL timeout                */
	uint32_t         count;      /* SEND count                         */
	uint64_t         want;       /* EXPECT right-hand side             */
	const char      *a;          /* first node name, or NULL           */
	const char      *b;          /* second node name, or NULL          */
	const char      *why;        /* what the step is proving           */
	struct sim_link  link;       /* LINK* conditions                   */
};

/* ---- link conditions ------------------------------------------------- */
#define SIM_LINK(from, to, ...) \
	{ SIM_STEP_LINK, 0, 0, 0, 0, 0, 0, (from), (to), NULL, { __VA_ARGS__ } }
#define SIM_LINK_PAIR(x, y, ...) \
	{ SIM_STEP_LINK_PAIR, 0, 0, 0, 0, 0, 0, (x), (y), NULL, { __VA_ARGS__ } }
#define SIM_LINKS_ALL(...) \
	{ SIM_STEP_LINK_ALL, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL, { __VA_ARGS__ } }
#define SIM_CUT(x, y) \
	{ SIM_STEP_CUT, 0, 0, 1, 0, 0, 0, (x), (y), NULL, { 0 } }
#define SIM_HEAL(x, y) \
	{ SIM_STEP_CUT, 0, 0, 0, 0, 0, 0, (x), (y), NULL, { 0 } }

/* ---- node lifecycle --------------------------------------------------- */
#define SIM_NIC_DOWN(n) \
	{ SIM_STEP_NODE_LINK, 0, 0, 0, 0, 0, 0, (n), NULL, NULL, { 0 } }
#define SIM_NIC_UP(n) \
	{ SIM_STEP_NODE_LINK, 0, 0, 1, 0, 0, 0, (n), NULL, NULL, { 0 } }
#define SIM_HALT(n) \
	{ SIM_STEP_HALT, 0, 0, 0, 0, 0, 0, (n), NULL, NULL, { 0 } }
#define SIM_BOOT(n) \
	{ SIM_STEP_BOOT, 0, 0, 0, 0, 0, 0, (n), NULL, NULL, { 0 } }

/* ---- time ------------------------------------------------------------- */
#define SIM_RUN(msec) \
	{ SIM_STEP_RUN, 0, 0, 0, (msec), 0, 0, NULL, NULL, NULL, { 0 } }
#define SIM_UNTIL_ALL_VCS_OPEN(timeout) \
	{ SIM_STEP_UNTIL_VCS, 0, 0, 0, (timeout), 0, 0, NULL, NULL, NULL, { 0 } }
#define SIM_UNTIL_ALL_CHANNELS(timeout) \
	{ SIM_STEP_UNTIL_CHANNELS, 0, 0, 0, (timeout), 0, 0, NULL, NULL, NULL, \
	  { 0 } }

/* ---- traffic ---------------------------------------------------------- */
#define SIM_SEND(from, to, n) \
	{ SIM_STEP_SEND, 0, 0, 0, 0, (n), 0, (from), (to), NULL, { 0 } }

/* ---- expectations ----------------------------------------------------- */
#define SIM_EXPECT(op, m, v, reason) \
	{ SIM_STEP_EXPECT, (op), (m), 0, 0, 0, (uint64_t)(v), NULL, NULL, \
	  (reason), { 0 } }
#define SIM_EXPECT_EQ(m, v, reason) SIM_EXPECT(SIM_CMP_EQ, (m), (v), (reason))
#define SIM_EXPECT_GE(m, v, reason) SIM_EXPECT(SIM_CMP_GE, (m), (v), (reason))
#define SIM_EXPECT_LE(m, v, reason) SIM_EXPECT(SIM_CMP_LE, (m), (v), (reason))

/* The same, scoped to one node, or to one node's circuit to one peer. */
#define SIM_EXPECT_NODE(op, node, m, v, reason) \
	{ SIM_STEP_EXPECT, (op), (m), 0, 0, 0, (uint64_t)(v), (node), NULL, \
	  (reason), { 0 } }
#define SIM_EXPECT_PAIR(op, node, peer, m, v, reason) \
	{ SIM_STEP_EXPECT, (op), (m), 0, 0, 0, (uint64_t)(v), (node), (peer), \
	  (reason), { 0 } }

/* The commonest assertion of all, spelled out: this node's circuit to that
 * peer is in this state. */
#define SIM_EXPECT_VC(node, peer, state) \
	SIM_EXPECT_PAIR(SIM_CMP_EQ, (node), (peer), SIM_M_VC_STATE, (state), \
			"circuit " node " -> " peer)
#define SIM_EXPECT_CHANNEL(node, peer, state) \
	SIM_EXPECT_PAIR(SIM_CMP_EQ, (node), (peer), SIM_M_CHANNEL_STATE, \
			(state), "channel " node " -> " peer)

/* ---- reporting -------------------------------------------------------- */
#define SIM_DUMP(title) \
	{ SIM_STEP_DUMP, 0, 0, 0, 0, 0, 0, NULL, NULL, (title), { 0 } }

#define SIM_END { SIM_STEP_END, 0, 0, 0, 0, 0, 0, NULL, NULL, NULL, { 0 } }

/* ==========================================================================
 * 4. A scenario, and running it
 * ========================================================================== */
struct sim_scenario {
	const char                 *name;
	const struct sim_node_decl *nodes;
	uint32_t                    n_nodes;
	const struct sim_step      *steps;
	uint32_t                    n_steps;
};

#define SIM_SCENARIO(nm, nodearr, steparr) { \
	(nm), (nodearr), (uint32_t)(sizeof(nodearr) / sizeof((nodearr)[0])), \
	(steparr), (uint32_t)(sizeof(steparr) / sizeof((steparr)[0])) }

#define SIM_MAX_FAILURES 16

struct sim_failure {
	uint32_t step;
	char     text[220];
};

struct sim_scenario_out {
	struct sim         sim;      /* the finished cluster, for extra reads */
	struct sim_dump    dump;     /* every SIM_DUMP, plus the final one    */
	uint32_t           checks;
	uint32_t           failures;
	uint32_t           n_recorded;
	struct sim_failure failure[SIM_MAX_FAILURES];
	uint64_t           trace;    /* the determinism witness               */
};

/*
 * Run one scenario at one seed. Returns 0 when every expectation held, or the
 * number of failures. `out` may be NULL (then nothing is reported, which is
 * only useful for a warm-up); it is large -- make it static, not a stack
 * frame.
 */
int sim_scenario_run(const struct sim_scenario *scn, uint64_t seed,
		     struct sim_scenario_out *out);

/* Print the report: every failure, then the accumulated snapshots. */
void sim_scenario_report(const struct sim_scenario *scn,
			 const struct sim_scenario_out *out, int verbose);

#endif /* OVMX_SIM_SCENARIO_H */
