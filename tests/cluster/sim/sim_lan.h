/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_lan.h - the VIRTUAL LAN the simulated ports transmit onto: per-DIRECTED-
 * LINK loss, duplication, reordering and latency, plus multicast-group
 * membership and a hard cut (FC-P1.4).
 *
 * WHAT IT MODELS, AND WHY EACH PIECE IS THERE. Design §3.9's rung 2 asks for
 * "a virtual LAN with configurable loss/reorder/duplication/latency". Every one
 * of those four is a failure the campaign actually met on real wire:
 *
 *   LOSS        the CN=3 campaign's join stalls were retransmit ladders; a
 *               lossless simulator proves nothing about the code that exists
 *               only to survive loss (spec §4(k)'s member retransmitting a
 *               padded HELLO 24 times is exactly this).
 *   DUPLICATION spec §4(h)(4a) counted 506 duplicate sequenced messages in 47
 *               captures. A duplicate is NOT a gap and must never break a
 *               circuit; the only way to test that is to produce one.
 *   REORDERING  p. 2-31 breaks a virtual circuit on a sequence GAP. The
 *               difference between "a gap" and "a frame that arrives late" is
 *               where a port either works or destroys a cluster.
 *   LATENCY     it is what makes reordering possible at all, and what makes a
 *               formation race (both ends START at once, p. 2-14) reachable.
 *
 * PER DIRECTED LINK, NOT PER PAIR. A->B and B->A are separate `struct
 * sim_link`s with separate conditions and separate random streams. Half-duplex
 * failures are the interesting ones: a node that can hear but not be heard
 * looks alive to itself and dead to everyone else, and it is what a one-way
 * multicast filter or a wedged transmit ring actually does.
 *
 * MULTICAST IS GROUP MEMBERSHIP, NOT BROADCAST. A frame to a group address is
 * delivered only to the ports that JOINED that group (the model of
 * exec_lan_mcast_add / dev_mc_add), never to the sender, and each receiver's
 * copy is drawn against ITS OWN link -- so a multicast HELLO can be lost to one
 * peer and delivered to another, which is what a real LAN does and what a
 * naive "deliver to all or none" model would hide.
 *
 * A CUT LINK IS NOT 100 % LOSS. `cut` is a partition: nothing crosses, and it
 * is counted separately, so a scenario can tell "the network was severed" from
 * "the network was very lossy". A node whose PORT is down (`sim_lan_set_up`)
 * neither transmits nor receives -- that is the NIC being down, and it is the
 * LAN's business, not the FSM's: pe_fsm_link_down records the fact for the
 * layers above but does not gate transmission, so the harness has to.
 *
 * HOST-ONLY. Never compiled into vms.ko or the NetBSD kmod.
 */
#ifndef OVMX_SIM_LAN_H
#define OVMX_SIM_LAN_H

#include <stdint.h>

#include "sim_rng.h"

/* 2-8 simulated nodes is the range design §3.9 rung 2 names. */
#define SIM_MAX_NODES 8

/* The largest frame the wire carries: a spec §4(k) size-verify padded HELLO. */
#define SIM_FRAME_MAX 1514u

/*
 * Frames in flight at once. With the default zero latency this stays in single
 * digits; the headroom is for a scenario that gives a link a long delay. An
 * overflow is COUNTED and asserted zero -- a silently dropped frame would be
 * an invisible extra loss on top of the configured one.
 */
#define SIM_MAX_INFLIGHT 256

/* One DIRECTED link's conditions. All zero (the default) is a perfect wire. */
struct sim_link {
	uint32_t loss_pct;          /* frames dropped outright                */
	uint32_t dup_pct;           /* frames delivered TWICE                 */
	uint32_t reorder_pct;       /* frames held back by reorder_extra_ms   */
	uint32_t latency_ms;        /* base one-way delay                     */
	uint32_t reorder_extra_ms;  /* the hold-back, 0 selects the default   */
	uint8_t  cut;               /* a partition: nothing crosses at all    */
	uint8_t  pad[3];
};

/* The hold-back a reordered frame takes when a link does not name its own.
 * Long enough to be overtaken by the next frame on a 0 ms link, short enough
 * to stay well inside every protocol deadline in the stack. */
#define SIM_REORDER_DEFAULT_MS 5u

struct sim_inflight {
	uint8_t  in_use;
	uint8_t  from;
	uint8_t  to;
	uint8_t  pad;
	uint32_t len;
	uint64_t due_ms;
	uint64_t serial;
	uint8_t  b[SIM_FRAME_MAX];
};

struct sim_lan {
	uint32_t n_ports;
	uint8_t  mac[SIM_MAX_NODES][6];     /* each port's REAL hardware address */
	uint8_t  mcast[SIM_MAX_NODES][6];   /* the group it joined */
	uint8_t  mcast_valid[SIM_MAX_NODES];
	uint8_t  up[SIM_MAX_NODES];         /* the port's link state */

	struct sim_link link[SIM_MAX_NODES][SIM_MAX_NODES];
	struct sim_rng  rng[SIM_MAX_NODES][SIM_MAX_NODES];

	uint64_t serial;
	uint32_t n_slots;                   /* high-water of the in-flight table */
	struct sim_inflight q[SIM_MAX_INFLIGHT];

	/* Every one of these is a real count of a real decision. */
	uint64_t tx_frames;         /* transmit calls from a port            */
	uint64_t copies;            /* per-receiver copies considered        */
	uint64_t delivered;         /* copies put on the wire (they arrive
				     * unless the run ends before their due) */
	uint64_t lost;              /* dropped by the loss condition         */
	uint64_t duped;             /* extra copies the dup condition made   */
	uint64_t reordered;         /* copies held back                      */
	uint64_t cut_blocked;       /* copies a partition stopped            */
	uint64_t link_down_blocked; /* copies a down port stopped            */
	uint64_t undeliverable;     /* no port owns that address             */
	uint64_t queue_full;        /* in-flight overflow: asserted zero     */
};

void sim_lan_init(struct sim_lan *lan, uint64_t seed);

/*
 * Attach one port. `mcast` may be NULL for a port that joined no group (it
 * then receives unicast only). Returns the port index, or -1 when full.
 */
int  sim_lan_add_port(struct sim_lan *lan, const uint8_t mac[6],
		      const uint8_t *mcast);

/* Link conditions. `sim_lan_set_link_all` sets EVERY directed link, which is
 * what a scenario that says "10 % loss everywhere" means. */
void sim_lan_set_link(struct sim_lan *lan, uint32_t from, uint32_t to,
		      const struct sim_link *cond);
void sim_lan_set_link_all(struct sim_lan *lan, const struct sim_link *cond);

/* Cut / heal one DIRECTION. A scenario that wants a symmetric partition cuts
 * both, which is deliberate: asking for it twice is cheaper than debugging a
 * one-way partition somebody did not mean to create. */
void sim_lan_cut(struct sim_lan *lan, uint32_t from, uint32_t to, int cut);

/* The port's own link state (its NIC). */
void sim_lan_set_up(struct sim_lan *lan, uint32_t port, int up);

/*
 * Transmit one complete Ethernet frame from `from`. Expands it to the ports
 * that should see it, draws each copy's fate on ITS OWN link stream, and
 * schedules what survives. Returns the number of copies scheduled.
 */
uint32_t sim_lan_xmit(struct sim_lan *lan, uint32_t from, const uint8_t *frame,
		      uint32_t len, uint64_t now_ms);

/* The earliest scheduled delivery, by (due_ms, serial). 1 and `*slot`, or 0. */
int sim_lan_next(const struct sim_lan *lan, uint32_t *slot);

/* Read a scheduled delivery without removing it, and remove it. Split so the
 * engine can record the frame in its trace before the receiver mutates
 * anything. */
const struct sim_inflight *sim_lan_peek(const struct sim_lan *lan,
					uint32_t slot);
void sim_lan_take(struct sim_lan *lan, uint32_t slot);

/* How many copies are in flight (a scenario asserts the wire drained). */
uint32_t sim_lan_inflight(const struct sim_lan *lan);

#endif /* OVMX_SIM_LAN_H */
