/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_list_netbsd.c - the NetBSD realization of the OVMX executive intrusive
 * doubly-linked list (rd vms-4b4, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §5 caveat 2).
 *
 * The mutation half of the list contract declared in exec_list_netbsd.h. The
 * query (exec_list_empty), init (exec_list_head_init) and typed iterators live
 * in the header as inlines/macros because they must be visible to the compiler
 * at every call site (the iterators recover the element type via container_of);
 * the O(1) splice/unlink operations are ordinary functions and live here.
 *
 * This is OVMX's OWN circular doubly-linked list, written from the public,
 * textbook data structure -- no <sys/queue.h>, Linux <linux/list.h>, or VSI/HPE
 * source is copied (CLAUDE.md Rule 8). The list is the exact shape of the Linux
 * list_head this backend stands in for, so the substrate-agnostic facility
 * (src/kernel-core/vms_eflag.c) that walks it behaves identically on either
 * substrate.
 *
 * Invariant: every node on a list, and the sentinel head, satisfies
 *   node->next->prev == node  &&  node->prev->next == node.
 * An empty list is the head linked to itself (head->next == head->prev == head).
 */

#include "exec_list_netbsd.h"

/*
 * __exec_list_splice - insert `n' between the already-adjacent `prev' and
 * `next'. Internal helper; the ring invariant holds on entry (prev->next ==
 * next && next->prev == prev) and is restored with `n' spliced in.
 */
static __inline void
__exec_list_splice(exec_list_node_t *n, exec_list_node_t *prev,
    exec_list_node_t *next)
{
	next->prev = n;
	n->next    = next;
	n->prev    = prev;
	prev->next = n;
}

/*
 * exec_list_add - push `n' onto the FRONT of the list headed by `h' (i.e. just
 * after the sentinel head), so it becomes the first element a forward walk
 * yields. Matches Linux list_add(). `n' must not already be on a list.
 */
void
exec_list_add(exec_list_node_t *n, exec_list_head_t *h)
{
	__exec_list_splice(n, h, h->next);
}

/*
 * exec_list_add_tail - push `n' onto the BACK of the list (just before the
 * sentinel head), so it becomes the last element a forward walk yields. Matches
 * Linux list_add_tail(). `n' must not already be on a list. The event-flag
 * facility uses this to keep common clusters in creation order.
 */
void
exec_list_add_tail(exec_list_node_t *n, exec_list_head_t *h)
{
	__exec_list_splice(n, h->prev, h);
}

/*
 * exec_list_del - unlink `n' from whatever list it is on, in O(1), by splicing
 * its neighbours together. Matches Linux list_del(). After this the node's
 * links are poisoned to self so a stale re-walk faults fast rather than wanders
 * a freed ring; callers that will re-add the node re-initialize it first (the
 * facility only ever frees a node after deleting it).
 */
void
exec_list_del(exec_list_node_t *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = n;
	n->prev = n;
}
