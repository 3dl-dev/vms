/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/pe_stalled_guest.c - rd vms-8c54's rung-R2 leg: a node whose CPU
 * STOPS, and what it does with the frames that were waiting for it.
 *
 * WHY THIS IS NOT ANOTHER LOSS SCENARIO, AND WHY THAT IS THE WHOLE ITEM.
 *
 * Every fault this simulator could express before this file DESTROYS frames:
 * SIM_CUT partitions a pair, SIM_LINK's loss_pct drops them, SIM_NIC_DOWN
 * takes the port off the wire. Under all three the node KEEPS RUNNING, its
 * timers keep firing, and it observes SS4(M)'s listen deadline pass on its own
 * beat -- which is why nine months of blackout rigs never produced the field
 * failure this scenario is named for.
 *
 * A STALLED GUEST DEFERS INSTEAD. A visitor's slower machine starves the
 * emulator: the CPU stops, so no timer of that node fires, and the frames
 * addressed to it sit in its receive queue undamaged. When it runs again its
 * CLOCK AND ITS QUEUE JUMP TOGETHER and the queued frames are consumed BEFORE
 * any beat runs -- so every receive path that refreshes the listen deadline
 * gets to refresh one that had ALREADY expired. SIM_STALL is that fault, and
 * it touches nothing on the wire.
 *
 * WHAT WAS MEASURED, AND WHERE. Rig arm N-1
 * (tests/lab/captures/vms-8c54-stalled-guest-20260928/): an OVMX member,
 * admitted 2 s earlier beside a real OpenVMS VAX V7.3 founder, had its QEMU
 * process SIGSTOPped for 10 s. The VAX closed its virtual circuit 8.2 s in.
 * On wake the OVMX node put 0x5b/0x4b/0x48 sequenced traffic on a circuit that
 * no longer existed, and when the VAX re-verified the channel and sent a fresh
 * 0x41 START, the OVMX node answered STACK from a circuit it still believed
 * was OPEN -- it NEVER SENT A START OF ITS OWN. The VAX never acknowledged
 * that STACK: it re-STARTed every 5 s for the rest of the run while the OVMX
 * node re-sent its STACK eight times a beat, and 20 s later each side removed
 * the other from the cluster.
 *
 * THE ORACLE, the same 10 s SIGSTOP applied to a REAL V7.3 member beside a
 * real V7.3 survivor (oracle/oracle-stall-o1.pcap, oracle-stall2.out): the
 * stalled node logged "%CNXMAN, lost connection to system VAX1" 1.4 s after
 * wake -- it NOTICED -- and then BOTH sides sent their own START, STACKed each
 * other and ACKed, four frames in one millisecond. Neither node was removed
 * and no transition was proposed.
 *
 * SO THE ASSERTION IS THE ORACLE'S BEHAVIOUR, and the one that discriminates
 * is the stalled node's OWN START: with the deadline read only on the beat,
 * this node answers and never initiates, which is exactly the wire the rig
 * captured.
 *
 * WHAT THIS SCENARIO CANNOT SHOW, said plainly. Both nodes here are OVMX, and
 * an OVMX peer is more forgiving than the real V7.3 was: it acknowledges the
 * STACK, so even with the deadline read only on the beat the circuit does
 * eventually re-open (later, and driven entirely by the PEER). What the real
 * VAX did instead -- refuse to complete a re-formation the woken node never
 * started, re-STARTing every 5 s until each side removed the other -- is a
 * property of ITS connection manager and is proved where it was measured, on
 * the rig. So the assertion that discriminates here is the STALLED NODE'S OWN
 * START, which is the frame the capture shows missing; SIM_M_VC_DOWNS is
 * recorded beside it because the oracle's node tore its circuit down too, not
 * because it alone would catch the defect.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1. The stall is a real interval on the
 * virtual clock with a real receive queue behind it, the re-formation is a
 * bounded number of beats rather than a dispatched event, and every value
 * asserted is read back out of the SHIPPING struct pe_vc / struct pe_fsm.
 */
#include <stdio.h>

#include "cluster_test.h"
#include "sim_scenario.h"

/*
 * TWO NODES, because the fault is about one pair's circuit and a third would
 * only add circuits that prove the same thing twice.
 */
static const struct sim_node_decl pair[] = {
	SIM_NODE("OVMXA", 1987),
	SIM_NODE("OVMXB", 1988)
};

/*
 * THE STALL IS LONGER THAN THE LISTEN TIMEOUT (PE_LISTEN_TIMEOUT_DEFAULT_MS is
 * 8 s, the oracle's measured port constant -- rd vms-b98; the HELLO cadence
 * 2 s), which is the case the field failure is: the deadline expired while the
 * node was not running, and roughly a dozen of the peer's HELLOs were waiting
 * for it when it came back.
 */
static const struct sim_step steps[] = {
	SIM_UNTIL_ALL_VCS_OPEN(120000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2, "both circuits formed first"),
	SIM_EXPECT_NODE(SIM_CMP_EQ, "OVMXB", SIM_M_STARTS_TX, 1,
			"and OVMXB started exactly one of them"),

	/* The visitor's machine. Nothing on the wire is touched. */
	SIM_STALL("OVMXB", 25000),
	SIM_RUN(40000),

	/* IT NOTICED. The deferred frames did not revive a channel whose
	 * deadline had already run out. */
	SIM_EXPECT_NODE(SIM_CMP_GE, "OVMXB", SIM_M_VC_DOWNS, 1,
			"the woken node tore down the circuit it had lost"),
	SIM_EXPECT_NODE(SIM_CMP_GE, "OVMXB", SIM_M_STARTS_TX, 2,
			"and re-formed it from ITS OWN side with a START -- "
			"the frame the rig measured missing"),

	/* AND THE PAIR IS WHOLE AGAIN, which is the oracle's outcome: no node
	 * removed, no cluster transition, both circuits carrying again. */
	SIM_UNTIL_ALL_VCS_OPEN(120000),
	SIM_EXPECT_EQ(SIM_M_VCS_OPEN, 2, "both circuits are OPEN again"),
	SIM_END
};

static const struct sim_scenario scn =
	SIM_SCENARIO("a stalled guest notices, and re-forms from its own side",
		     pair, steps);

static struct sim_scenario_out g_out;   /* large: static, not a frame */

int main(void)
{
	int failures;

	printf("pe_stalled_guest (rd vms-8c54, rung R2): SIGSTOP DEFERS, "
	       "a dark wire DESTROYS\n");
	failures = sim_scenario_run(&scn, 1u, &g_out);
	if (failures != 0)
		sim_scenario_report(&scn, &g_out, 0);
	ct_check_eq_u32((unsigned long)failures, 0, "every expectation held");

	/* The fault really happened, in this run, read back out of the node
	 * the harness stalled -- not assumed from the step having been listed
	 * (the rig taught this the expensive way: a whole matrix once graded
	 * six green arms whose injector had silently never fired). */
	ct_check(g_out.sim.node[1].stall_frames_deferred >= 1u,
		 "frames really were DEFERRED into the stalled node's queue");
	ct_check(g_out.sim.node[1].stall_dropped_timers >= 1u,
		 "and its own beats really did not run while it was stopped");
	ct_check_eq_u32(g_out.sim.node[1].stall_queue_overflow, 0,
			"and NOTHING was destroyed: the queue held every "
			"deferred frame, which is what makes this fault "
			"different from a dark wire");
	printf("     (%u frames deferred, %u timer fires dropped, "
	       "re-opened by T+%llu ms)\n",
	       g_out.sim.node[1].stall_frames_deferred,
	       g_out.sim.node[1].stall_dropped_timers,
	       (unsigned long long)g_out.sim.clock.now_ms);
	return ct_summary("pe_stalled_guest");
}
