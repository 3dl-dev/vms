/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_pending.c - the unanswered-request table (rd vms-f87).
 *
 * Read vms_dlm_pending.h first: it carries why the table exists, the
 * measurement that grounds the deferred grant, and the bound.
 */
#include "vms_dlm_pending.h"

static void pending_zero(struct vms_dlm_pending_slot *s)
{
	uint32_t i;

	s->csid = 0u;
	s->req_lkid = 0u;
	s->len = 0u;
	s->used = 0u;
	for (i = 0u; i < (uint32_t)VMS_CM_BODY_LEN; i++)
		s->body[i] = 0u;
}

void vms_dlm_pending_init(struct vms_dlm_pending *p)
{
	uint32_t i;

	if (p == (struct vms_dlm_pending *)0)
		return;
	for (i = 0u; i < VMS_DLM_PENDING_MAX; i++)
		pending_zero(&p->slot[i]);
	p->kept = 0u;
	p->replaced = 0u;
	p->overflow = 0u;
	p->dropped = 0u;
}

/* The slot holding (csid, req_lkid), or NULL. */
static struct vms_dlm_pending_slot *pending_find(struct vms_dlm_pending *p,
						 uint32_t csid,
						 uint32_t req_lkid)
{
	uint32_t i;

	for (i = 0u; i < VMS_DLM_PENDING_MAX; i++) {
		if (p->slot[i].used && p->slot[i].csid == csid &&
		    p->slot[i].req_lkid == req_lkid)
			return &p->slot[i];
	}
	return (struct vms_dlm_pending_slot *)0;
}

int vms_dlm_pending_keep(struct vms_dlm_pending *p, uint32_t csid,
			 uint32_t req_lkid, const uint8_t *body, uint32_t len)
{
	struct vms_dlm_pending_slot *s;
	uint32_t i;

	if (p == (struct vms_dlm_pending *)0)
		return 0;
	/*
	 * NOTHING TO ECHO, OR NOTHING TO KEY ON. A short body could not
	 * reproduce the requester's frame, and lock id 0 is the one value this
	 * protocol's handles never take (the fc8540ae lesson) -- so neither is
	 * stored, rather than stored as something to be sent later.
	 */
	if (body == (const uint8_t *)0 || len < (uint32_t)VMS_CM_BODY_LEN ||
	    req_lkid == 0u || csid == 0u)
		return 0;

	s = pending_find(p, csid, req_lkid);
	if (s != (struct vms_dlm_pending_slot *)0) {
		p->replaced++;          /* a retransmit IS the same request */
	} else {
		for (i = 0u; i < VMS_DLM_PENDING_MAX; i++) {
			if (!p->slot[i].used) {
				s = &p->slot[i];
				break;
			}
		}
		if (s == (struct vms_dlm_pending_slot *)0) {
			/*
			 * FULL. The request is still queued in the lock
			 * database and still answered when the requester asks
			 * again (the engine's idempotent cross-node ENQ), which
			 * is exactly the behaviour that shipped before this
			 * table existed. Counted so a full table is visible
			 * rather than felt.
			 */
			p->overflow++;
			return 0;
		}
		p->kept++;
	}

	s->used = 1u;
	s->csid = csid;
	s->req_lkid = req_lkid;
	s->len = (uint32_t)VMS_CM_BODY_LEN;
	for (i = 0u; i < (uint32_t)VMS_CM_BODY_LEN; i++)
		s->body[i] = body[i];
	return 1;
}

uint32_t vms_dlm_pending_take(struct vms_dlm_pending *p, uint32_t csid,
			      uint32_t req_lkid, uint8_t *out, uint32_t cap)
{
	struct vms_dlm_pending_slot *s;
	uint32_t i, n;

	if (p == (struct vms_dlm_pending *)0 || out == (uint8_t *)0)
		return 0u;
	s = pending_find(p, csid, req_lkid);
	if (s == (struct vms_dlm_pending_slot *)0)
		return 0u;
	n = s->len;
	if (n > cap)
		return 0u;          /* a partial body is not a frame */
	for (i = 0u; i < n; i++)
		out[i] = s->body[i];
	pending_zero(s);
	p->dropped++;
	return n;
}

int vms_dlm_pending_drop(struct vms_dlm_pending *p, uint32_t csid,
			 uint32_t req_lkid)
{
	struct vms_dlm_pending_slot *s;

	if (p == (struct vms_dlm_pending *)0)
		return 0;
	s = pending_find(p, csid, req_lkid);
	if (s == (struct vms_dlm_pending_slot *)0)
		return 0;
	pending_zero(s);
	p->dropped++;
	return 1;
}

uint32_t vms_dlm_pending_forget_system(struct vms_dlm_pending *p, uint32_t csid)
{
	uint32_t i, n = 0u;

	if (p == (struct vms_dlm_pending *)0)
		return 0u;
	for (i = 0u; i < VMS_DLM_PENDING_MAX; i++) {
		if (p->slot[i].used && p->slot[i].csid == csid) {
			pending_zero(&p->slot[i]);
			p->dropped++;
			n++;
		}
	}
	return n;
}

uint32_t vms_dlm_pending_held(const struct vms_dlm_pending *p)
{
	uint32_t i, n = 0u;

	if (p == (const struct vms_dlm_pending *)0)
		return 0u;
	for (i = 0u; i < VMS_DLM_PENDING_MAX; i++) {
		if (p->slot[i].used)
			n++;
	}
	return n;
}
