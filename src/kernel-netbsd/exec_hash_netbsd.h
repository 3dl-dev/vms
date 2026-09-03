/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_hash_netbsd.h - the NetBSD realization of the OVMX executive intrusive
 * hash-table contract (rd vms-846b, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §4, §5 caveat 2).
 *
 * DO NOT include this directly -- include "exec_hash.h", which selects this
 * file on a NetBSD kernel build. See that header for the op contract.
 *
 * STATUS -- CONTRACT ONLY, NOT YET COMPILED. The process table (vms_proctab.c)
 * is not in the NetBSD `vms' module's SRCS (src/kernel-netbsd/Makefile builds
 * only vms_eflag.c today), so nothing on the NetBSD side includes exec_hash.h
 * yet. This header therefore DEFINES THE INTERFACE the shared vms_proctab.c
 * names -- so the seam boundary is fixed and correct now (Phase F) -- and names
 * the concrete NetBSD mapping the proctab-on-NetBSD/VAX proof (rd vms-9dc) will
 * implement, exactly as exec_list_netbsd.{h,c} was written for eflag's proof.
 * It is the direct analogue of exec_list_netbsd.h: an OVMX-shipped, freestanding
 * intrusive container, because Linux hash_for_each and BSD hashinit(9)/LIST_*
 * have incompatible shapes and cannot be macro-bridged (design record §5 #2).
 *
 * THE REAL MAPPING (what vms-9dc implements in a companion exec_hash_netbsd.c):
 *   - exec_hash_node_t  == a two-word intrusive link (an OVMX hlist node, the
 *     same shape exec_list_netbsd.h ships), embedded in struct vms_proc.
 *   - the table is `struct exec_hlist_head buckets[1 << bits]`, sized by
 *     hashinit(9) or a fixed power-of-two, keyed like the Linux side
 *     (hash_min(pid, bits)); DECLARE/DEFINE/init/add/possible-lookup live in the
 *     NetBSD vms_internal.h twin + vms_netbsd.c glue (raw), mirroring how the
 *     Linux table lives in module glue -- the core facility spells only the ops
 *     below.
 *   - exec_hash_del_rcu: on NetBSD there are no lockless hash readers (the walks
 *     run under the hash kmutex -- design record §5 caveat 3's blessed
 *     fallback), so this is a plain unlink; the paired exec_free_deferred frees
 *     immediately under that lock. The "_rcu" in the name is the shared
 *     contract's, honoured trivially here.
 *   - exec_hash_for_each[_safe]: walk the OVMX buckets, container_of the node
 *     back to the element -- the same macro shape exec_list_netbsd.h uses.
 *
 * Until vms-9dc ships exec_hash_netbsd.c, the ops forward to extern helpers that
 * are declared but not defined; this file only ever PARSES (no NetBSD TU
 * expands these macros yet), so it cannot affect the live NetBSD event-flag
 * build. When proctab joins the NetBSD SRCS, the helpers get their .c and the
 * two-process PCB-identity proof runs, exactly as Phase C did for event flags.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own container; when implemented it maps
 * only to public, documented NetBSD KPIs (hashinit(9)/queue(3)) or ships OVMX's
 * own nodes. No NetBSD or VSI/HPE source is copied.
 */

#ifndef OVMX_EXEC_HASH_NETBSD_H
#define OVMX_EXEC_HASH_NETBSD_H

#include <sys/types.h>

/* Intrusive node embedded in each hashed element (OVMX hlist node shape). */
typedef struct exec_hash_node {
	struct exec_hash_node  *next;
	struct exec_hash_node **pprev;
} exec_hash_node_t;

/* Deletion + iteration forward to OVMX helpers implemented by rd vms-9dc's
 * companion exec_hash_netbsd.c (declared here so the interface is complete). */
void exec_hash_del_rcu(exec_hash_node_t *n);

/*
 * The typed iterators need the element type + member, so they are macros over a
 * generic bucket walk (exec_hash_walk_init/next), the same technique
 * exec_list_netbsd.h uses. Placeholder shapes; vms-9dc fills the walker in .c.
 */
#define exec_hash_entry(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* NULL-safe container_of: maps a NULL node to a NULL element so an exhausted
 * bucket leaves the loop cursor NULL (the sentinel the outer walk keys on). */
#define exec_hash_entry_safe(ptr, type, member) \
	((ptr) ? exec_hash_entry(ptr, type, member) : NULL)

/*
 * FAITHFUL to Linux hash_for_each's break/return semantics (rd vms-f8a). Linux's
 * hash_for_each guards its OUTER bucket loop with `obj == NULL', so a `break' in
 * the body -- which leaves `obj' set to the matched element -- terminates the
 * WHOLE walk, not just the current bucket's chain. The shared core relies on this
 * (vms_ioctl_procscan breaks at the ordinal it wants; vms_proc_reap_dead breaks
 * on the first dead entry it will reap). A naive nested double-`for' (two
 * independent loops) would make `break' escape only the INNER chain loop and let
 * the OUTER bucket loop keep scanning later buckets -- silently visiting past the
 * intended stop (the P4-A proctab bug: $PROCESS_SCAN's positional `target' was
 * overwritten by a later bucket's entry, so it returned the wrong row and never
 * enumerated a live, $SETPRN-named process). Match Linux exactly: the inner
 * chain loop drives `obj' (NULL at chain end via exec_hash_entry_safe) and the
 * outer loop advances buckets only while `obj == NULL'.
 */
#define exec_hash_for_each(name, bkt, obj, member) \
	for ((bkt) = 0, (obj) = NULL; \
	     (obj) == NULL && (bkt) < (int)(sizeof(name) / sizeof((name)[0])); \
	     (bkt)++) \
		for ((obj) = exec_hash_entry_safe((name)[bkt].first, \
						  __typeof__(*(obj)), member); \
		     (obj) != NULL; \
		     (obj) = exec_hash_entry_safe((obj)->member.next, \
						  __typeof__(*(obj)), member))

#define exec_hash_for_each_safe(name, bkt, tmp, obj, member) \
	for ((bkt) = 0, (obj) = NULL; \
	     (obj) == NULL && (bkt) < (int)(sizeof(name) / sizeof((name)[0])); \
	     (bkt)++) \
		for ((obj) = exec_hash_entry_safe((name)[bkt].first, \
						  __typeof__(*(obj)), member); \
		     (obj) != NULL && ((tmp) = (obj)->member.next, 1); \
		     (obj) = exec_hash_entry_safe((tmp), __typeof__(*(obj)), member))

/*
 * PHASE G ADDITIONS (rd vms-84a) -- CONTRACT ONLY, NOT YET COMPILED. The lock
 * manager's resource hash (vms_res_hash) lives wholly in the core facility and
 * uses the non-RCU table vocabulary below. When locks join the NetBSD SRCS (rd
 * vms-ff7) these map to OVMX bucket helpers in exec_hash_netbsd.c. A bucket is a
 * one-word head; EXEC_DEFINE_HASHTABLE lays down the array, exec_hash_init empties
 * it, exec_hash_add/exec_hash_del splice under the resource lock, and
 * exec_hash_for_each_possible walks the keyed bucket. (exec_jhash is GONE with
 * FC-P4.3 -- see the note in src/kernel-core/exec_hash.h; the bucket key is
 * vms_lock.c's own private one.)
 */
struct exec_hash_head { exec_hash_node_t *first; };

#define EXEC_DECLARE_HASHTABLE(name, bits)  extern struct exec_hash_head name[1 << (bits)]
#define EXEC_DEFINE_HASHTABLE(name, bits)   struct exec_hash_head name[1 << (bits)]

void exec_hash_init_helper(struct exec_hash_head *tbl, unsigned int nbuckets);
#define exec_hash_init(name) \
	exec_hash_init_helper((name), (unsigned int)(sizeof(name) / sizeof((name)[0])))

void exec_hash_add_helper(struct exec_hash_head *tbl, unsigned int nbuckets,
			  exec_hash_node_t *node, uint32_t key);
#define exec_hash_add(name, node, key) \
	exec_hash_add_helper((name), (unsigned int)(sizeof(name) / sizeof((name)[0])), \
			     (node), (key))

void exec_hash_del(exec_hash_node_t *n);


/*
 * The keyed-bucket walk, typed. Placeholder shape (exec_hash_bucket_of maps a key
 * to its head); vms-ff7 fills the helper in .c, same technique as the full-table
 * iterators above.
 */
struct exec_hash_head *exec_hash_bucket_of(struct exec_hash_head *tbl,
					   unsigned int nbuckets, uint32_t key);
#define exec_hash_for_each_possible(name, obj, member, key) \
	for (exec_hash_node_t *_ehp = exec_hash_bucket_of((name), \
		(unsigned int)(sizeof(name) / sizeof((name)[0])), (key))->first; \
	     _ehp && ((obj) = exec_hash_entry(_ehp, __typeof__(*(obj)), member), 1); \
	     _ehp = _ehp->next)

#endif /* OVMX_EXEC_HASH_NETBSD_H */
