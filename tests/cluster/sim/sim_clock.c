/* SPDX-License-Identifier: GPL-2.0 */
/* sim_clock.c - the virtual clock and the timer wheel. See sim_clock.h. */

#include <string.h>

#include "sim_clock.h"

void sim_clock_init(struct sim_clock *c, uint64_t vms_origin)
{
	memset(c, 0, sizeof(*c));
	c->vms_origin = vms_origin;
}

uint32_t sim_clock_now_ms(const struct sim_clock *c)
{
	return (uint32_t)(c->now_ms & 0xffffffffull);
}

uint64_t sim_clock_now_vms(const struct sim_clock *c)
{
	/* 100 ns units: one millisecond is 10 000 of them. */
	return c->vms_origin + c->now_ms * 10000ull;
}

/* ------------------------------------------------------------------ *
 * The wheel
 * ------------------------------------------------------------------ */

static int timer_is(const struct sim_timer *t, uint8_t node, uint8_t which,
		    uint32_t key)
{
	return t->in_use && t->node == node && t->which == which &&
	       t->key == key;
}

static struct sim_timer *timer_find(struct sim_clock *c, uint8_t node,
				    uint8_t which, uint32_t key)
{
	uint32_t i;

	for (i = 0; i < c->n_slots; i++) {
		if (timer_is(&c->t[i], node, which, key))
			return &c->t[i];
	}
	return NULL;
}

static struct sim_timer *timer_alloc(struct sim_clock *c)
{
	uint32_t i;

	for (i = 0; i < c->n_slots; i++) {
		if (!c->t[i].in_use)
			return &c->t[i];
	}
	if (c->n_slots >= SIM_MAX_TIMERS) {
		c->overflows++;
		return NULL;
	}
	return &c->t[c->n_slots++];
}

void sim_clock_arm(struct sim_clock *c, uint8_t node, uint8_t which,
		   uint32_t key, uint32_t ms)
{
	struct sim_timer *t = timer_find(c, node, which, key);

	if (t != NULL) {
		/* The pe_ops contract: re-arming MOVES the deadline. */
		t->due_ms = c->now_ms + (uint64_t)ms;
		t->serial = c->serial++;
		c->moves++;
		return;
	}
	t = timer_alloc(c);
	if (t == NULL)
		return;
	t->in_use = 1u;
	t->node = node;
	t->which = which;
	t->pad = 0u;
	t->key = key;
	t->due_ms = c->now_ms + (uint64_t)ms;
	t->serial = c->serial++;
	c->arms++;
}

void sim_clock_cancel(struct sim_clock *c, uint8_t node, uint8_t which,
		      uint32_t key)
{
	struct sim_timer *t = timer_find(c, node, which, key);

	if (t == NULL)
		return;
	t->in_use = 0u;
	c->cancels++;
}

/* The total order two runs on one seed must agree on. Returns 1 when `a`
 * fires before `b`. */
static int timer_before(const struct sim_timer *a, const struct sim_timer *b)
{
	if (a->due_ms != b->due_ms)
		return a->due_ms < b->due_ms;
	if (a->node != b->node)
		return a->node < b->node;
	if (a->which != b->which)
		return a->which < b->which;
	if (a->key != b->key)
		return a->key < b->key;
	return a->serial < b->serial;
}

int sim_clock_next(const struct sim_clock *c, uint32_t *slot)
{
	uint32_t i, best = 0u;
	int found = 0;

	for (i = 0; i < c->n_slots; i++) {
		if (!c->t[i].in_use)
			continue;
		if (!found || timer_before(&c->t[i], &c->t[best])) {
			best = i;
			found = 1;
		}
	}
	if (found)
		*slot = best;
	return found;
}

int sim_clock_fire(struct sim_clock *c, uint32_t slot, struct sim_timer *out)
{
	if (slot >= c->n_slots || !c->t[slot].in_use)
		return 0;
	*out = c->t[slot];
	c->t[slot].in_use = 0u;
	c->fires++;
	return 1;
}

uint32_t sim_clock_armed(const struct sim_clock *c)
{
	uint32_t i, n = 0u;

	for (i = 0; i < c->n_slots; i++) {
		if (c->t[i].in_use)
			n++;
	}
	return n;
}
