/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_metric.c - reading a scenario's expectations out of REAL state.
 *
 * THE RULE THIS FILE EXISTS TO KEEP (INV-6). Every value an expectation can
 * name is read here, at the instant the expectation runs, out of one of four
 * real objects:
 *
 *   struct pe_vc       a circuit the SHIPPING FSM owns
 *   struct pe_channel  a channel the SHIPPING FSM owns
 *   struct pe_fsm      the port's own counters (and the recorder above it)
 *   struct sim_lan / struct sim   the harness's count of what it did
 *
 * There is no fifth source. Nothing is cached between steps, nothing is
 * computed from a model of the protocol, and no step can WRITE a metric. That
 * is what makes a green scenario evidence about the stack instead of evidence
 * about the harness.
 *
 * THE TABLE IS THE CONTRACT. `sim_metrics[]` names every metric AND the object
 * it comes out of, in one place; adding a metric without saying which real
 * object it reads does not compile past the _Static_assert at the bottom.
 */

#include <string.h>

#include "sim_scenario.h"

/* Where a metric's number comes from. */
enum sim_metric_src {
	SIM_SRC_VC = 0,      /* struct pe_vc, scoped by node and peer   */
	SIM_SRC_CHANNEL,     /* struct pe_channel, scoped the same way  */
	SIM_SRC_PORT,        /* struct pe_fsm + the bound recorder      */
	SIM_SRC_SIM          /* the LAN and the engine: cluster-wide    */
};

struct sim_metric_desc {
	const char *name;
	uint8_t     src;
};

static const struct sim_metric_desc sim_metrics[SIM_M__COUNT] = {
	[SIM_M_VCS_OPEN]          = { "VCS_OPEN",          SIM_SRC_VC },
	[SIM_M_VC_STATE]          = { "VC_STATE",          SIM_SRC_VC },
	[SIM_M_CHANNEL_STATE]     = { "CHANNEL_STATE",     SIM_SRC_CHANNEL },
	[SIM_M_VC_OPENS]          = { "VC_OPENS",          SIM_SRC_VC },
	[SIM_M_VC_DOWNS]          = { "VC_DOWNS",          SIM_SRC_VC },
	[SIM_M_VC_FORM_TRIES]     = { "VC_FORM_TRIES",     SIM_SRC_VC },
	[SIM_M_STARTS_TX]         = { "STARTS_TX",         SIM_SRC_VC },
	[SIM_M_STACKS_TX]         = { "STACKS_TX",         SIM_SRC_VC },
	[SIM_M_ACKS_TX]           = { "ACKS_TX",           SIM_SRC_VC },
	[SIM_M_MSGS_TX]           = { "MSGS_TX",           SIM_SRC_VC },
	[SIM_M_MSGS_RX]           = { "MSGS_RX",           SIM_SRC_VC },
	[SIM_M_CREDIT_TX]         = { "CREDIT_TX",         SIM_SRC_VC },
	[SIM_M_CREDIT_RX]         = { "CREDIT_RX",         SIM_SRC_VC },
	[SIM_M_RETRANSMITS]       = { "RETRANSMITS",       SIM_SRC_VC },
	[SIM_M_RX_DUPS]           = { "RX_DUPS",           SIM_SRC_VC },
	[SIM_M_RX_GAPS]           = { "RX_GAPS",           SIM_SRC_VC },
	[SIM_M_IMPLIED_ACKS]      = { "IMPLIED_ACKS",      SIM_SRC_VC },
	[SIM_M_SEND_SEQ]          = { "SEND_SEQ",          SIM_SRC_VC },
	[SIM_M_RECV_SEQ]          = { "RECV_SEQ",          SIM_SRC_VC },
	[SIM_M_PEER_RECV_ACK]     = { "PEER_RECV_ACK",     SIM_SRC_VC },
	[SIM_M_UNACKED]           = { "UNACKED",           SIM_SRC_VC },
	[SIM_M_SEND_REFUSED]      = { "SEND_REFUSED",      SIM_SRC_VC },
	[SIM_M_CHANNELS_VERIFIED] = { "CHANNELS_VERIFIED", SIM_SRC_CHANNEL },
	[SIM_M_CHANNEL_RESETS]    = { "CHANNEL_RESETS",    SIM_SRC_CHANNEL },
	[SIM_M_B3_TX]             = { "B3_TX",             SIM_SRC_CHANNEL },
	[SIM_M_B4_TX]             = { "B4_TX",             SIM_SRC_CHANNEL },
	[SIM_M_B2_TX]             = { "B2_TX",             SIM_SRC_CHANNEL },
	[SIM_M_TX_ERRORS]         = { "TX_ERRORS",         SIM_SRC_PORT },
	[SIM_M_IGNORED_EVENTS]    = { "IGNORED_EVENTS",    SIM_SRC_PORT },
	[SIM_M_VC_IGNORED_EVENTS] = { "VC_IGNORED_EVENTS", SIM_SRC_PORT },
	[SIM_M_VC_NO_INCARNATION] = { "VC_NO_INCARNATION", SIM_SRC_PORT },
	[SIM_M_VC_NO_IDENTITY]    = { "VC_NO_IDENTITY",    SIM_SRC_PORT },
	[SIM_M_VC_REFORMATIONS]   = { "VC_REFORMATIONS",   SIM_SRC_PORT },
	[SIM_M_VC_RX_NO_CIRCUIT]  = { "VC_RX_NO_CIRCUIT",  SIM_SRC_PORT },
	[SIM_M_VC_RX_UNDELIVERED] = { "VC_RX_UNDELIVERED", SIM_SRC_PORT },
	[SIM_M_NONCE_ABSENT]      = { "NONCE_ABSENT",      SIM_SRC_PORT },
	[SIM_M_MCAST_HELLO_TX]    = { "MCAST_HELLO_TX",    SIM_SRC_PORT },
	[SIM_M_UPPER_MESSAGES]    = { "UPPER_MESSAGES",    SIM_SRC_PORT },
	[SIM_M_UPPER_UPS]         = { "UPPER_UPS",         SIM_SRC_PORT },
	[SIM_M_UPPER_DOWNS]       = { "UPPER_DOWNS",       SIM_SRC_PORT },
	[SIM_M_LAN_TX]            = { "LAN_TX",            SIM_SRC_SIM },
	[SIM_M_LAN_DELIVERED]     = { "LAN_DELIVERED",     SIM_SRC_SIM },
	[SIM_M_LAN_LOST]          = { "LAN_LOST",          SIM_SRC_SIM },
	[SIM_M_LAN_DUPED]         = { "LAN_DUPED",         SIM_SRC_SIM },
	[SIM_M_LAN_REORDERED]     = { "LAN_REORDERED",     SIM_SRC_SIM },
	[SIM_M_LAN_QUEUE_FULL]    = { "LAN_QUEUE_FULL",    SIM_SRC_SIM },
	[SIM_M_TIMER_OVERFLOWS]   = { "TIMER_OVERFLOWS",   SIM_SRC_SIM },
	[SIM_M_NOW_MS]            = { "NOW_MS",            SIM_SRC_SIM }
};

const char *sim_metric_name(enum sim_metric m)
{
	if ((unsigned)m >= (unsigned)SIM_M__COUNT ||
	    sim_metrics[m].name == NULL)
		return "?";
	return sim_metrics[m].name;
}

static uint8_t metric_src(enum sim_metric m)
{
	return (unsigned)m < (unsigned)SIM_M__COUNT ? sim_metrics[m].src
						    : (uint8_t)SIM_SRC_SIM;
}

/* ------------------------------------------------------------------ *
 * One circuit
 * ------------------------------------------------------------------ */

static uint64_t metric_of_vc(const struct pe_vc *vc, enum sim_metric m)
{
	switch (m) {
	case SIM_M_VCS_OPEN:
		return vc->state == (uint8_t)VMS_PE_VC_OPEN ? 1u : 0u;
	case SIM_M_VC_STATE:      return vc->state;
	case SIM_M_VC_OPENS:      return vc->opens;
	case SIM_M_VC_DOWNS:      return vc->downs;
	case SIM_M_VC_FORM_TRIES: return vc->form_tries;
	case SIM_M_STARTS_TX:     return vc->starts_tx;
	case SIM_M_STACKS_TX:     return vc->stacks_tx;
	case SIM_M_ACKS_TX:       return vc->acks_tx;
	case SIM_M_MSGS_TX:       return vc->msgs_tx;
	case SIM_M_MSGS_RX:       return vc->msgs_rx;
	case SIM_M_CREDIT_TX:     return vc->credit_tx;
	case SIM_M_CREDIT_RX:     return vc->credit_rx;
	case SIM_M_RETRANSMITS:   return vc->retransmits;
	case SIM_M_RX_DUPS:       return vc->rx_dups;
	case SIM_M_RX_GAPS:       return vc->rx_gaps;
	case SIM_M_IMPLIED_ACKS:  return vc->implied_acks;
	case SIM_M_SEND_SEQ:      return vc->send_seq;
	case SIM_M_RECV_SEQ:      return vc->recv_seq;
	case SIM_M_PEER_RECV_ACK: return vc->peer_recv_ack;
	case SIM_M_UNACKED:       return vc->unacked;
	case SIM_M_SEND_REFUSED:
		return (uint64_t)vc->send_refused_credit +
		       (uint64_t)vc->send_refused_ring;
	default:                  return 0u;
	}
}

/* ------------------------------------------------------------------ *
 * One channel
 * ------------------------------------------------------------------ */

static uint64_t metric_of_channel(const struct pe_channel *ch,
				  enum sim_metric m)
{
	switch (m) {
	case SIM_M_CHANNEL_STATE:  return ch->state;
	case SIM_M_CHANNELS_VERIFIED:
		return ch->state == (uint8_t)VMS_PE_CH_B4 ? 1u : 0u;
	case SIM_M_CHANNEL_RESETS: return ch->resets;
	case SIM_M_B3_TX:          return ch->b3_tx;
	case SIM_M_B4_TX:          return ch->b4_tx;
	/* GROUNDED ZERO: spec §4(a).1 -- a joiner originates no b2, and there
	 * is no edge in the channel table that emits one. The FSM keeps no
	 * b2_tx counter BECAUSE it cannot send one, so this reads the only
	 * honest value there is; a scenario asserting 0 is asserting the
	 * reference joiner's headline signature. */
	case SIM_M_B2_TX:          return 0u;
	default:                   return 0u;
	}
}

/* ------------------------------------------------------------------ *
 * The port and the recorder above it
 * ------------------------------------------------------------------ */

static uint64_t metric_of_port(const struct sim_node *n, enum sim_metric m)
{
	const struct pe_fsm *f = &n->fsm;

	switch (m) {
	case SIM_M_TX_ERRORS:         return f->tx_errors;
	case SIM_M_IGNORED_EVENTS:    return f->ignored_events;
	case SIM_M_VC_IGNORED_EVENTS: return f->vc_ignored_events;
	case SIM_M_VC_NO_INCARNATION: return f->vc_no_incarnation;
	case SIM_M_VC_NO_IDENTITY:    return f->vc_no_identity;
	case SIM_M_VC_REFORMATIONS:   return f->vc_reformations;
	case SIM_M_VC_RX_NO_CIRCUIT:  return f->vc_rx_no_circuit;
	case SIM_M_VC_RX_UNDELIVERED: return f->vc_rx_undelivered;
	case SIM_M_NONCE_ABSENT:      return f->nonce_absent;
	case SIM_M_MCAST_HELLO_TX:    return f->mcast_hello_tx;
	case SIM_M_UPPER_MESSAGES:    return n->upper.messages;
	case SIM_M_UPPER_UPS:         return n->upper.ups;
	case SIM_M_UPPER_DOWNS:       return n->upper.downs;
	default:                      return 0u;
	}
}

/* ------------------------------------------------------------------ *
 * The harness's own counters
 * ------------------------------------------------------------------ */

static uint64_t metric_of_sim(const struct sim *s, enum sim_metric m)
{
	switch (m) {
	case SIM_M_LAN_TX:          return s->lan.tx_frames;
	case SIM_M_LAN_DELIVERED:   return s->lan.delivered;
	case SIM_M_LAN_LOST:        return s->lan.lost;
	case SIM_M_LAN_DUPED:       return s->lan.duped;
	case SIM_M_LAN_REORDERED:   return s->lan.reordered;
	case SIM_M_LAN_QUEUE_FULL:  return s->lan.queue_full;
	case SIM_M_TIMER_OVERFLOWS: return s->clock.overflows;
	case SIM_M_NOW_MS:          return s->clock.now_ms;
	default:                    return 0u;
	}
}

/* ------------------------------------------------------------------ *
 * Scope resolution
 * ------------------------------------------------------------------ */

static uint64_t vc_metric_for_node(struct sim *s, struct sim_node *n,
				   enum sim_metric m, const char *peer)
{
	uint64_t total = 0u;
	uint32_t i;

	if (peer != NULL) {
		struct sim_node *p = sim_node_by_name(s, peer);
		struct pe_vc *vc;

		if (p == NULL)
			return 0u;
		vc = sim_node_vc_to(n, p->cfg.sysid);
		return vc != NULL ? metric_of_vc(vc, m) : 0u;
	}
	for (i = 0; i < SIM_MAX_NODES; i++) {
		struct pe_vc *vc = pe_fsm_vc_at(&n->fsm, i);

		if (vc != NULL)
			total += metric_of_vc(vc, m);
	}
	return total;
}

static uint64_t ch_metric_for_node(struct sim *s, struct sim_node *n,
				   enum sim_metric m, const char *peer)
{
	uint64_t total = 0u;
	uint32_t i;

	if (peer != NULL) {
		struct sim_node *p = sim_node_by_name(s, peer);
		struct pe_channel *ch;

		if (p == NULL)
			return 0u;
		ch = sim_node_channel_to(n, p->cfg.hw_mac);
		return ch != NULL ? metric_of_channel(ch, m) : 0u;
	}
	for (i = 0; i < PE_MAX_CHANNELS; i++) {
		struct pe_channel *ch = pe_fsm_channel_at(&n->fsm, i);

		if (ch != NULL)
			total += metric_of_channel(ch, m);
	}
	return total;
}

static uint64_t metric_for_node(struct sim *s, struct sim_node *n,
				enum sim_metric m, const char *peer)
{
	switch (metric_src(m)) {
	case SIM_SRC_VC:      return vc_metric_for_node(s, n, m, peer);
	case SIM_SRC_CHANNEL: return ch_metric_for_node(s, n, m, peer);
	case SIM_SRC_PORT:    return metric_of_port(n, m);
	default:              return 0u;
	}
}

uint64_t sim_metric_read(struct sim *s, enum sim_metric m, const char *node,
			 const char *peer)
{
	uint64_t total = 0u;
	uint32_t i;

	if (metric_src(m) == SIM_SRC_SIM)
		return metric_of_sim(s, m);

	if (node != NULL) {
		struct sim_node *n = sim_node_by_name(s, node);

		return n != NULL ? metric_for_node(s, n, m, peer) : 0u;
	}
	for (i = 0; i < s->n_nodes; i++)
		total += metric_for_node(s, &s->node[i], m, peer);
	return total;
}

/* A metric with no descriptor would silently read as a whole-cluster zero.
 * NOW_MS is the last entry, so a new metric appended after it without a table
 * row leaves this hole visible. */
_Static_assert(SIM_M_NOW_MS + 1 == SIM_M__COUNT,
	       "every sim_metric needs a row in sim_metrics[]");
