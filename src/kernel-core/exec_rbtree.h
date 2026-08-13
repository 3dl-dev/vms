/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_rbtree.h - the OVMX executive intrusive red-black tree contract (rd
 * vms-84a, epic vms-8e8; design record docs/design-netbsd-executive-core.md §3
 * row `exec_rbtree_*`, §5 caveat 2). The FOURTH shim seam, alongside
 * exec_kbackend.h, exec_list.h and exec_hash.h.
 *
 * This is the intrusive, embedded-node balanced tree the lock manager keys its
 * lock-ID database on (vms_lock_id_tree, keyed by the monotonically-assigned
 * lock id). Its API is shaped exactly like the LOW-LEVEL, open-coded
 * <linux/rbtree.h> the executive already uses -- caller-driven descent
 * (rb_left/rb_right), rb_link_node + rb_insert_color to splice a new node, and
 * rb_erase to remove -- but lives in the OVMX `exec_rbtree_*` / `exec_rb_*`
 * namespace so a substrate-agnostic facility in src/kernel-core/ never names a
 * `<linux/…>` type or macro. Like the other three seams it selects a concrete
 * backend at build time:
 *
 *   Linux   (src/kernel/exec_rbtree_linux.h)          -> macro-forwards every op
 *                                                        to <linux/rbtree.h>
 *                                                        (zero behaviour change).
 *   NetBSD  (src/kernel-netbsd/exec_rbtree_netbsd.h)  -> a real OVMX intrusive
 *                                                        red-black tree with the
 *                                                        SAME low-level ops,
 *                                                        because Linux's
 *                                                        rb_link_node/
 *                                                        rb_insert_color
 *                                                        open-coded descent and
 *                                                        NetBSD rb_tree(3)'s
 *                                                        comparator-callback API
 *                                                        have incompatible shapes
 *                                                        and CANNOT be
 *                                                        macro-bridged (design
 *                                                        record §5 #2). It is the
 *                                                        analogue of
 *                                                        exec_list_netbsd.{h,c}.
 *
 * WHY THE LOW-LEVEL SHAPE (and not a comparator API): the executive's lock-ID
 * lookup and insert are open-coded descents over the tree (lock_find_by_id /
 * lock_insert_id in vms_lock.c). Keeping the seam low-level makes the Linux
 * backend a set of trivial forwarders -- the refactor is byte-identical on
 * Linux, which is Phase G's acceptance bar -- at the cost of the NetBSD backend
 * shipping a real intrusive tree rather than binding rb_tree(3). That trade is
 * the same one exec_list.h/exec_hash.h already made for the list and the hash.
 *
 * WHY IT LANDS NOW: vms_lock.c (the lock manager, rd vms-84a, Phase G) is the
 * only executive facility that uses a red-black tree (design record §2 table:
 * `vms_lock.c` is the sole `rbtree` consumer), and it is the LAST facility to be
 * promoted onto the shared core. This header is deliberately MINIMAL -- only the
 * ops vms_lock.c actually calls.
 *
 * Clean-room (CLAUDE.md Rule 8): the container API and its balancing semantics
 * are OVMX's own; the Linux backend maps them to the PUBLIC, documented
 * <linux/rbtree.h> API only. No Linux, NetBSD, or VSI/HPE source or binary is
 * copied.
 *
 * ================================================================
 * THE OPS (contract; the backend header provides the concrete impl)
 * ================================================================
 *
 * Types (concrete per substrate):
 *   exec_rbtree_root_t   the tree anchor. Embed/define one per tree. Linux:
 *                        struct rb_root.
 *   exec_rbtree_node_t   an intrusive link embedded in each element struct.
 *                        Linux: struct rb_node.
 *
 * Initialization:
 *   void exec_rbtree_init(exec_rbtree_root_t *root)   init an anchor empty.
 *       (The former static `= RB_ROOT` initializer becomes a runtime init in the
 *       facility's init entry -- the portable form, exactly as exec_list.h's
 *       anchors and eflag's file-scope lock do. Linux: root->rb_node = NULL.)
 *
 * Descent (read the tree; `node` is an exec_rbtree_node_t *):
 *   exec_rbtree_node_t  *exec_rbtree_first(root)   the root's node, or NULL if
 *                                                  the tree is empty (== the
 *                                                  start of a search descent).
 *   exec_rbtree_node_t  *exec_rb_left (node)       node's left child, or NULL.
 *   exec_rbtree_node_t  *exec_rb_right(node)       node's right child, or NULL.
 *   exec_rb_entry(node, type, member)              recover the typed element from
 *                                                  its embedded node (container_of).
 *                                                  A macro (only a macro sees the
 *                                                  element type + member name),
 *                                                  the same reason exec_list.h's
 *                                                  iterators are macros. Linux:
 *                                                  rb_entry.
 *
 * Insert (splice a NEW node; the caller descends to the empty slot first):
 *   exec_rbtree_node_t **exec_rbtree_root_link(root)   &root's node pointer -- the
 *                                                      writable start of an insert
 *                                                      descent (a "link" slot).
 *   exec_rbtree_node_t **exec_rb_left_link (node)      &node->left  child slot.
 *   exec_rbtree_node_t **exec_rb_right_link(node)      &node->right child slot.
 *   void exec_rb_link_node(exec_rbtree_node_t *node, exec_rbtree_node_t *parent,
 *                          exec_rbtree_node_t **link)
 *       splice `node` in at the empty `link` slot below `parent` (as a red leaf).
 *       Linux: rb_link_node.
 *   void exec_rb_insert_color(exec_rbtree_node_t *node, exec_rbtree_root_t *root)
 *       rebalance/recolour after a link_node. Linux: rb_insert_color.
 *
 * Erase:
 *   void exec_rb_erase(exec_rbtree_node_t *node, exec_rbtree_root_t *root)
 *       remove `node` and rebalance. Linux: rb_erase.
 *
 * USAGE (the exact shape vms_lock.c uses):
 *   search:  n = exec_rbtree_first(&t);
 *            while (n) { e = exec_rb_entry(n, T, m);
 *                        if (k < e->k)      n = exec_rb_left(n);
 *                        else if (k > e->k) n = exec_rb_right(n);
 *                        else               return e; }
 *   insert:  p = exec_rbtree_root_link(&t); parent = NULL;
 *            while (*p) { e = exec_rb_entry(*p, T, m); parent = *p;
 *                         p = (nk < e->k) ? exec_rb_left_link(*p)
 *                                         : exec_rb_right_link(*p); }
 *            exec_rb_link_node(&ne->m, parent, p);
 *            exec_rb_insert_color(&ne->m, &t);
 *   erase:   exec_rb_erase(&e->m, &t);
 */

#ifndef OVMX_EXEC_RBTREE_H
#define OVMX_EXEC_RBTREE_H

/*
 * Backend selection -- identical scheme to exec_kbackend.h / exec_list.h /
 * exec_hash.h.
 */
#if defined(OVMX_KBACKEND_NETBSD)
#  include "exec_rbtree_netbsd.h"
#elif defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__)
#  include "exec_rbtree_linux.h"
#else
#  error "exec_rbtree.h: no kernel backend selected (define OVMX_KBACKEND_LINUX or OVMX_KBACKEND_NETBSD)"
#endif

#endif /* OVMX_EXEC_RBTREE_H */
