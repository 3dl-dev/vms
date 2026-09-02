/* SPDX-License-Identifier: GPL-2.0 */
/* sim_engine.c - the deterministic event loop. See sim.h for the ordering
 * contract and the trace. */

#include <string.h>

#include "sim.h"

/* ------------------------------------------------------------------ *
 * The trace: a rolling FNV-1a-64 over everything that happened
 *
 * Cheap enough to leave on for every scenario, and total enough that two runs
 * agree on it only if they dispatched the same events, at the same virtual
 * instants, carrying the same bytes.
 * ------------------------------------------------------------------ */

#define FNV64_OFFSET 0xcbf29ce484222325ull
#define FNV64_PRIME  0x00000100000001b3ull

void sim_trace_mix(struct sim *s, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++) {
		s->trace ^= (v >> (i * 8)) & 0xffull;
		s->trace *= FNV64_PRIME;
	}
}

void sim_trace_bytes(struct sim *s, const uint8_t *b, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		s->trace ^= b[i];
		s->trace *= FNV64_PRIME;
	}
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

void sim_init(struct sim *s, uint64_t seed)
{
	memset(s, 0, sizeof(*s));
	s->seed = seed;
	s->trace = FNV64_OFFSET;
	sim_clock_init(&s->clock, SIM_VMS_ORIGIN);
	sim_lan_init(&s->lan, seed);
	sim_trace_mix(s, seed);
}

int sim_add_node(struct sim *s, const struct sim_node_cfg *cfg)
{
	int port;

	if (s->n_nodes >= SIM_MAX_NODES)
		return -1;
	port = sim_lan_add_port(&s->lan, cfg->hw_mac, cfg->mcast);
	if (port < 0)
		return -1;
	sim_node_attach(&s->node[port], s, (uint8_t)port, cfg);
	s->n_nodes = (uint32_t)port + 1u;
	return port;
}

int sim_boot_all(struct sim *s)
{
	uint32_t i, n = 0u;

	for (i = 0; i < s->n_nodes; i++) {
		if (s->node[i].booted)
			continue;
		if (sim_node_boot(&s->node[i]) != 0)
			return -1;
		n++;
	}
	return (int)n;
}

struct sim_node *sim_node_by_name(struct sim *s, const char *name)
{
	uint32_t i;

	if (name == NULL)
		return NULL;
	for (i = 0; i < s->n_nodes; i++) {
		if (s->node[i].cfg.name != NULL &&
		    strcmp(s->node[i].cfg.name, name) == 0)
			return &s->node[i];
	}
	return NULL;
}

struct sim_node *sim_node_at(struct sim *s, uint32_t index)
{
	return index < s->n_nodes ? &s->node[index] : NULL;
}

uint64_t sim_now_ms(const struct sim *s)
{
	return s->clock.now_ms;
}

/* ------------------------------------------------------------------ *
 * Dispatching one event
 * ------------------------------------------------------------------ */

/* A frame arrived. It goes into the SHIPPING FSM's receive path, where the
 * CODEC decides what it is -- the harness never classifies a frame itself. */
static void sim_dispatch_delivery(struct sim *s, uint32_t slot)
{
	const struct sim_inflight *e = sim_lan_peek(&s->lan, slot);
	struct sim_node *n;
	uint8_t frame[SIM_FRAME_MAX];
	uint32_t len;
	uint8_t to;

	if (e == NULL)
		return;
	to = e->to;
	len = e->len;
	memcpy(frame, e->b, len);

	sim_trace_mix(s, (uint64_t)SIM_EV_DELIVERY);
	sim_trace_mix(s, s->clock.now_ms);
	sim_trace_mix(s, ((uint64_t)e->from << 8) | (uint64_t)to);
	sim_trace_bytes(s, frame, len);

	sim_lan_take(&s->lan, slot);
	s->deliveries++;

	n = sim_node_at(s, to);
	if (n == NULL || !n->booted)
		return;
	(void)pe_fsm_rx(&n->fsm, frame, len);
}

/*
 * A timer expired. Each pe_timer identity has exactly one entry point in the
 * FSM's public surface; this table is the whole mapping, and it is the same
 * one vms_pe.c's fork-module wrappers make in production.
 */
static void sim_fire_pe_timer(struct sim_node *n, const struct sim_timer *t)
{
	switch ((enum pe_timer)t->which) {
	case PE_TIMER_HELLO:
		/* The port-wide cadence beat: the multicast HELLO, every
		 * channel's tick, every circuit's tick, and its own re-arm. */
		(void)pe_fsm_tick(&n->fsm, NULL, 0u);
		break;
	case PE_TIMER_CHANNEL:
		(void)pe_fsm_channel_timer(&n->fsm, t->key);
		break;
	case PE_TIMER_RETRANSMIT:
		pe_fsm_vc_timer(&n->fsm, t->key);
		break;
	case PE_TIMER_VCFAIL:
		pe_fsm_vc_event(&n->fsm, t->key, PE_EV_TIMER_VCFAIL);
		break;
	default:
		break;
	}
}

static void sim_dispatch_timer(struct sim *s, uint32_t slot)
{
	struct sim_timer t;
	struct sim_node *n;

	if (!sim_clock_fire(&s->clock, slot, &t))
		return;

	sim_trace_mix(s, (uint64_t)SIM_EV_TIMER);
	sim_trace_mix(s, s->clock.now_ms);
	sim_trace_mix(s, ((uint64_t)t.node << 16) | ((uint64_t)t.which << 8) |
			 (uint64_t)(t.key & 0xffu));
	s->timer_fires++;

	n = sim_node_at(s, t.node);
	if (n == NULL || !n->booted)
		return;
	sim_fire_pe_timer(n, &t);
}

/* ------------------------------------------------------------------ *
 * Choosing the next event
 * ------------------------------------------------------------------ */

struct sim_next {
	enum sim_event_kind kind;
	uint32_t            slot;
	uint64_t            due_ms;
};

/*
 * The earliest event of either kind, or SIM_EV_NONE. At the same millisecond a
 * DELIVERY wins: the frame was put on the wire before the timer's deadline
 * arrived, so it arrives first. That choice is arbitrary but it is FIXED, and
 * a fixed total order is what makes the run reproducible.
 */
static struct sim_next sim_peek_next(struct sim *s)
{
	struct sim_next r;
	uint32_t slot;

	r.kind = SIM_EV_NONE;
	r.slot = 0u;
	r.due_ms = 0u;

	if (sim_lan_next(&s->lan, &slot)) {
		const struct sim_inflight *e = sim_lan_peek(&s->lan, slot);

		r.kind = SIM_EV_DELIVERY;
		r.slot = slot;
		r.due_ms = e->due_ms;
	}
	if (sim_clock_next(&s->clock, &slot)) {
		uint64_t due = s->clock.t[slot].due_ms;

		if (r.kind == SIM_EV_NONE || due < r.due_ms) {
			r.kind = SIM_EV_TIMER;
			r.slot = slot;
			r.due_ms = due;
		}
	}
	return r;
}

/* Move the clock to the event and dispatch it. Time never goes backwards: a
 * delivery scheduled in the past (it cannot be, but the guard is free) runs at
 * the current instant. */
static void sim_step(struct sim *s, const struct sim_next *ev)
{
	if (ev->due_ms > s->clock.now_ms)
		s->clock.now_ms = ev->due_ms;
	s->events++;
	if (ev->kind == SIM_EV_DELIVERY)
		sim_dispatch_delivery(s, ev->slot);
	else
		sim_dispatch_timer(s, ev->slot);
}

/* ------------------------------------------------------------------ *
 * Running
 * ------------------------------------------------------------------ */

uint64_t sim_run_ms(struct sim *s, uint32_t ms)
{
	uint64_t horizon = s->clock.now_ms + (uint64_t)ms;
	uint64_t before = s->events;

	for (;;) {
		struct sim_next ev = sim_peek_next(s);

		if (ev.kind == SIM_EV_NONE || ev.due_ms > horizon)
			break;
		if (s->events >= SIM_MAX_EVENTS) {
			s->event_cap_hit = 1u;
			break;
		}
		sim_step(s, &ev);
	}
	/* The window is over even if nothing was scheduled in it: a scenario
	 * that says "run 30 seconds" gets 30 seconds of virtual time. */
	if (s->clock.now_ms < horizon)
		s->clock.now_ms = horizon;
	return s->events - before;
}

int sim_run_until(struct sim *s, sim_predicate_fn pred, void *ctx,
		  uint32_t timeout_ms)
{
	uint64_t horizon = s->clock.now_ms + (uint64_t)timeout_ms;

	if (pred(s, ctx))
		return 1;
	for (;;) {
		struct sim_next ev = sim_peek_next(s);

		if (ev.kind == SIM_EV_NONE || ev.due_ms > horizon)
			break;
		if (s->events >= SIM_MAX_EVENTS) {
			s->event_cap_hit = 1u;
			break;
		}
		sim_step(s, &ev);
		if (pred(s, ctx))
			return 1;
	}
	if (s->clock.now_ms < horizon)
		s->clock.now_ms = horizon;
	return pred(s, ctx);
}

/* ------------------------------------------------------------------ *
 * Reading the cluster back -- every number from real FSM state
 * ------------------------------------------------------------------ */

uint32_t sim_count_vcs_open(struct sim *s)
{
	uint32_t i, k, n = 0u;

	for (i = 0; i < s->n_nodes; i++) {
		for (k = 0; k < SIM_MAX_NODES; k++) {
			struct pe_vc *vc = pe_fsm_vc_at(&s->node[i].fsm, k);

			if (vc != NULL && vc->state == (uint8_t)VMS_PE_VC_OPEN)
				n++;
		}
	}
	return n;
}

uint32_t sim_count_channels_verified(struct sim *s)
{
	uint32_t i, k, n = 0u;

	for (i = 0; i < s->n_nodes; i++) {
		for (k = 0; k < PE_MAX_CHANNELS; k++) {
			struct pe_channel *ch =
				pe_fsm_channel_at(&s->node[i].fsm, k);

			/* NULL is "past the end OR a free slot", so a hole in
			 * the middle is skipped, never treated as the end. */
			if (ch != NULL && ch->state == (uint8_t)VMS_PE_CH_B4)
				n++;
		}
	}
	return n;
}

/* With every node booted and every link usable, each ordered pair owes one
 * circuit: a circuit is one-ended and each end holds its own. */
static uint32_t sim_expected_pairs(const struct sim *s)
{
	return s->n_nodes * (s->n_nodes - 1u);
}

int sim_all_vcs_open(struct sim *s, void *unused)
{
	(void)unused;
	return sim_count_vcs_open(s) >= sim_expected_pairs(s);
}

int sim_all_channels_verified(struct sim *s, void *unused)
{
	(void)unused;
	return sim_count_channels_verified(s) >= sim_expected_pairs(s);
}
