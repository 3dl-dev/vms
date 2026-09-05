/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_recnx_fsm.h - the RECNXINTERVAL/TIMVCFAIL reconnect loop and the
 * last-gasp emission (FC-P3.6).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (CLUB/CSB), SS3.9 (pure
 * FSM, injected ops, injected clock). The CSB ladder this drives is
 * vms_cnxman_csb.h; the structures are vms_cluster.h.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS LAYER IS FOR, IN ONE SENTENCE FROM THE BOOK
 *
 * *VAXcluster Principles* (Davis 1993) p. 7-30: when the local connection
 * manager loses its connection with a remote connection manager for any reason
 * NOT involving a last-gasp datagram, "For a limited period of time, the local
 * Connection Manager will attempt once a second to establish another connection
 * with the remote Connection Manager. If all such 'reconnect' attempts fail, and
 * if no other Connection Manager has already instituted a cluster state
 * transition, then the local Connection Manager starts a state transition to
 * reconfigure the cluster."
 *
 * Three rules follow, and this file exists to get all three right:
 *
 *   1. MEMBERSHIP IS HELD across the break. The same page: "Do not presume that
 *      the remote system has left, or will be leaving the cluster simply
 *      because the local Connection Manager has lost contact". A cluster that
 *      drops a member on the first lost frame reforms constantly.
 *
 *   2. THE PERIOD IS A MAX, NOT RECNXINTERVAL. p. 7-30: the period is "the
 *      maximum of the local value for RECNXINTERVAL and a port dependent number
 *      supplied by the remote Connection Manager". For a LAN circuit that
 *      remote number "was fixed at 16 prior to Version 5.5. But starting with
 *      Version 5.5, the remote system supplies the value of its TIMVCFAIL
 *      parameter." The book's worked example -- local RECNXINTERVAL 20, remote
 *      TIMVCFAIL 10, period 20 s -- is a unit test.
 *
 *   3. THE PROPOSAL IS CONDITIONAL. If another connection manager already
 *      instituted a transition, this node starts none.
 *
 * And on the way out, p. 7-29: a system leaving through SHUTDOWN.COM or a fatal
 * BUGCHECK "sends a 'last gasp' datagram to each of the other VAX systems in
 * the cluster", so the survivors remove it at once instead of waiting out the
 * reconnect period.
 * ---------------------------------------------------------------------------
 *
 * WHAT THIS FILE DOES NOT DO. It sends nothing and it builds no frame. A
 * reconnect is an SCS connect the glue issues; a transition is FC-P3.12's; the
 * last gasp is a PORT-level frame whose byte form is grounded in the wire spec
 * (SS4(O.30): a multicast HELLO with the departure marker at abs 30 and the
 * cluster nonce at abs 68) and is therefore built by the port, which owns that
 * frame class. This FSM decides WHEN, from real CSB state and an injected
 * clock, and hands the decision back as a record. That is what makes the whole
 * reconnect apparatus testable in microseconds with no wire, no VAX and no boot.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_RECNX_FSM_H
#define OVMX_VMS_CNXMAN_RECNX_FSM_H

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"

/* ==========================================================================
 * 1. The published constants
 *
 * All three are PUBLISHED VALUES, not OVMX choices, and each names its source.
 * ========================================================================== */

/* p. 7-30: "will attempt once a second". */
#define CNXMAN_RECNX_ATTEMPT_MS 1000u

/* p. 7-30: for a LAN circuit the remote-supplied number "was fixed at 16 prior
 * to Version 5.5". Seconds. */
#define CNXMAN_RECNX_LAN_PRE_V55_SECS 16u

/*
 * RECNXINTERVAL's own default, from the public OpenVMS System Management
 * Utilities Reference Manual -- the same 20 that tools/vms_sysgen.c's parameter
 * table carries, so the two agree by construction. Used ONLY when SYSGEN
 * carried no RECNXINTERVAL at all: a zero-second reconnect period would abandon
 * a member on the first lost frame, which is precisely what p. 7-30 forbids.
 * Whether the default was used is recorded in the CLUB, not hidden.
 */
#define CNXMAN_RECNX_DEFAULT_SECS 20u

/* ==========================================================================
 * 2. The p. 7-30 arithmetic -- two pure functions, unit-tested against the
 *    book's own worked example
 * ========================================================================== */

/*
 * The number a remote connection manager supplies for a LAN virtual circuit:
 * its TIMVCFAIL from V5.5 on, the fixed 16 before that. `v55_or_later` is a
 * fact about the REMOTE system (its advertised software version), never an
 * OVMX build-time assumption.
 */
uint32_t cnxman_recnx_lan_remote_secs(int v55_or_later,
				      uint32_t remote_timvcfail);

/*
 * The reconnect timeout period, in seconds: the maximum of this node's
 * RECNXINTERVAL and the number the remote connection manager supplied. A remote
 * that supplied nothing contributes 0, so the local value stands alone -- an
 * absent remote value is never invented.
 */
uint32_t cnxman_recnx_period_secs(uint32_t local_recnxinterval,
				  uint32_t remote_port_secs);

/* ==========================================================================
 * 3. What a tick asks the caller to do
 * ========================================================================== */
struct cnxman_recnx_rec {
	uint32_t csb_index;   /* the CLUB's CSB slot this concerns */
	uint8_t  action;      /* enum cnxman_csb_action */
	uint8_t  pad[3];
};

/* ==========================================================================
 * 4. The FSM context
 *
 * No globals (design SS3.9 rule 3): everything hangs off the per-node
 * struct vms_cluster, and the ops carry the clock, the timer and the console.
 * ========================================================================== */
struct cnxman_recnx {
	struct vms_cluster      *cl;
	const struct cnxman_ops *ops;

	uint8_t  running;        /* the once-a-second timer is armed */
	uint8_t  departing;      /* the last gasp went out: propose nothing more */
	uint8_t  pad[2];

	/* Instrumentation, all counted from real dispatches. */
	uint32_t ticks;
	uint32_t attempts_issued;
	uint32_t proposals;
	uint32_t last_gasps;
};

/* ==========================================================================
 * 5. Lifecycle and events
 * ========================================================================== */

/* Bind the FSM to a node. Does not arm anything. */
void cnxman_recnx_init(struct cnxman_recnx *r, struct vms_cluster *cl,
		       const struct cnxman_ops *ops);

/* Arm / cancel the once-a-second beat (CNXMAN_TIMER_RECNX). Idempotent. */
void cnxman_recnx_start(struct cnxman_recnx *r);
void cnxman_recnx_stop(struct cnxman_recnx *r);

/*
 * The connection to the CM behind `csb` came up (or came back). Membership is
 * untouched -- an SCS connection is connectivity, and membership is a cluster
 * decision (p. 7-28/7-49).
 */
enum cnxman_csb_action cnxman_recnx_connectivity_gained(struct cnxman_recnx *r,
							struct vms_csb *csb);

/*
 * The connection to the CM behind `csb` went away.
 *
 * `announced_departure` is the p. 7-29 distinction and the caller MUST classify
 * it honestly: nonzero only when the peer actually told us it was leaving (the
 * port saw its last-gasp datagram and reports the circuit down for that
 * reason). Nonzero means immediate reconfiguration; zero means the p. 7-30
 * reconnect window, with membership held. Guessing here is how a cluster either
 * reforms on every blip or waits 20 s for a node that is already gone.
 *
 * Returns the action the caller owes the cluster.
 */
enum cnxman_csb_action cnxman_recnx_connectivity_lost(struct cnxman_recnx *r,
						      struct vms_csb *csb,
						      int announced_departure);

/*
 * The once-a-second beat. Walks every CSB in a reconnect state, fires an
 * attempt when one is due and expires the window when the period has run out,
 * re-arms itself, and fills up to `max` records with what the caller must do.
 * Returns the number of records written.
 *
 * Time comes from ops->now_ms and nowhere else, and every comparison is
 * wrap-safe, so a 49.7-day uptime rollover cannot make a deadline look
 * infinitely far away.
 */
uint32_t cnxman_recnx_tick(struct cnxman_recnx *r,
			   struct cnxman_recnx_rec *out, uint32_t max);

/*
 * Leaving the cluster (CNXMAN_EV_SHUTDOWN, and vms_cnxman_stop()).
 *
 * p. 7-49: the departing system sets the SHUTDOWN flag in its CLUB and in its
 * own CSB. p. 7-29: it emits a last-gasp datagram so the survivors remove it
 * without waiting out the reconnect period. Both happen here; the datagram
 * itself is emitted by the port when the caller acts on the returned
 * CNXMAN_CSB_ACT_LAST_GASP record.
 *
 * Emits the record only if this node actually has cluster members to tell --
 * a standalone node announces nothing. After this, ticks propose no more
 * transitions: a node on its way out does not reconfigure the cluster it is
 * leaving. Returns the number of records written (0 or 1).
 */
uint32_t cnxman_recnx_shutdown(struct cnxman_recnx *r,
			       struct cnxman_recnx_rec *out, uint32_t max);

#endif /* OVMX_VMS_CNXMAN_RECNX_FSM_H */
