/* SPDX-License-Identifier: GPL-2.0 */
/* sim_lan.c - the virtual LAN. See sim_lan.h for what each condition models
 * and why it is per DIRECTED link. */

#include <string.h>

#include "sim_lan.h"

/* The per-link stream identity. Distinct for every ordered pair, so adding a
 * node never reshuffles an existing link's dice (sim_rng.h). */
static uint64_t link_stream(uint32_t from, uint32_t to)
{
	return (uint64_t)from * (uint64_t)SIM_MAX_NODES + (uint64_t)to + 1ull;
}

void sim_lan_init(struct sim_lan *lan, uint64_t seed)
{
	uint32_t i, j;

	memset(lan, 0, sizeof(*lan));
	for (i = 0; i < SIM_MAX_NODES; i++) {
		for (j = 0; j < SIM_MAX_NODES; j++)
			sim_rng_seed(&lan->rng[i][j], seed, link_stream(i, j));
	}
}

int sim_lan_add_port(struct sim_lan *lan, const uint8_t mac[6],
		     const uint8_t *mcast)
{
	uint32_t p;

	if (lan->n_ports >= SIM_MAX_NODES)
		return -1;
	p = lan->n_ports++;
	memcpy(lan->mac[p], mac, 6);
	if (mcast != NULL) {
		memcpy(lan->mcast[p], mcast, 6);
		lan->mcast_valid[p] = 1u;
	}
	lan->up[p] = 1u;
	return (int)p;
}

void sim_lan_set_link(struct sim_lan *lan, uint32_t from, uint32_t to,
		      const struct sim_link *cond)
{
	if (from >= SIM_MAX_NODES || to >= SIM_MAX_NODES)
		return;
	lan->link[from][to] = *cond;
}

void sim_lan_set_link_all(struct sim_lan *lan, const struct sim_link *cond)
{
	uint32_t i, j;

	for (i = 0; i < lan->n_ports; i++) {
		for (j = 0; j < lan->n_ports; j++) {
			if (i != j)
				lan->link[i][j] = *cond;
		}
	}
}

void sim_lan_cut(struct sim_lan *lan, uint32_t from, uint32_t to, int cut)
{
	if (from >= SIM_MAX_NODES || to >= SIM_MAX_NODES)
		return;
	lan->link[from][to].cut = cut ? 1u : 0u;
}

void sim_lan_set_up(struct sim_lan *lan, uint32_t port, int up)
{
	if (port < SIM_MAX_NODES)
		lan->up[port] = up ? 1u : 0u;
}

/* ------------------------------------------------------------------ *
 * Addressing: who should see this frame
 * ------------------------------------------------------------------ */

/* IEEE 802: bit 0 of the first octet is the group bit. The cluster HELLO group
 * AB-00-04-01-xx and the broadcast address both have it set. */
static int mac_is_group(const uint8_t mac[6])
{
	return (mac[0] & 0x01u) != 0;
}

static int mac_is_broadcast(const uint8_t mac[6])
{
	int i;

	for (i = 0; i < 6; i++) {
		if (mac[i] != 0xffu)
			return 0;
	}
	return 1;
}

/* Does port `p` accept a frame addressed to `dst`? Unicast: its own hardware
 * address. Group: only a group it JOINED (dev_mc_add), or broadcast. */
static int port_accepts(const struct sim_lan *lan, uint32_t p,
			const uint8_t dst[6])
{
	if (!mac_is_group(dst))
		return memcmp(lan->mac[p], dst, 6) == 0;
	if (mac_is_broadcast(dst))
		return 1;
	return lan->mcast_valid[p] && memcmp(lan->mcast[p], dst, 6) == 0;
}

/* ------------------------------------------------------------------ *
 * Scheduling
 * ------------------------------------------------------------------ */

static struct sim_inflight *inflight_alloc(struct sim_lan *lan)
{
	uint32_t i;

	for (i = 0; i < lan->n_slots; i++) {
		if (!lan->q[i].in_use)
			return &lan->q[i];
	}
	if (lan->n_slots >= SIM_MAX_INFLIGHT) {
		lan->queue_full++;
		return NULL;
	}
	return &lan->q[lan->n_slots++];
}

static void schedule(struct sim_lan *lan, uint32_t from, uint32_t to,
		     const uint8_t *frame, uint32_t len, uint64_t due_ms)
{
	struct sim_inflight *e = inflight_alloc(lan);

	if (e == NULL)
		return;
	e->in_use = 1u;
	e->from = (uint8_t)from;
	e->to = (uint8_t)to;
	e->pad = 0u;
	e->len = len;
	e->due_ms = due_ms;
	e->serial = lan->serial++;
	memcpy(e->b, frame, len);
}

/* Why a copy did not go on the wire, or 0 when it did. Separated from the
 * random draws so the deterministic reasons never consume a draw -- a cut link
 * must not shift the loss pattern of the link beside it. */
static int copy_blocked(struct sim_lan *lan, uint32_t from, uint32_t to)
{
	if (!lan->up[from] || !lan->up[to]) {
		lan->link_down_blocked++;
		return 1;
	}
	if (lan->link[from][to].cut) {
		lan->cut_blocked++;
		return 1;
	}
	return 0;
}

/* One receiver's copy: draw its fate on this link's own stream and schedule
 * what survives. */
static void deliver_copy(struct sim_lan *lan, uint32_t from, uint32_t to,
			 const uint8_t *frame, uint32_t len, uint64_t now_ms)
{
	const struct sim_link *l = &lan->link[from][to];
	struct sim_rng *r = &lan->rng[from][to];
	uint64_t due;

	lan->copies++;
	if (copy_blocked(lan, from, to))
		return;
	if (sim_rng_pct(r, l->loss_pct)) {
		lan->lost++;
		return;
	}

	due = now_ms + (uint64_t)l->latency_ms;
	if (sim_rng_pct(r, l->reorder_pct)) {
		uint32_t extra = l->reorder_extra_ms ? l->reorder_extra_ms
						    : SIM_REORDER_DEFAULT_MS;

		due += (uint64_t)extra;
		lan->reordered++;
	}
	schedule(lan, from, to, frame, len, due);
	lan->delivered++;

	/* A duplicate is a SECOND arrival of the same bytes, right behind the
	 * first: spec §4(h)(4a)'s 506 measured duplicates are peer
	 * retransmissions and near-simultaneous copies, not a fresh message. */
	if (sim_rng_pct(r, l->dup_pct)) {
		schedule(lan, from, to, frame, len, due);
		lan->duped++;
		lan->delivered++;
	}
}

uint32_t sim_lan_xmit(struct sim_lan *lan, uint32_t from, const uint8_t *frame,
		      uint32_t len, uint64_t now_ms)
{
	uint32_t to, before, receivers = 0u;

	if (from >= lan->n_ports || len < 14u || len > SIM_FRAME_MAX)
		return 0u;
	lan->tx_frames++;
	before = sim_lan_inflight(lan);

	/* Ascending receiver index: the draw order is part of the run's
	 * determinism, so it is fixed here and nowhere else. */
	for (to = 0; to < lan->n_ports; to++) {
		if (to == from || !port_accepts(lan, to, frame))
			continue;
		receivers++;
		deliver_copy(lan, from, to, frame, len, now_ms);
	}
	if (receivers == 0u)
		lan->undeliverable++;
	return sim_lan_inflight(lan) - before;
}

/* ------------------------------------------------------------------ *
 * Draining
 * ------------------------------------------------------------------ */

static int inflight_before(const struct sim_inflight *a,
			   const struct sim_inflight *b)
{
	if (a->due_ms != b->due_ms)
		return a->due_ms < b->due_ms;
	return a->serial < b->serial;   /* FIFO within one instant */
}

int sim_lan_next(const struct sim_lan *lan, uint32_t *slot)
{
	uint32_t i, best = 0u;
	int found = 0;

	for (i = 0; i < lan->n_slots; i++) {
		if (!lan->q[i].in_use)
			continue;
		if (!found || inflight_before(&lan->q[i], &lan->q[best])) {
			best = i;
			found = 1;
		}
	}
	if (found)
		*slot = best;
	return found;
}

const struct sim_inflight *sim_lan_peek(const struct sim_lan *lan,
					uint32_t slot)
{
	if (slot >= lan->n_slots || !lan->q[slot].in_use)
		return NULL;
	return &lan->q[slot];
}

void sim_lan_take(struct sim_lan *lan, uint32_t slot)
{
	if (slot < lan->n_slots)
		lan->q[slot].in_use = 0u;
}

uint32_t sim_lan_inflight(const struct sim_lan *lan)
{
	uint32_t i, n = 0u;

	for (i = 0; i < lan->n_slots; i++) {
		if (lan->q[i].in_use)
			n++;
	}
	return n;
}
