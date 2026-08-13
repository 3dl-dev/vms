/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_list_netbsd.h - the NetBSD realization of the OVMX executive intrusive
 * list contract (rd vms-4b4, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §5 caveat 2, §7).
 *
 * DO NOT include this directly -- include "exec_list.h", which selects this
 * file on a NetBSD kernel build (OVMX_KBACKEND_NETBSD). See that header for the
 * op contract.
 *
 * WHY THIS SHIPS REAL CODE (not a macro-forward like the Linux backend). Linux
 * list_for_each_entry() and the BSD <sys/queue.h> TAILQ have incompatible
 * shapes -- TAILQ's iterator, node-embedding and container recovery differ from
 * the Linux list_head's -- so the substrate-agnostic facility (which is written
 * against the Linux-shaped exec_list_* API) CANNOT be bridged to TAILQ by
 * macros. The NetBSD backend therefore implements the OVMX list DIRECTLY, from
 * scratch, as a circular doubly-linked list with a sentinel head -- exactly the
 * shape the Linux <linux/list.h> has, so the facility behaves identically.
 *
 * Clean-room (CLAUDE.md Rule 8): this is OVMX's OWN implementation of a textbook
 * circular doubly-linked list. It copies no Linux, NetBSD (<sys/queue.h>), or
 * VSI/HPE source -- only the PUBLIC, universally documented data structure. The
 * O(1) splice/unlink logic and the container_of-based typed iterators are the
 * standard, unpatentable idioms every such list uses.
 *
 * SHAPE. A list is a ring of nodes threaded through an embedded sentinel head:
 *
 *     empty:   head <-> head            (head.next == head.prev == &head)
 *     one:     head <-> A <-> head
 *     two:     head <-> A <-> B <-> head
 *
 * The head and every node are the SAME concrete type (exec_list_node_t), joined
 * in one ring; "empty" is the head pointing at itself. This is what lets
 * exec_list_del() unlink a node in O(1) without knowing the head, and what makes
 * the _safe iterator able to free the current element mid-walk.
 */

#ifndef OVMX_EXEC_LIST_NETBSD_H
#define OVMX_EXEC_LIST_NETBSD_H

#include <sys/types.h>
#include <sys/systm.h>   /* offsetof, and the kernel's C environment */

/* ---- container types ----
 * One concrete struct plays both roles (anchor and embedded link), exactly as
 * the Linux backend uses a single struct list_head for both. The contract keeps
 * the two typedef names so a reader knows which role a field plays. */
struct exec_list_node {
	struct exec_list_node *next;
	struct exec_list_node *prev;
};
typedef struct exec_list_node exec_list_head_t;
typedef struct exec_list_node exec_list_node_t;

/* ---- static definition / initialization ----
 * EXEC_LIST_HEAD_INIT(name) is the self-linked (empty) initializer for an
 * anchor named `name'; EXEC_LIST_HEAD(name) defines and initializes one. */
#define EXEC_LIST_HEAD_INIT(name)  { &(name), &(name) }
#define EXEC_LIST_HEAD(name) \
	exec_list_head_t name = EXEC_LIST_HEAD_INIT(name)

static __inline void
exec_list_head_init(exec_list_head_t *h)
{
	h->next = h;
	h->prev = h;
}

/* ---- query ---- */
static __inline int
exec_list_empty(const exec_list_head_t *h)
{
	return h->next == h;
}

/* ---- mutation (real functions in exec_list_netbsd.c) ---- */
void exec_list_add(exec_list_node_t *n, exec_list_head_t *h);
void exec_list_add_tail(exec_list_node_t *n, exec_list_head_t *h);
void exec_list_del(exec_list_node_t *n);
void exec_list_move(exec_list_node_t *n, exec_list_head_t *h);

/* ---- container recovery ----
 * EXEC_CONTAINER_OF recovers the element that embeds `ptr' as its `member'
 * field. Standard offsetof idiom; named in the OVMX namespace so it never
 * collides with a substrate's own container_of. */
#define EXEC_CONTAINER_OF(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* ---- first element (typed; UNDEFINED on an empty list -- see exec_list.h) ----
 * Same container-recovery idiom as the iterators; the caller must have
 * established non-emptiness (exec_list_empty()) first. */
#define exec_list_first_entry(head, type, member) \
	EXEC_CONTAINER_OF((head)->next, type, member)

/* ---- first element or NULL on empty (typed; see exec_list.h) ----
 * The empty ring is the head linked to itself, so head->next == head; testing
 * that before recovering the container is what makes this safe on an empty
 * list where exec_list_first_entry is undefined. */
#define exec_list_first_entry_or_null(head, type, member) \
	(exec_list_empty(head) ? (type *)0 \
	 : EXEC_CONTAINER_OF((head)->next, type, member))

/* ---- iteration (typed; `member' is the exec_list_node_t field name) ----
 * These MUST be macros: they recover the element from its embedded node using
 * the caller's element type + member name (EXEC_CONTAINER_OF), which a function
 * cannot see. This is the exact reason this backend ships an implementation
 * rather than macro-bridging to TAILQ (design record §5 caveat 2).
 *
 * The walk starts at head->next and stops when the cursor returns to &head, so
 * the sentinel head is never yielded as an element. `pos'/`n' are typed element
 * pointers; the loop condition dereferences the embedded node, never the head
 * as if it were an element. */
#define exec_list_for_each_entry(pos, head, member)                          \
	for ((pos) = EXEC_CONTAINER_OF((head)->next, __typeof__(*(pos)), member); \
	     &(pos)->member != (head);                                           \
	     (pos) = EXEC_CONTAINER_OF((pos)->member.next, __typeof__(*(pos)), member))

#define exec_list_for_each_entry_safe(pos, n, head, member)                  \
	for ((pos) = EXEC_CONTAINER_OF((head)->next, __typeof__(*(pos)), member), \
	     (n)   = EXEC_CONTAINER_OF((pos)->member.next, __typeof__(*(pos)), member); \
	     &(pos)->member != (head);                                           \
	     (pos) = (n),                                                        \
	     (n)   = EXEC_CONTAINER_OF((n)->member.next, __typeof__(*(n)), member))

#endif /* OVMX_EXEC_LIST_NETBSD_H */
