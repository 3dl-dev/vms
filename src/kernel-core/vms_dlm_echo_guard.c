/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_echo_guard.c - the echo guard's decision (rd vms-b5b0).
 *
 * Read vms_dlm_echo_guard.h first: it carries the rule, the measurement that
 * forced it, and why the bound is a sameness count rather than a rate.
 */
#include "vms_dlm_echo_guard.h"

/* FNV-1a over a body. A private "are these the same bytes" signature. */
static uint32_t echo_sig(const uint8_t *b, uint32_t len)
{
	uint32_t h = 2166136261u, i;

	for (i = 0u; i < len; i++) {
		h ^= (uint32_t)b[i];
		h *= 16777619u;
	}
	return h;
}

/* The slot already tracking this system, or NULL. */
static struct vms_dlm_echo_slot *slot_for(struct vms_dlm_echo_guard *g,
					  uint32_t from)
{
	uint32_t i;

	for (i = 0u; i < VMS_DLM_ECHO_SLOTS; i++) {
		if (g->slot[i].used && g->slot[i].from == from)
			return &g->slot[i];
	}
	return (struct vms_dlm_echo_slot *)0;
}

/*
 * A slot to track a newly-seen system in: a free one, else the live slot with
 * the shortest run. Reusing the shortest run is deliberate -- the state worth
 * keeping is a run that is building towards the bound.
 */
static struct vms_dlm_echo_slot *slot_claim(struct vms_dlm_echo_guard *g)
{
	struct vms_dlm_echo_slot *weakest = &g->slot[0];
	uint32_t i;

	for (i = 0u; i < VMS_DLM_ECHO_SLOTS; i++) {
		if (!g->slot[i].used)
			return &g->slot[i];
		if (g->slot[i].run < weakest->run)
			weakest = &g->slot[i];
	}
	return weakest;
}

static void slot_start(struct vms_dlm_echo_slot *s, uint32_t from,
		       uint32_t req_sig, uint32_t ans_sig)
{
	s->used = 1u;
	s->from = from;
	s->req_sig = req_sig;
	s->ans_sig = ans_sig;
	s->run = 1u;
}

void vms_dlm_echo_guard_init(struct vms_dlm_echo_guard *g)
{
	uint32_t i;

	if (g == (struct vms_dlm_echo_guard *)0)
		return;
	for (i = 0u; i < VMS_DLM_ECHO_SLOTS; i++) {
		g->slot[i].from = 0u;
		g->slot[i].req_sig = 0u;
		g->slot[i].ans_sig = 0u;
		g->slot[i].run = 0u;
		g->slot[i].used = 0u;
	}
	g->refused = 0u;
	g->runs_capped = 0u;
}

int vms_dlm_echo_admit(struct vms_dlm_echo_guard *g, uint32_t from,
		       const uint8_t *req, uint32_t req_len,
		       const uint8_t *ans, uint32_t ans_len,
		       uint8_t *first_cap)
{
	struct vms_dlm_echo_slot *s;
	uint32_t req_sig, ans_sig;

	if (first_cap != (uint8_t *)0)
		*first_cap = 0u;
	/* No guard, or nothing to compare: there is no loop to see. */
	if (g == (struct vms_dlm_echo_guard *)0)
		return 1;
	if (req == (const uint8_t *)0 || req_len == 0u ||
	    ans == (const uint8_t *)0 || ans_len == 0u)
		return 1;

	req_sig = echo_sig(req, req_len);
	ans_sig = echo_sig(ans, ans_len);

	s = slot_for(g, from);
	if (s == (struct vms_dlm_echo_slot *)0) {
		slot_start(slot_claim(g), from, req_sig, ans_sig);
		return 1;
	}
	if (s->req_sig != req_sig || s->ans_sig != ans_sig) {
		/* The dialogue MOVED. That is what a working exchange looks
		 * like, so the run starts over. */
		slot_start(s, from, req_sig, ans_sig);
		return 1;
	}
	if (s->run >= VMS_DLM_ECHO_MAX_SAME) {
		if (s->run == VMS_DLM_ECHO_MAX_SAME) {
			g->runs_capped++;
			if (first_cap != (uint8_t *)0)
				*first_cap = 1u;
		}
		/* Saturate the run so the counter above fires exactly once,
		 * however long the peer keeps asking. */
		if (s->run < 0xffffffffu)
			s->run++;
		g->refused++;
		return 0;
	}
	s->run++;
	return 1;
}
