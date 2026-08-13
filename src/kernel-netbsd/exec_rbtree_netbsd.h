/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_rbtree_netbsd.h - the NetBSD realization of the OVMX executive intrusive
 * red-black tree contract (rd vms-84a, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §3 row `exec_rbtree_*`, §5 caveat 2).
 *
 * DO NOT include this directly -- include "exec_rbtree.h", which selects this
 * file on a NetBSD kernel build. See that header for the op contract.
 *
 * STATUS -- IMPLEMENTED (rd vms-ff7). The lock manager (vms_lock.c) is now in
 * the NetBSD `vms' module's SRCS (src/kernel-netbsd/Makefile), so this header's
 * types + descent macros are compiled and the three balancing ops it declares
 * (exec_rb_link_node / exec_rb_insert_color / exec_rb_erase) are DEFINED in the
 * companion exec_rbtree_netbsd.c -- OVMX's own textbook red-black tree, the
 * direct analogue of exec_list_netbsd.{h,c} for eflag and exec_hash_netbsd.{h,c}
 * for proctab. It is an OVMX-shipped, freestanding intrusive container.
 *
 * WHY AN OVMX INTRUSIVE TREE AND NOT rb_tree(3): the shared contract is
 * LOW-LEVEL -- the core descends the tree itself (exec_rb_left/right), splices a
 * node with exec_rb_link_node + exec_rb_insert_color, and removes with
 * exec_rb_erase. NetBSD's rb_tree(3) hides balancing behind a comparator
 * callback and exposes no rb_link_node/rb_insert_color equivalent, so it CANNOT
 * back this low-level contract (design record §5 #2). rd vms-ff7 therefore ships
 * a real ~150-250 LOC OVMX red-black tree (its own code, clean-room) whose node
 * carries rb_left/rb_right/parent-colour fields with the same names the contract
 * exposes, the same way exec_list_netbsd.h ships an OVMX list rather than binding
 * queue(3)'s TAILQ.
 *
 * THE REAL MAPPING (what vms-ff7 implements in a companion exec_rbtree_netbsd.c):
 *   - exec_rbtree_node_t == a three-word intrusive node: rb_left, rb_right, and
 *     a parent+colour word, embedded in struct vms_lock_entry.
 *   - exec_rbtree_root_t == a one-word anchor { rb_node }.
 *   - exec_rbtree_init: rb_node = NULL.
 *   - exec_rbtree_first / exec_rb_left / exec_rb_right / *_link: plain field
 *     reads and address-of, identical in shape to the Linux macros.
 *   - exec_rb_entry: container_of, the same macro exec_hash_netbsd.h ships.
 *   - exec_rb_link_node / exec_rb_insert_color / exec_rb_erase: the OVMX tree's
 *     splice-as-red-leaf, rebalance-after-insert, and erase-with-rebalance --
 *     the standard CLR red-black algorithm, OVMX's own implementation.
 *
 * The splice/rebalance/erase ops are declared here and DEFINED in
 * exec_rbtree_netbsd.c, which every module TU that includes exec_rbtree.h links
 * against (the lock manager is the sole consumer). The Linux backend's
 * corresponding ops macro-forward to <linux/rbtree.h>; this backend supplies its
 * own implementation, exactly as Phase C did for event flags' list.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own container; when implemented it maps
 * only to public, documented NetBSD KPIs or ships OVMX's own tree. No NetBSD or
 * VSI/HPE source is copied.
 */

#ifndef OVMX_EXEC_RBTREE_NETBSD_H
#define OVMX_EXEC_RBTREE_NETBSD_H

#include <sys/types.h>

/* Intrusive node embedded in each tree element (OVMX rb node shape). The
 * parent+colour word is packed as Linux's rb_node does, so exec_rb_link_node can
 * store parent and colour in one field; the core never reads it. */
typedef struct exec_rbtree_node {
	struct exec_rbtree_node  *rb_left;
	struct exec_rbtree_node  *rb_right;
	unsigned long             __rb_parent_color;
} exec_rbtree_node_t;

/* One-word anchor, matching struct rb_root's shape. */
typedef struct exec_rbtree_root {
	struct exec_rbtree_node  *rb_node;
} exec_rbtree_root_t;

/* ---- initialization ---- */
static __inline void exec_rbtree_init(exec_rbtree_root_t *root) { root->rb_node = 0; }

/* ---- descent (read): plain field access, same shape as the Linux macros ---- */
#define exec_rbtree_first(root)      ((root)->rb_node)
#define exec_rb_left(node)           ((node)->rb_left)
#define exec_rb_right(node)          ((node)->rb_right)
#define exec_rb_entry(node, type, member) \
	((type *)((char *)(node) - offsetof(type, member)))

/* ---- insert: writable link slots + splice + rebalance ---- */
#define exec_rbtree_root_link(root)  (&(root)->rb_node)
#define exec_rb_left_link(node)      (&(node)->rb_left)
#define exec_rb_right_link(node)     (&(node)->rb_right)

/* The balancing ops are real OVMX code shipped by rd vms-ff7's companion
 * exec_rbtree_netbsd.c (declared here so the interface is complete). */
void exec_rb_link_node(exec_rbtree_node_t *node, exec_rbtree_node_t *parent,
		       exec_rbtree_node_t **link);
void exec_rb_insert_color(exec_rbtree_node_t *node, exec_rbtree_root_t *root);
void exec_rb_erase(exec_rbtree_node_t *node, exec_rbtree_root_t *root);

#endif /* OVMX_EXEC_RBTREE_NETBSD_H */
