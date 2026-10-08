/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_dir.c - the lock directory's entries (rd vms-8219). See the header.
 *
 * Open addressing with linear probing over a power-of-two table, keyed by the
 * resource NAME (see the header: NOT by the wire hash, which this node does not
 * always have and which its own entries never have). A removal back-shifts the
 * rest of its probe chain (Knuth vol. 3, 6.4 Algorithm R), so there are no
 * tombstones to compact and every chain stays exactly as short as its contents.
 */
#include "vms_dlm_dir.h"

/* ==========================================================================
 * 1. Identity and probing
 * ========================================================================== */

/*
 * THIS TABLE'S OWN PROBE INDEX -- OVMX's, over the NAME BYTES, and it is not a
 * directory hash.
 *
 * FNV-1a (offset basis 2166136261, prime 16777619): public, well understood,
 * and chosen precisely because it bears no relationship to DEC's unpublished
 * resource-name hash, which this tree never reproduces (Rule 8). Nothing is
 * ROUTED by this value: routing is the Lock Directory Weight Vector indexed by
 * the WIRE hash (vms_dlm_ldwv.h), and no caller of this file ever sees the
 * number below. It selects a bucket in a private in-memory table and that is
 * all it does -- the same freedom a real directory node has, which indexes its
 * Resource Hash Table however it likes and then "scans that chain BY NAME"
 * (Davis p. 6-50; docs/design-cluster-book-grounding.md §1.2).
 */
static uint32_t dir_name_index(const uint8_t *name, uint32_t name_len)
{
	uint32_t h = 2166136261u, i;

	for (i = 0u; i < name_len; i++) {
		h ^= (uint32_t)name[i];
		h *= 16777619u;
	}
	return h;
}

static uint32_t dir_home(const struct vms_dlm_dir *d,
			 const struct vms_dlm_dir_entry *e)
{
	return dir_name_index(e->id.name, e->id.name_len) & (d->cap - 1u);
}

static int dir_same_name(const struct vms_dlm_res_ident *a,
			 const uint8_t *name, uint32_t name_len)
{
	uint32_t i;

	if (a->name_len != (uint8_t)name_len)
		return 0;
	for (i = 0u; i < name_len; i++) {
		if (a->name[i] != name[i])
			return 0;
	}
	return 1;
}

/*
 * EXACT WIRE IDENTITY. A NAME-ONLY entry (this node's own self-claim) carries no
 * group and no access mode, so it can never match one: the wire ops stay exact,
 * and the conflation lives only where it is safe -- see the header.
 */
static int dir_same(const struct vms_dlm_dir_entry *e,
		    const struct vms_dlm_res_ident *b)
{
	if (e->name_only)
		return 0;
	if (e->id.group != b->group || e->id.mode != b->mode)
		return 0;
	return dir_same_name(&e->id, b->name, b->name_len);
}

/* The slot holding `id` by EXACT wire identity, or -1. */
static int32_t dir_slot_of(const struct vms_dlm_dir *d,
			   const struct vms_dlm_res_ident *id)
{
	uint32_t i, n, mask;

	if (d == (const struct vms_dlm_dir *)0 || d->cap == 0u)
		return -1;
	mask = d->cap - 1u;
	for (n = 0u, i = dir_name_index(id->name, id->name_len) & mask;
	     n < d->cap; n++, i = (i + 1u) & mask) {
		const struct vms_dlm_dir_entry *e = &d->slot[i];

		if (e->state == (uint8_t)VMS_DLM_DIR_EMPTY)
			return -1;
		if (e->state == (uint8_t)VMS_DLM_DIR_USED && dir_same(e, id))
			return (int32_t)i;
	}
	return -1;
}

/* The first empty slot on the probe chain of `name`, or -1. */
static int32_t dir_free_slot(const struct vms_dlm_dir *d, const uint8_t *name,
			     uint32_t name_len)
{
	uint32_t i, n, mask = d->cap - 1u;

	for (n = 0u, i = dir_name_index(name, name_len) & mask; n < d->cap;
	     n++, i = (i + 1u) & mask) {
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
		home = dir_home(d, &d->slot[j]);
		if (dir_between(i, home, j))
			continue;
		d->slot[i] = d->slot[j];
		d->slot[j].state = (uint8_t)VMS_DLM_DIR_EMPTY;
		i = j;
	}
}

/* Is there room for one more entry? Keep 1/8 free so probes stay short. */
static int dir_has_room(struct vms_dlm_dir *d)
{
	if (d->cap == 0u || d->used + 1u > d->cap - d->cap / 8u) {
		d->full_refusals++;
		return 0;
	}
	return 1;
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
	if (!dir_has_room(d))
		return -1;
	i = dir_free_slot(d, id->name, id->name_len);
	d->slot[i].id = *id;
	d->slot[i].master = master;
	d->slot[i].state = (uint8_t)VMS_DLM_DIR_USED;
	/*
	 * A wire frame carried body[128:132] -- but a ZERO there is not a hash.
	 * The codec states the rule for the field ("there is no 'hash 0'
	 * fallback, because 'the frame did not carry one' and 'the hash is 0'
	 * are different facts") and this is the same rule one layer up: an entry
	 * whose stored value is 0 is an entry with NO usable hash, so a caller
	 * that needs one to address a frame is told it has none rather than
	 * handed a zero. A zero in that field is what made a real VAX install
	 * OVMX as master of resources it did not master.
	 */
	d->slot[i].hash_known = (id->hash != 0u) ? 1u : 0u;
	d->slot[i].name_only = 0u;
	d->used++;
	return 0;
}

/* ==========================================================================
 * 2. The directory's operations
 * ========================================================================== */

/* Defined with vms_dlm_dir_lookup_name below; used by the claim above it. */
static enum vms_dlm_dir_name_outcome
dir_scan_name(struct vms_dlm_dir *d, const uint8_t *name, uint32_t name_len,
	      vms_csid_t *out_master, uint32_t *out_hash,
	      uint8_t *out_hash_known);

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
	for (i = 0u; i < cap; i++) {
		storage[i].state = (uint8_t)VMS_DLM_DIR_EMPTY;
		storage[i].hash_known = 0u;
		storage[i].name_only = 0u;
	}
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

/*
 * Does a NAME-ONLY entry for this name name `self`? (rd vms-db2a.) That is this
 * node's own record of its own mastery, and it is the only thing that makes
 * p. 6-51's outcome (a) reachable for a name whose UIC group and access mode
 * this executive does not hold.
 */
static int dir_self_claims(const struct vms_dlm_dir *d,
			   const struct vms_dlm_res_ident *id, vms_csid_t self)
{
	uint32_t i, n, mask;

	if (self == 0u || d->cap == 0u)
		return 0;
	mask = d->cap - 1u;
	for (n = 0u, i = dir_name_index(id->name, id->name_len) & mask;
	     n < d->cap; n++, i = (i + 1u) & mask) {
		const struct vms_dlm_dir_entry *e = &d->slot[i];

		if (e->state == (uint8_t)VMS_DLM_DIR_EMPTY)
			return 0;
		if (e->name_only && e->master == self &&
		    dir_same_name(&e->id, id->name, id->name_len))
			return 1;
	}
	return 0;
}

enum vms_dlm_dir_outcome vms_dlm_dir_lookup(struct vms_dlm_dir *d,
					    const struct vms_dlm_res_ident *id,
					    vms_csid_t requester,
					    vms_csid_t self,
					    vms_csid_t *out_master)
{
	const struct vms_dlm_dir_entry *e;

	if (d == (struct vms_dlm_dir *)0 || id == (const struct vms_dlm_res_ident *)0 ||
	    out_master == (vms_csid_t *)0 || requester == 0u)
		return VMS_DLM_DIR_ANSWER_NONE;
	e = vms_dlm_dir_find(d, id);
	if (e != (const struct vms_dlm_dir_entry *)0 && e->master != requester) {
		/* p. 6-51: an entry naming the DIRECTORY NODE ITSELF means this
		 * node is the master and resolves the request -- it is not a
		 * redirect, which would send the asker straight back here. */
		if (self != 0u && e->master == self) {
			*out_master = self;
			d->answered_self++;
			return VMS_DLM_DIR_ANSWER_SELF;
		}
		*out_master = e->master;
		d->answered_redirect++;
		return VMS_DLM_DIR_ANSWER_REDIRECT;
	}
	if (e == (const struct vms_dlm_dir_entry *)0) {
		/* No exact-identity entry -- but this node may hold a NAME-ONLY
		 * record of its own mastery of that name, and then the answer is
		 * p. 6-51's outcome (a) and NEVER "you master it". */
		if (dir_self_claims(d, id, self)) {
			*out_master = self;
			d->answered_self++;
			return VMS_DLM_DIR_ANSWER_SELF;
		}
		if (dir_set(d, id, requester) != 0)
			return VMS_DLM_DIR_ANSWER_NONE;
	}
	*out_master = requester;
	d->answered_you++;
	return VMS_DLM_DIR_ANSWER_YOU;
}

int vms_dlm_dir_claim_self(struct vms_dlm_dir *d, const char *name,
			   uint32_t name_len, vms_csid_t master)
{
	vms_csid_t held = 0u;
	int32_t slot;

	if (d == (struct vms_dlm_dir *)0 || name == (const char *)0 ||
	    master == 0u || name_len == 0u || name_len > VMS_DLM_NAME_MAX)
		return -1;
	/* Already recorded as ours, exactly or name-only? Nothing to do. */
	if (dir_scan_name(d, (const uint8_t *)name, name_len, &held,
			  (uint32_t *)0, (uint8_t *)0) ==
	    VMS_DLM_DIR_NAME_MASTER && held == master)
		return 0;
	if (!dir_has_room(d))
		return -1;
	slot = dir_free_slot(d, (const uint8_t *)name, name_len);
	if (slot < 0)
		return -1;
	{
		struct vms_dlm_dir_entry *e = &d->slot[slot];
		uint32_t i;

		e->id.hash = 0u;
		e->id.group = 0u;
		e->id.mode = 0u;
		e->id.name_len = (uint8_t)name_len;
		for (i = 0u; i < name_len; i++)
			e->id.name[i] = (uint8_t)name[i];
		e->master = master;
		e->state = (uint8_t)VMS_DLM_DIR_USED;
		e->hash_known = 0u;   /* this node holds none: never a zero hash */
		e->name_only = 1u;
	}
	d->used++;
	d->self_claims++;
	return 0;
}

/*
 * Scan the name's probe chain. Counts nothing, so the claim path above can use
 * it too -- `name_lookups` means "this executive's own $ENQ asked", and a claim
 * is not an ask.
 */
static enum vms_dlm_dir_name_outcome
dir_scan_name(struct vms_dlm_dir *d, const uint8_t *name, uint32_t name_len,
	      vms_csid_t *out_master, uint32_t *out_hash,
	      uint8_t *out_hash_known)
{
	uint32_t i, n, mask;
	vms_csid_t master = 0u;
	uint32_t hash = 0u;
	uint8_t hash_known = 0u;
	int found = 0;

	mask = d->cap - 1u;
	for (n = 0u, i = dir_name_index((const uint8_t *)name, name_len) & mask;
	     n < d->cap; n++, i = (i + 1u) & mask) {
		const struct vms_dlm_dir_entry *e = &d->slot[i];

		if (e->state == (uint8_t)VMS_DLM_DIR_EMPTY)
			break;
		if (!dir_same_name(&e->id, name, name_len))
			continue;
		if (found && e->master != master) {
			/* Two resource domains of one name, mastered on
			 * different systems. Refused: picking one would route a
			 * lock request on a coin toss. */
			return VMS_DLM_DIR_NAME_AMBIGUOUS;
		}
		found = 1;
		master = e->master;
		if (e->hash_known && !hash_known) {
			hash = e->id.hash;
			hash_known = 1u;
		}
	}
	if (!found)
		return VMS_DLM_DIR_NAME_NONE;
	*out_master = master;
	if (out_hash != (uint32_t *)0)
		*out_hash = hash;
	if (out_hash_known != (uint8_t *)0)
		*out_hash_known = hash_known;
	return VMS_DLM_DIR_NAME_MASTER;
}

enum vms_dlm_dir_name_outcome
vms_dlm_dir_lookup_name(struct vms_dlm_dir *d, const char *name,
			uint32_t name_len, vms_csid_t *out_master,
			uint32_t *out_hash, uint8_t *out_hash_known)
{
	enum vms_dlm_dir_name_outcome o;

	if (d == (struct vms_dlm_dir *)0 || name == (const char *)0 ||
	    out_master == (vms_csid_t *)0 || d->cap == 0u || name_len == 0u ||
	    name_len > VMS_DLM_NAME_MAX)
		return VMS_DLM_DIR_NAME_INVAL;

	d->name_lookups++;
	o = dir_scan_name(d, (const uint8_t *)name, name_len, out_master,
			  out_hash, out_hash_known);
	if (o == VMS_DLM_DIR_NAME_AMBIGUOUS)
		d->name_ambiguous++;
	return o;
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
				 int (*keep)(void *ctx, uint32_t hash,
					     int hash_known),
				 void *ctx)
{
	uint32_t i, n = 0u;

	if (d == (struct vms_dlm_dir *)0 ||
	    keep == (int (*)(void *, uint32_t, int))0)
		return 0u;
	for (i = 0u; i < d->cap; i++) {
		while (d->slot[i].state == (uint8_t)VMS_DLM_DIR_USED &&
		       !keep(ctx, d->slot[i].id.hash,
			     d->slot[i].hash_known ? 1 : 0)) {
			dir_kill(d, i);    /* may shift a later entry into i */
			n++;
		}
	}
	d->dropped += n;
	return n;
}
