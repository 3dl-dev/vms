/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_acp_cache.h - the Files-11 ACP's block cache (rd vms-943): file headers,
 * directory blocks and the other single blocks the ACP reads off a volume, kept
 * in executive memory so a lookup that walks [000000] -> ... -> the file's
 * directory, or reads a header it read a moment ago, does not go to the disk
 * again.
 *
 * WHY. The XQP keeps exactly these caches (the pools the public SYSGEN
 * parameters ACP_HDRCACHE, ACP_DIRCACHE and ACP_MAPCACHE size, in blocks), so
 * on VMS a program that opens files by name over and over
 * pays memory lookups, not disk reads. Before this cache, every ACP lookup
 * here read each directory level's header and data blocks one synchronous
 * 512-byte transfer at a time.
 *
 * WHAT IT IS. A set-associative table of 512-byte blocks keyed by
 * (backing major, backing minor, LBN), filled by the ACP's single-block reads.
 * Header-only and lock-free by itself -- the caller holds its lock around every
 * call and owns the storage -- so the SAME code the executive runs is
 * unit-tested on the host (tests/vmsfs/test_acp_cache.c) without a kernel.
 *
 * COHERENCE (the rules the caller keeps; the cache can only ever hold the
 * bytes that are on the disk):
 *   - a block is filled ONLY with bytes just read from the disk, and only if no
 *     write and no invalidation happened anywhere since the read began (the
 *     generation check: vms_acp_cache_gen() before the read, fill(..., gen)
 *     after it). A read that raced a write is used once and not kept.
 *   - every write of a block, by the ACP or by the MSCP server on behalf of a
 *     remote node, invalidates that block AFTER the disk write completes
 *     (vms_acp_cache_wrote). The next read fetches it from the disk.
 *   - $MOUNT and $DISMOUNT of a unit drop every block of that unit.
 *   - the executive keeps the cache OFF while the node is in, or is joining,
 *     a cluster: another node can write a shared disk, and VMS keeps its file
 *     system caches coherent across a cluster through the lock manager, which
 *     this cache does not do. The caller checks that before every use.
 */
#ifndef _VMS_ACP_CACHE_H
#define _VMS_ACP_CACHE_H

#define VMS_ACP_CACHE_BLOCK 512u
#define VMS_ACP_CACHE_WAYS  16u           /* blocks per set */

struct vms_acp_cache_ent {
	unsigned long long lbn;
	unsigned int       major;
	unsigned int       minor;
	unsigned int       valid;
	unsigned int       ref;               /* second-chance bit */
	unsigned char      data[VMS_ACP_CACHE_BLOCK];
};

struct vms_acp_cache_set {
	struct vms_acp_cache_ent way[VMS_ACP_CACHE_WAYS];
	unsigned int             hand;        /* replacement cursor */
};

struct vms_acp_cache {
	struct vms_acp_cache_set **set;       /* nset pointers (caller-owned storage) */
	unsigned int               nset;      /* a power of two */
	unsigned long long         gen;       /* bumped by every write / invalidation */
	unsigned long long         hits, misses, fills, stale, inval;
};

static inline void vms_acp_cache__copy(unsigned char *d, const unsigned char *s)
{
	unsigned int i;

	for (i = 0; i < VMS_ACP_CACHE_BLOCK; i++)
		d[i] = s[i];
}

/* `set` holds nset pointers to zeroed sets; nset must be a power of two. */
static inline void vms_acp_cache_init(struct vms_acp_cache *c,
				      struct vms_acp_cache_set **set,
				      unsigned int nset)
{
	c->set = set;
	c->nset = nset;
	c->gen = 1;
	c->hits = c->misses = c->fills = c->stale = c->inval = 0;
}

static inline struct vms_acp_cache_set *
vms_acp_cache__set(const struct vms_acp_cache *c, unsigned int major,
		   unsigned int minor, unsigned long long lbn)
{
	unsigned long long h = lbn * 0x9E3779B97F4A7C15ull;

	h ^= ((unsigned long long)major << 40) ^ ((unsigned long long)minor << 20);
	h ^= h >> 29;
	return c->set[(unsigned int)h & (c->nset - 1u)];
}

static inline struct vms_acp_cache_ent *
vms_acp_cache__find(struct vms_acp_cache_set *s, unsigned int major,
		    unsigned int minor, unsigned long long lbn)
{
	unsigned int w;

	for (w = 0; w < VMS_ACP_CACHE_WAYS; w++) {
		struct vms_acp_cache_ent *e = &s->way[w];

		if (e->valid && e->lbn == lbn && e->major == major && e->minor == minor)
			return e;
	}
	return 0;
}

/* 1 and the block's bytes in `out` on a hit; 0 on a miss. */
static inline int vms_acp_cache_lookup(struct vms_acp_cache *c, unsigned int major,
				       unsigned int minor, unsigned long long lbn,
				       unsigned char *out)
{
	struct vms_acp_cache_ent *e;

	if (!c->set)
		return 0;
	e = vms_acp_cache__find(vms_acp_cache__set(c, major, minor, lbn), major, minor, lbn);
	if (!e) {
		c->misses++;
		return 0;
	}
	e->ref = 1;
	vms_acp_cache__copy(out, e->data);
	c->hits++;
	return 1;
}

/* Take before reading a block from the disk; hand to vms_acp_cache_fill. */
static inline unsigned long long vms_acp_cache_gen(const struct vms_acp_cache *c)
{
	return c->gen;
}

/* Keep `data`, just read from the disk, unless a write or an invalidation
 * happened since `gen` was taken (then the read may predate the write). */
static inline void vms_acp_cache_fill(struct vms_acp_cache *c, unsigned int major,
				      unsigned int minor, unsigned long long lbn,
				      const unsigned char *data, unsigned long long gen)
{
	struct vms_acp_cache_set *s;
	struct vms_acp_cache_ent *e;
	unsigned int n;

	if (!c->set)
		return;
	if (gen != c->gen) {
		c->stale++;
		return;
	}
	s = vms_acp_cache__set(c, major, minor, lbn);
	e = vms_acp_cache__find(s, major, minor, lbn);
	if (!e) {
		/* an empty way, else the first unreferenced one from the hand
		 * (clearing reference bits on the way: second chance) */
		for (n = 0; n < 2u * VMS_ACP_CACHE_WAYS; n++) {
			struct vms_acp_cache_ent *x = &s->way[s->hand];

			s->hand = (s->hand + 1u) % VMS_ACP_CACHE_WAYS;
			if (!x->valid || !x->ref) {
				e = x;
				break;
			}
			x->ref = 0;
		}
		if (!e)
			e = &s->way[s->hand];
	}
	e->major = major;
	e->minor = minor;
	e->lbn = lbn;
	e->ref = 0;
	vms_acp_cache__copy(e->data, data);
	e->valid = 1;
	c->fills++;
}

/* A block of (major,minor) at `lbn` was written (or a write was attempted):
 * forget it, and refuse every fill whose read began before now. */
static inline void vms_acp_cache_wrote(struct vms_acp_cache *c, unsigned int major,
				       unsigned int minor, unsigned long long lbn)
{
	struct vms_acp_cache_ent *e;

	c->gen++;
	if (!c->set)
		return;
	e = vms_acp_cache__find(vms_acp_cache__set(c, major, minor, lbn), major, minor, lbn);
	if (e) {
		e->valid = 0;
		c->inval++;
	}
}

/* Drop every block of (major,minor): $MOUNT, $DISMOUNT. */
static inline void vms_acp_cache_drop_unit(struct vms_acp_cache *c, unsigned int major,
					   unsigned int minor)
{
	unsigned int i, w;

	c->gen++;
	if (!c->set)
		return;
	for (i = 0; i < c->nset; i++)
		for (w = 0; w < VMS_ACP_CACHE_WAYS; w++) {
			struct vms_acp_cache_ent *e = &c->set[i]->way[w];

			if (e->valid && e->major == major && e->minor == minor) {
				e->valid = 0;
				c->inval++;
			}
		}
}

/* Drop everything: the node went into (or toward) a cluster. */
static inline void vms_acp_cache_drop_all(struct vms_acp_cache *c)
{
	unsigned int i, w;

	c->gen++;
	if (!c->set)
		return;
	for (i = 0; i < c->nset; i++)
		for (w = 0; w < VMS_ACP_CACHE_WAYS; w++)
			if (c->set[i]->way[w].valid) {
				c->set[i]->way[w].valid = 0;
				c->inval++;
			}
}

#endif /* _VMS_ACP_CACHE_H */
