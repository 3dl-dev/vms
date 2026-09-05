/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_hash_netbsd.c - the NetBSD realization of the OVMX executive intrusive
 * hash table (rd vms-ca7, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §4, §5 caveat 2).
 *
 * The mutation half of the hash contract declared in exec_hash_netbsd.h. The
 * typed iterators (exec_hash_for_each[_safe]) live in the header as macros
 * because they recover the element type via container_of at each call site (the
 * same reason exec_list_netbsd.h's iterators are macros); the O(1) splice /
 * unlink / bucketing operations are ordinary functions and live here.
 *
 * This is OVMX's OWN chained hash over a fixed bucket array, each bucket an
 * hlist-shaped singly-linked list with a back-pointer (pprev) so an element can
 * unlink itself in O(1) without walking the chain -- the exact shape of the
 * Linux hlist this backend stands in for, so the substrate-agnostic facility
 * (src/kernel-core/vms_proctab.c) that walks it behaves identically on either
 * substrate. Written from the public, textbook data structure -- no
 * <sys/queue.h>, Linux <linux/hashtable.h>, or VSI/HPE source is copied
 * (CLAUDE.md Rule 8).
 *
 * RCU on this substrate. exec_hash_del_rcu is a PLAIN unlink here: NetBSD has no
 * lockless hash readers -- every walk of vms_proc_hash runs under
 * vms_proc_hash_lock (design record §5 caveat 3's blessed fallback), so once a
 * node is unlinked under that lock no reader can still be traversing it, and its
 * paired exec_free_deferred (exec_kbackend_netbsd.h) reclaims it immediately.
 * The "_rcu" in the name is the shared contract's, honoured trivially here; the
 * grace-period idiom (unlink-with-del_rcu, free-with-free_deferred) is preserved
 * so the facility source is identical to the Linux build.
 */

#include <sys/param.h>
#include <sys/systm.h>     /* NULL */
#include "exec_hash_netbsd.h"

/*
 * exec_hash_del_rcu - RCU-safe unlink of `n' from whatever bucket it is on
 * (plain unlink on NetBSD; see the RCU note above). O(1) via the pprev back-
 * pointer. Matches Linux hash_del_rcu / hlist_del_rcu in effect. After this the
 * node's pprev is left NULL so a double-unlink is a detectable no-op (the
 * ownership-claim discipline vms_proctab.c's reaper relies on).
 */
void
exec_hash_del_rcu(exec_hash_node_t *n)
{
	exec_hash_node_t *next = n->next;
	exec_hash_node_t **pprev = n->pprev;

	if (pprev == NULL)
		return;                 /* already unhashed -- claim already taken */
	*pprev = next;
	if (next != NULL)
		next->pprev = pprev;
	n->next = NULL;
	n->pprev = NULL;
}

/*
 * exec_hash_del - plain (non-RCU) unlink, contract symmetry with exec_hash.h's
 * Phase G resource-hash vocabulary (vms_lock.c, rd vms-ff7). Identical mechanism
 * to exec_hash_del_rcu on this substrate, since NetBSD's del_rcu is already a
 * plain unlink. Type-checked / linked now; called once locks join this module's
 * SRCS.
 */
void
exec_hash_del(exec_hash_node_t *n)
{
	exec_hash_del_rcu(n);
}

/*
 * exec_hash_init_helper - empty every bucket of a fixed-size table (each head's
 * first pointer NULL). Called through the exec_hash_init(name) macro, which
 * passes the bucket count from sizeof. Matches Linux hash_init / __hash_init.
 */
void
exec_hash_init_helper(struct exec_hash_head *tbl, unsigned int nbuckets)
{
	unsigned int i;

	for (i = 0; i < nbuckets; i++)
		tbl[i].first = NULL;
}

/*
 * exec_hash_add_helper - insert `node' at the HEAD of its key's bucket
 * (key % nbuckets), maintaining the hlist back-pointer so the node can later
 * unlink itself in O(1). Matches Linux hash_add / hlist_add_head. Called through
 * the exec_hash_add(name, node, key) macro. The bucket index is a pure
 * distribution choice (a name/pid match in the walk is what decides correctness),
 * so a modulo suffices -- exec_hash.h's contract note that a bucket key's VALUE
 * is never a correctness decision applies to keyed placement here too.
 */
void
exec_hash_add_helper(struct exec_hash_head *tbl, unsigned int nbuckets,
    exec_hash_node_t *node, uint32_t key)
{
	struct exec_hash_head *head = &tbl[key % nbuckets];
	exec_hash_node_t *first = head->first;

	node->next = first;
	if (first != NULL)
		first->pprev = &node->next;
	head->first = node;
	node->pprev = &head->first;
}

/*
 * exec_hash_bucket_of - map `key' to its bucket head, the keyed-lookup half of
 * the resource-hash vocabulary (exec_hash.h Phase G; vms_lock.c, rd vms-ff7).
 * The header's exec_hash_for_each_possible(name, obj, member, key) macro walks
 * the chain returned here. This MUST use the SAME `key % nbuckets' placement as
 * exec_hash_add_helper above, or a keyed lookup would search a different bucket
 * than the one an insert with the same key populated -- so a resource $ENQ'd by
 * one process could never be found by another (the DLM's whole point). The
 * header left this "vms-ff7 fills the helper in .c"; this is that fill (rd
 * vms-f78bb -- required for the `vms' module to LINK with all OVMX symbols
 * resolved so it can be modload'ed; a placeholder-only prototype leaves the
 * whole module with unresolved externals and it cannot load on either
 * substrate).
 */
struct exec_hash_head *
exec_hash_bucket_of(struct exec_hash_head *tbl, unsigned int nbuckets,
    uint32_t key)
{
	return &tbl[key % nbuckets];
}

