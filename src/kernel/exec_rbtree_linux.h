/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_rbtree_linux.h - the LINUX realization of the OVMX executive intrusive
 * red-black tree contract (rd vms-84a, epic vms-8e8).
 *
 * DO NOT include this directly -- include "exec_rbtree.h", which selects this
 * file on a Linux kernel build. See that header for the op contract.
 *
 * Every op macro-forwards (or trivially inlines) to the EXACT <linux/rbtree.h>
 * primitive the lock manager already used, so vms_lock.c converted onto
 * exec_rbtree_* / exec_rb_* compiles to byte-identical behaviour:
 *
 *   exec_rbtree_root_t / exec_rbtree_node_t  == struct rb_root / struct rb_node
 *   exec_rbtree_init(root)                   -> root->rb_node = NULL  (== RB_ROOT)
 *   exec_rbtree_first(root)                  -> (root)->rb_node
 *   exec_rbtree_root_link(root)              -> &(root)->rb_node
 *   exec_rb_left / exec_rb_right             -> (node)->rb_left / ->rb_right
 *   exec_rb_left_link / exec_rb_right_link   -> &(node)->rb_left / &->rb_right
 *   exec_rb_entry                            -> rb_entry
 *   exec_rb_link_node                        -> rb_link_node
 *   exec_rb_insert_color                     -> rb_insert_color
 *   exec_rb_erase                            -> rb_erase
 *
 * The exec_rb_entry form MUST stay a macro: it resolves the element from its
 * embedded node with the caller's element type + member name (container_of),
 * which a function cannot see -- the exact reason the NetBSD backend has to SHIP
 * an implementation rather than macro-bridge to rb_tree(3).
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only the public,
 * documented Linux <linux/rbtree.h> API. No code is copied from the Linux source.
 */

#ifndef OVMX_EXEC_RBTREE_LINUX_H
#define OVMX_EXEC_RBTREE_LINUX_H

#include <linux/rbtree.h>

/* ---- container types ---- */
typedef struct rb_root exec_rbtree_root_t;
typedef struct rb_node exec_rbtree_node_t;

/* ---- initialization (runtime; == the former static RB_ROOT) ---- */
static inline void exec_rbtree_init(exec_rbtree_root_t *root) { root->rb_node = NULL; }

/* ---- descent (read) ---- */
#define exec_rbtree_first(root)      ((root)->rb_node)
#define exec_rb_left(node)           ((node)->rb_left)
#define exec_rb_right(node)          ((node)->rb_right)
#define exec_rb_entry(node, type, member) \
	rb_entry(node, type, member)

/* ---- insert (writable link slots + splice + rebalance) ---- */
#define exec_rbtree_root_link(root)  (&(root)->rb_node)
#define exec_rb_left_link(node)      (&(node)->rb_left)
#define exec_rb_right_link(node)     (&(node)->rb_right)
static inline void exec_rb_link_node(exec_rbtree_node_t *node,
				     exec_rbtree_node_t *parent,
				     exec_rbtree_node_t **link)
{
	rb_link_node(node, parent, link);
}
static inline void exec_rb_insert_color(exec_rbtree_node_t *node,
					exec_rbtree_root_t *root)
{
	rb_insert_color(node, root);
}

/* ---- erase ---- */
static inline void exec_rb_erase(exec_rbtree_node_t *node,
				 exec_rbtree_root_t *root)
{
	rb_erase(node, root);
}

#endif /* OVMX_EXEC_RBTREE_LINUX_H */
