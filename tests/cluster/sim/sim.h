/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim.h - the rung-2 host cluster simulator's ENGINE: N instances of the pure
 * stack, one virtual LAN, one virtual clock, one deterministic event loop
 * (FC-P1.4).
 *
 * Design: docs/design-faithful-cluster-executive.md §3.9, test-ladder rung 2 --
 * "N instances of the PURE stack wired to a virtual LAN with configurable
 * loss/reorder/duplication/latency and a virtual clock ... in milliseconds,
 * deterministic, with the simulator's own SDA-like snapshots". Plan row
 * FC-P1.4.
 *
 * ---------------------------------------------------------------------------
 * THE EVENT LOOP, AND WHY IT IS THE WAY IT IS
 *
 * There are exactly two kinds of thing that can happen: a frame arrives, or a
 * timer expires. The loop repeatedly takes the EARLIEST of the two, moves the
 * virtual clock to it, and dispatches it -- it never advances time by a fixed
 * step and it never polls. Consequences worth knowing before writing a
 * scenario:
 *
 *   - A 16-second TIMVCFAIL costs one loop iteration, not 16 000.
 *   - A timer the FSM armed and nothing re-arms is VISIBLE: the wheel empties
 *     and the run ends early, instead of a poll hiding the stall.
 *   - The order is TOTAL. At the same millisecond, a frame already on the wire
 *     is delivered before a timer expires (it was sent earlier); ties inside
 *     each kind break by (serial) and (node, timer, key, serial). Two runs on
 *     one seed therefore dispatch the identical sequence of events, which is
 *     what "deterministic by seed" has to mean to be worth anything.
 *
 * ---------------------------------------------------------------------------
 * THE TRACE, AND WHY DETERMINISM IS PROVEN RATHER THAN ASSERTED
 *
 * Every dispatched event is folded into a 64-bit rolling digest: its kind, the
 * millisecond it happened, the node, and -- for a delivery -- the frame's
 * bytes. Two runs are identical iff their digests match, and the digest is
 * cheap enough to keep switched on for every scenario. The determinism test
 * also compares the full SDA-like dump text, so a difference the digest could
 * in principle miss would still be caught, and it runs a DIFFERENT seed as a
 * negative control -- a "deterministic" harness that produces the same digest
 * for every seed is an inert one.
 *
 * ---------------------------------------------------------------------------
 * HOST-ONLY. tests/cluster/sim/ builds with a plain host compiler against
 * src/kernel-core alone, with no kernel headers on the include path. It is NOT
 * cluster-core code: tools/ci/cluster_core_includes_gate.sh scans
 * src/kernel-core, and this directory is a test harness on the same footing as
 * tests/cluster/host.
 */
#ifndef OVMX_SIM_H
#define OVMX_SIM_H

#include <stdint.h>

#include "sim_clock.h"
#include "sim_lan.h"
#include "sim_node.h"

/* What the loop dispatched. Recorded in the trace, so the enum's values are
 * part of the determinism contract: never renumber them. */
enum sim_event_kind {
	SIM_EV_NONE     = 0,
	SIM_EV_DELIVERY = 1,   /* a frame reached a port */
	SIM_EV_TIMER    = 2    /* an armed pe_timer expired */
};

/*
 * A hard ceiling on events per run. A scenario that hits it has found either a
 * livelock in the stack or a mistake in itself; either way the run stops and
 * says so, rather than hanging a CI job. Generous: a 3-node 120 s run at 10 %
 * loss dispatches a few thousand.
 */
#define SIM_MAX_EVENTS 2000000u

struct sim {
	uint64_t         seed;
	struct sim_clock clock;
	struct sim_lan   lan;
	uint32_t         n_nodes;
	struct sim_node  node[SIM_MAX_NODES];

	/* The determinism witness (see the file comment). */
	uint64_t trace;
	uint64_t events;          /* dispatched, total */
	uint64_t deliveries;
	uint64_t timer_fires;
	uint8_t  event_cap_hit;   /* SIM_MAX_EVENTS reached: a loud failure */
	uint8_t  pad[7];
};

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void sim_init(struct sim *s, uint64_t seed);

/* Add a node from its config. Returns its index, or -1 when the table is full
 * or the LAN refused the port. Does not boot it. */
int  sim_add_node(struct sim *s, const struct sim_node_cfg *cfg);

/* Boot every node that is not booted yet, at the current virtual instant.
 * Returns the number booted, or -1 if any node's config was refused. */
int  sim_boot_all(struct sim *s);

struct sim_node *sim_node_by_name(struct sim *s, const char *name);
struct sim_node *sim_node_at(struct sim *s, uint32_t index);

/* ==========================================================================
 * Running
 * ========================================================================== */

/* Advance the virtual clock by `ms`, dispatching every event that falls in the
 * window. Returns the number of events dispatched. */
uint64_t sim_run_ms(struct sim *s, uint32_t ms);

/*
 * Run until `pred` returns non-zero, or until `timeout_ms` of virtual time has
 * elapsed. Returns 1 if the predicate held (the clock stops at the event that
 * made it true, which is what makes "how long did it take" meaningful), 0 on
 * timeout. The predicate is evaluated after every dispatched event.
 */
typedef int (*sim_predicate_fn)(struct sim *s, void *ctx);
int sim_run_until(struct sim *s, sim_predicate_fn pred, void *ctx,
		  uint32_t timeout_ms);

/* The two predicates every phase wants; more live with their scenarios. */
int sim_all_vcs_open(struct sim *s, void *unused);
int sim_all_channels_verified(struct sim *s, void *unused);

/* ==========================================================================
 * Reading the cluster back -- always from real FSM state
 * ========================================================================== */

/* Circuits in state OPEN across the whole simulated cluster. With N nodes all
 * connected that is N*(N-1): a circuit is one-ended, and each end has its own. */
uint32_t sim_count_vcs_open(struct sim *s);
uint32_t sim_count_channels_verified(struct sim *s);

/* Elapsed virtual milliseconds. */
uint64_t sim_now_ms(const struct sim *s);

/* ==========================================================================
 * Internals the LAN-facing ops thunks need (sim_node.c calls these)
 * ========================================================================== */
void sim_trace_mix(struct sim *s, uint64_t v);
void sim_trace_bytes(struct sim *s, const uint8_t *b, uint32_t len);

#endif /* OVMX_SIM_H */
