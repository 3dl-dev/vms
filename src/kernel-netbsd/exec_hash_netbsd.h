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
#define exec_hash_for_each(name, bkt, obj, member) \
	for ((bkt) = 0; (bkt) < (int)(sizeof(name) / sizeof((name)[0])); (bkt)++) \
		for (exec_hash_node_t *_ehn = (name)[bkt].first; \
		     _ehn && ((obj) = exec_hash_entry(_ehn, __typeof__(*(obj)), member), 1); \
		     _ehn = _ehn->next)

#define exec_hash_for_each_safe(name, bkt, tmp, obj, member) \
	for ((bkt) = 0; (bkt) < (int)(sizeof(name) / sizeof((name)[0])); (bkt)++) \
		for (exec_hash_node_t *_ehn = (name)[bkt].first; \
		     _ehn && ((tmp) = _ehn->next, \
			      (obj) = exec_hash_entry(_ehn, __typeof__(*(obj)), member), 1); \
		     _ehn = (tmp))

#define exec_hash_entry(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

#endif /* OVMX_EXEC_HASH_NETBSD_H */
