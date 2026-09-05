/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_list_host.h - the HOST realization of the OVMX executive intrusive
 * doubly-linked list contract (rd FC-P4.9; see exec_kbackend_host.h's header
 * comment for how/why the host build reaches this file instead of
 * exec_list_linux.h without exec_list.h itself being touched).
 *
 * Unlike exec_kbackend_host.h (scoped to only what vms_lock.c calls), this
 * implements the FULL exec_list.h contract: the plan item's own summary
 * covers "vms_lock.c and the FSMs" (docs/plan-faithful-cluster-executive.md
 * FC-P4.9), and design sec 3.9's per-layer table says every `_fsm.c` reaches
 * exec_kbackend.h's container seams (list/hash/rbtree) directly -- so a later
 * FSM host build (FC-P4.10, blocked on this item) needs the complete
 * vocabulary, not just today's subset. A real, standalone circular doubly-
 * linked list, the same shape <linux/list.h> uses (and exec_list.h's own
 * contract comment documents), implemented fresh in plain C -- Rule 8: no
 * Linux/NetBSD source is read or copied, this is a standard, public data-
 * structure technique.
 */

#ifndef OVMX_EXEC_LIST_HOST_H
#define OVMX_EXEC_LIST_HOST_H

#include "exec_host_common.h"   /* EXEC_CONTAINER_OF */

/* ---- container types (a node IS an anchor -- same shape, exec_list.h's
 * documented reason for keeping them distinct typedefs) ---- */
typedef struct exec_list_node {
	struct exec_list_node *next;
	struct exec_list_node *prev;
} exec_list_node_t;

typedef exec_list_node_t exec_list_head_t;

/* ---- static definition / initialization ---- */
#define EXEC_LIST_HEAD_INIT(name) { &(name), &(name) }
#define EXEC_LIST_HEAD(name) exec_list_head_t name = EXEC_LIST_HEAD_INIT(name)

static inline void exec_list_head_init(exec_list_head_t *h)
{
	h->next = h;
	h->prev = h;
}

/* ---- mutation (O(1); node must not already be on a list for the adds) --- */
static inline void exec_list_add(exec_list_node_t *n, exec_list_head_t *h)
{
	n->next = h->next;
	n->prev = h;
	h->next->prev = n;
	h->next = n;
}

static inline void exec_list_add_tail(exec_list_node_t *n, exec_list_head_t *h)
{
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

static inline void exec_list_del(exec_list_node_t *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	n->next = (exec_list_node_t *)0;
	n->prev = (exec_list_node_t *)0;
}

static inline void exec_list_move(exec_list_node_t *n, exec_list_head_t *h)
{
	exec_list_del(n);
	exec_list_add(n, h);
}

/* ---- query ---- */
static inline int exec_list_empty(const exec_list_head_t *h)
{
	return h->next == h;
}

/* ---- first element (typed) ---- */
#define exec_list_first_entry(head, type, member) \
	EXEC_CONTAINER_OF((head)->next, type, member)

#define exec_list_first_entry_or_null(head, type, member) \
	(exec_list_empty(head) ? (type *)0 : exec_list_first_entry(head, type, member))

/* ---- iteration (typed; `member` is the exec_list_node_t field name) ---- */
#define exec_list_for_each_entry(pos, head, member)                         \
	for (pos = EXEC_CONTAINER_OF((head)->next, __typeof__(*pos), member);   \
	     &pos->member != (head);                                            \
	     pos = EXEC_CONTAINER_OF(pos->member.next, __typeof__(*pos), member))

#define exec_list_for_each_entry_safe(pos, n, head, member)                       \
	for (pos = EXEC_CONTAINER_OF((head)->next, __typeof__(*pos), member),        \
	     n = EXEC_CONTAINER_OF(pos->member.next, __typeof__(*pos), member);      \
	     &pos->member != (head);                                                 \
	     pos = n, n = EXEC_CONTAINER_OF(n->member.next, __typeof__(*n), member))

#endif /* OVMX_EXEC_LIST_HOST_H */
