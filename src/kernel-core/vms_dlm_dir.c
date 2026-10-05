/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_dir.c - the lock directory's entries (rd vms-8219). See the header.
 *
 * Open addressing with linear probing over a power-of-two table, keyed by the
 * resource's wire hash. A removal back-shifts the rest of its probe chain
 * (Knuth vol. 3, 6.4 Algorithm R), so there are no tombstones to compact and
 * every chain stays exactly as short as its contents.
 */
#include "vms_dlm_dir.h"

/* ==========================================================================
 * 1. Identity and probing
 * ========================================================================== */

static int dir_same(const struct vms_dlm_res_ident *a,
		    const struct vms_dlm_res_ident *b)
{
	uint32_t i;

	if (a->group != b->group || a->mode != b->mode ||
	    a->name_len != b->name_len)
		return 0;
	for (i = 0u; i < a->name_len; i++) {
		if (a->name[i] != b->name[i])
			return 0;
	}
	return 1;
}

/* The slot holding `id`, or -1. */
static int32_t dir_slot_of(const struct vms_dlm_dir *d,
			   const struct vms_dlm_res_ident *id)
{
	uint32_t i, n, mask;

	if (d == (const struct vms_dlm_dir *)0 || d->cap == 0u)
		return -1;
	mask = d->cap - 1u;
	for (n = 0u, i = id->hash & mask; n < d->cap; n++, i = (i + 1u) & mask) {
		const struct vms_dlm_dir_entry *e = &d->slot[i];

		if (e->state == (uint8_t)VMS_DLM_DIR_EMPTY)
			return -1;
		if (e->state == (uint8_t)VMS_DLM_DIR_USED && dir_same(&e->id, id))
			return (int32_t)i;
	}
	return -1;
}

/* The first empty slot on `id`'s probe chain, or -1. */
static int32_t dir_free_slot(const struct vms_dlm_dir *d,
			     const struct vms_dlm_res_ident *id)
{
	uint32_t i, n, mask = d->cap - 1u;

	for (n = 0u, i = id->hash & mask; n < d->cap; n++, i = (i + 1u) & mask) {
		if (d->slot[i].state != (uint8_t)VMS_DLM_DIR_USED)
			return (int32_t)i;
	}
	return -1;
}

/* Is `home` cyclically within (hole, j]? Then the entry at j may stay. */
static int dir_between(uint32_t hole, uint32_t home, uint32_t j)
{
	if (hole <= j)
		return hole < home && home <= j;
	return hole < home || home <= j;
}

/* Empty slot i and back-shift the chain behind it (Algorithm R). */
static void dir_kill(struct vms_dlm_dir *d, uint32_t i)
{
	uint32_t mask = d->cap - 1u, j = i;

	d->slot[i].state = (uint8_t)VMS_DLM_DIR_EMPTY;
	d->used--;
	for (;;) {
		uint32_t home;

		j = (j + 1u) & mask;
		if (d->slot[j].state != (uint8_t)VMS_DLM_DIR_USED)
			return;
		home = d->slot[j].id.hash & mask;
		if (dir_between(i, home, j))
			continue;
		d->slot[i] = d->slot[j];
		d->slot[j].state = (uint8_t)VMS_DLM_DIR_EMPTY;
		i = j;
	}
}

/* Set id -> master, inserting when absent. 0, or -1 when full. */
static int dir_set(struct vms_dlm_dir *d, const struct vms_dlm_res_ident *id,
		   vms_csid_t master)
{
	int32_t i = dir_slot_of(d, id);

	if (i >= 0) {
		d->slot[i].master = master;
		return 0;
	}
	if (d->cap == 0u || d->used + 1u > d->cap - d->cap / 8u) {
		d->full_refusals++;        /* keep 1/8 free: probes stay short */
		return -1;
	}
	i = dir_free_slot(d, id);
	d->slot[i].id = *id;
	d->slot[i].master = master;
	d->slot[i].state = (uint8_t)VMS_DLM_DIR_USED;
	d->used++;
	return 0;
}

/* ==========================================================================
 * 2. The directory's operations
 * ========================================================================== */

int vms_dlm_dir_init(struct vms_dlm_dir *d, struct vms_dlm_dir_entry *storage,
		     uint32_t cap)
{
	uint32_t i;
	struct vms_dlm_dir zero = { 0 };

	if (d == (struct vms_dlm_dir *)0)
		return -1;
	*d = zero;
	if (storage == (struct vms_dlm_dir_entry *)0 || cap == 0u ||
	    (cap & (cap - 1u)) != 0u)
		return -1;
	for (i = 0u; i < cap; i++)
		storage[i].state = (uint8_t)VMS_DLM_DIR_EMPTY;
	d->slot = storage;
	d->cap = cap;
	return 0;
}

const struct vms_dlm_dir_entry *vms_dlm_dir_find(const struct vms_dlm_dir *d,
						 const struct vms_dlm_res_ident *id)
{
	int32_t i;

	if (id == (const struct vms_dlm_res_ident *)0)
		return (const struct vms_dlm_dir_entry *)0;
	i = dir_slot_of(d, id);
	return i < 0 ? (const struct vms_dlm_dir_entry *)0 : &d->slot[i];
}

int vms_dlm_dir_register(struct vms_dlm_dir *d,
			 const struct vms_dlm_res_ident *id, vms_csid_t master)
{
	if (d == (struct vms_dlm_dir *)0 || id == (const struct vms_dlm_res_ident *)0 ||
	    master == 0u)
		return -1;
	if (dir_set(d, id, master) != 0)
		return -1;
	d->registered++;
	return 0;
}

enum vms_dlm_dir_outcome vms_dlm_dir_lookup(struct vms_dlm_dir *d,
					    const struct vms_dlm_res_ident *id,
					    vms_csid_t requester,
					    vms_csid_t *out_master)
{
	const struct vms_dlm_dir_entry *e;

	if (d == (struct vms_dlm_dir *)0 || id == (const struct vms_dlm_res_ident *)0 ||
	    out_master == (vms_csid_t *)0 || requester == 0u)
		return VMS_DLM_DIR_ANSWER_NONE;
	e = vms_dlm_dir_find(d, id);
	if (e != (const struct vms_dlm_dir_entry *)0 && e->master != requester) {
		*out_master = e->master;
		d->answered_redirect++;
		return VMS_DLM_DIR_ANSWER_REDIRECT;
	}
	if (e == (const struct vms_dlm_dir_entry *)0 &&
	    dir_set(d, id, requester) != 0)
		return VMS_DLM_DIR_ANSWER_NONE;
	*out_master = requester;
	d->answered_you++;
	return VMS_DLM_DIR_ANSWER_YOU;
}

int vms_dlm_dir_remove(struct vms_dlm_dir *d,
		       const struct vms_dlm_res_ident *id, vms_csid_t master)
{
	int32_t i;

	if (d == (struct vms_dlm_dir *)0 || id == (const struct vms_dlm_res_ident *)0)
		return -1;
	i = dir_slot_of(d, id);
	if (i < 0 || d->slot[i].master != master) {
		d->remove_unknown++;
		return -1;
	}
	dir_kill(d, (uint32_t)i);
	d->removed++;
	return 0;
}

uint32_t vms_dlm_dir_drop_master(struct vms_dlm_dir *d, vms_csid_t master)
{
	uint32_t i, n = 0u;

	if (d == (struct vms_dlm_dir *)0 || master == 0u)
		return 0u;
	for (i = 0u; i < d->cap; i++) {
		while (d->slot[i].state == (uint8_t)VMS_DLM_DIR_USED &&
		       d->slot[i].master == master) {
			dir_kill(d, i);    /* may shift a later entry into i */
			n++;
		}
	}
	d->dropped += n;
	return n;
}

uint32_t vms_dlm_dir_drop_unless(struct vms_dlm_dir *d,
				 int (*keep)(void *ctx, uint32_t hash),
				 void *ctx)
{
	uint32_t i, n = 0u;

	if (d == (struct vms_dlm_dir *)0 || keep == (int (*)(void *, uint32_t))0)
		return 0u;
	for (i = 0u; i < d->cap; i++) {
		while (d->slot[i].state == (uint8_t)VMS_DLM_DIR_USED &&
		       !keep(ctx, d->slot[i].id.hash)) {
			dir_kill(d, i);    /* may shift a later entry into i */
			n++;
		}
	}
	d->dropped += n;
	return n;
}
