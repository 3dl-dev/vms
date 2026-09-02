/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_node.h - ONE simulated node: a REAL `struct pe_fsm` -- the same pure
 * translation unit vms.ko and the NetBSD kmod compile -- with its `pe_ops`
 * bound to the virtual LAN and the virtual clock (FC-P1.4).
 *
 * THE SIMULATOR DOES NOT REIMPLEMENT THE PROTOCOL. This is the whole point of
 * rung 2 and it is worth stating flatly: there is no state machine in
 * tests/cluster/sim/. A simulated node is src/kernel-core/vms_pe_fsm.c with
 * different ops injected, so a scenario's "expected" is asserted against what
 * the SHIPPING FSM did, never against a model of it. If this file ever grew a
 * `switch (state)` over cluster events, the harness would be testing itself.
 *
 * WHAT THE HARNESS OWNS AND WHAT THE FSM OWNS
 *   harness  the wire (sim_lan), time (sim_clock), the timer wheel, this
 *            node's CONFIGURATION, and a recorder standing in for SCS.
 *   FSM      channels, circuits, sequence numbers, credits, retransmission,
 *            every frame's bytes, and every decision.
 *
 * IDENTITY IS CONFIGURATION, AND IT IS HONEST. Every field of `pe_identity`
 * this file fills comes from the scenario's `sim_node_cfg` -- the SCSNODE, the
 * SCSSYSTEMID, the hardware address, SYSGEN CLUSTER_CREDITS, TIMVCFAIL -- or
 * from the virtual clock (the incarnation quadword is the instant this node
 * BOOTED on that clock, sampled once at boot exactly as the executive samples
 * its real boot time). Nothing here is copied out of a capture: no "VMS V7.3",
 * no captured incarnation, no harvested cluster nonce. A simulated OVMX node
 * broadcasts a simulated OVMX node's identity (the honest-identity ruling), and
 * a scenario that wants a different one says so in its own config.
 */
#ifndef OVMX_SIM_NODE_H
#define OVMX_SIM_NODE_H

#include <stdint.h>

#include "vms_pe.h"
#include "vms_pe_fsm.h"

#include "sim_lan.h"

struct sim;   /* the engine; a node reaches the LAN and the clock through it */

/* ==========================================================================
 * 1. What a scenario declares about a node
 *
 * Everything a `pe_identity` needs that is not derivable, and nothing else.
 * A zero field selects the FSM's own documented default (which is itself
 * either a measured wire figure or an OVMX choice labelled as one), so a
 * scenario names only what it is actually varying.
 * ========================================================================== */
struct sim_node_cfg {
	const char *name;            /* SCSNODE, <= 6 chars, space-padded  */
	uint16_t    sysid;           /* SCSSYSTEMID; the LAVC address is
				      * BUILT from it by the FSM           */
	uint8_t     hw_mac[6];       /* the port's REAL hardware address   */
	uint8_t     mcast[6];        /* the cluster HELLO group it joins   */
	uint8_t     credits;         /* SYSGEN CLUSTER_CREDITS this node
				      * GRANTS the peer (0 is legitimate)  */
	uint8_t     credits_valid;   /* 0 = not loaded, and then no grant  */
	uint16_t    max_sca_len;     /* NISCS_MAX_PKTSZ+2 after the MTU
				      * clamp; 0 = no size verification    */
	uint32_t    hello_interval_ms;
	uint32_t    listen_timeout_ms;
	uint32_t    timvcfail_ms;
	uint32_t    vc_retransmit_ms;
	const char *sw_version;      /* 8 ASCII, this node's OWN version   */
	const char *hw_type;         /* 4 ASCII, its hardware class        */
};

/* ==========================================================================
 * 2. The layer above -- a recorder, not a SYSAP
 *
 * SCS is FC-P2.2. Until it exists the simulator binds a counter to
 * `pe_upper_ops` so "the circuit told the layer above" is an ASSERTION and not
 * an assumption. It deliberately does nothing else: vms_pe_fsm.h §3b(a) makes
 * acknowledgement a transport fact that must not depend on anybody being home,
 * and a scenario can prove that by not binding this at all.
 * ========================================================================== */
struct sim_upper {
	uint32_t    messages, datagrams, ups, downs;
	vms_conid_t last_conid;
	uint32_t    last_len;
	uint32_t    last_down_reason;
	uint64_t    bytes;
};

/* ==========================================================================
 * 3. The node
 * ========================================================================== */
struct sim_node {
	struct sim_node_cfg cfg;
	struct sim         *sim;        /* back-pointer, for the ops thunks   */
	uint8_t             index;      /* == its LAN port index              */
	uint8_t             booted;
	uint8_t             pad[2];

	struct pe_fsm       fsm;        /* THE SHIPPING FSM                   */
	struct pe_ops       ops;
	struct pe_vc        vcs[SIM_MAX_NODES];  /* the glue's circuit table  */
	struct pe_upper_ops upper_ops;
	struct sim_upper    upper;

	uint64_t boot_ms;               /* when it booted, on the virtual clock */
	uint64_t incarnation;           /* the quadword it boots with          */

	/* Harness-side counts of what the FSM asked the harness to do. */
	uint32_t tx_calls;              /* ops->send invocations               */
	uint32_t tx_bytes;
	uint32_t logs;
	char     last_log[160];

	/* The stand-in upper layer's send accounting (sim_msg.c). */
	uint32_t msgs_offered, msgs_accepted, msgs_refused;
	int32_t  last_send_status;      /* enum pe_vc_send_status              */
};

/* Fill `cfg` with the harness defaults for a simulated OVMX node: the group
 * AB-00-04-01-01-01, a locally-administered hardware address derived from
 * `index`, CLUSTER_CREDITS 10, and this node's own honest software version.
 * A scenario overrides any field afterwards. */
void sim_node_cfg_default(struct sim_node_cfg *cfg, const char *name,
			  uint16_t sysid, uint8_t index);

/*
 * Apply a scenario's overrides on top of `base`. A field left ZERO in `ov`
 * keeps the default -- the same convention `pe_identity` itself documents
 * ("0 selects the documented default; a SYSGEN value always wins"), so a
 * scenario names only what it is varying and cannot accidentally blank the
 * node's hardware address by mentioning its TIMVCFAIL.
 *
 * The one field where zero is a legitimate CONFIGURED value, SYSGEN
 * CLUSTER_CREDITS, carries its own `_valid` flag: a scenario that really means
 * a zero grant writes `.credits = 0, .credits_valid = 1`.
 */
void sim_node_cfg_overlay(struct sim_node_cfg *base,
			  const struct sim_node_cfg *ov);

/* Bind a node to the engine and its LAN port. Does NOT boot it. */
void sim_node_attach(struct sim_node *n, struct sim *s, uint8_t index,
		     const struct sim_node_cfg *cfg);

/*
 * Boot the node: build its `pe_identity` from the config and the clock, init
 * the FSM, bind the circuit table and the recorder, and start the HELLO
 * cadence. Returns 0, or -1 when the config is one the FSM refuses (an
 * SCSSYSTEMID that does not fit the two bytes the wire grounds), which is a
 * scenario error and is reported as one.
 */
int  sim_node_boot(struct sim_node *n);

/* Announce the departure the way p. 7-29 does and stop: the §4(O.30) last
 * gasp, then shutdown. */
void sim_node_halt(struct sim_node *n);

/* The port's NIC comes up / goes down. Both halves happen: the FSM is told
 * (so it drops circuits and channels) AND the LAN stops carrying its frames. */
void sim_node_set_link(struct sim_node *n, int up);

/* The circuit to a peer SCSSYSTEMID, or NULL. A thin, named wrapper over the
 * FSM's own lookup so scenario code never reaches into `struct pe_fsm`. */
struct pe_vc      *sim_node_vc_to(struct sim_node *n, uint16_t peer_sysid);
struct pe_channel *sim_node_channel_to(struct sim_node *n,
				       const uint8_t peer_mac[6]);

#endif /* OVMX_SIM_NODE_H */
