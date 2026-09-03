/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_rbtree_host.h - the HOST realization of the OVMX executive intrusive
 * red-black tree contract (rd FC-P4.9; see exec_kbackend_host.h's header
 * comment for how/why the host build reaches this file instead of
 * exec_rbtree_linux.h without exec_rbtree.h itself being touched).
 *
 * vms_lock.c is exec_rbtree.h's ONLY documented consumer (its own header
 * comment: "the sole rbtree consumer"), keyed by the monotonically-assigned
 * lock ID (vms_lock_id_tree). A real, standalone left-leaning-free (classic
 * CLRS-style) red-black tree, implemented fresh in plain C from the
 * textbook algorithm -- Rule 8: no Linux/NetBSD source is read or copied,
 * this is standard, public computer-science technique, not a VMS wire
 * artifact.
 *
 * CORRECTNESS NOTE: the red/black COLOUR bookkeeping only affects the tree's
 * balance (height), never its BST ordering -- every rotate here preserves
 * in-order sequence, and erase always relinks a node's children/parent
 * before touching colour, so even a subtly-imperfect fixup could only cost
 * performance, never correctness of exec_rbtree_first/exec_rb_left/
 * exec_rb_right descent (vms_lock.c's lock_find_by_id / lock_insert_id /
 * lock_remove_id). Exercised under a larger randomized insert/erase/lookup
 * workload in test_lock_host.c precisely to catch a structural bug
 * empirically, not just by code reading.
 */

#ifndef OVMX_EXEC_RBTREE_HOST_H
#define OVMX_EXEC_RBTREE_HOST_H

#include "exec_host_common.h"   /* EXEC_CONTAINER_OF */

/* ---- node / anchor types ---- */
typedef struct exec_rbtree_node {
	struct exec_rbtree_node *left;
	struct exec_rbtree_node *right;
	struct exec_rbtree_node *parent;
	int                       red;   /* 1 = red, 0 = black */
} exec_rbtree_node_t;

typedef struct exec_rbtree_root {
	exec_rbtree_node_t *node;
} exec_rbtree_root_t;

/* ---- initialization ---- */
static inline void exec_rbtree_init(exec_rbtree_root_t *root) { root->node = (exec_rbtree_node_t *)0; }

/* ---- descent ---- */
static inline exec_rbtree_node_t *exec_rbtree_first(exec_rbtree_root_t *root) { return root->node; }
static inline exec_rbtree_node_t *exec_rb_left(exec_rbtree_node_t *n)  { return n->left; }
static inline exec_rbtree_node_t *exec_rb_right(exec_rbtree_node_t *n) { return n->right; }

#define exec_rb_entry(node, type, member) EXEC_CONTAINER_OF(node, type, member)

/* ---- insert (link slots + splice + rebalance) ---- */
static inline exec_rbtree_node_t **exec_rbtree_root_link(exec_rbtree_root_t *root) { return &root->node; }
static inline exec_rbtree_node_t **exec_rb_left_link(exec_rbtree_node_t *n)  { return &n->left; }
static inline exec_rbtree_node_t **exec_rb_right_link(exec_rbtree_node_t *n) { return &n->right; }

static inline void exec_rb_link_node(exec_rbtree_node_t *node, exec_rbtree_node_t *parent,
				     exec_rbtree_node_t **link)
{
	node->left   = (exec_rbtree_node_t *)0;
	node->right  = (exec_rbtree_node_t *)0;
	node->parent = parent;
	node->red    = 1;
	*link = node;
}

static inline int exec_rb_is_red_(exec_rbtree_node_t *n) { return n && n->red; }

static inline void exec_rb_rotate_left_(exec_rbtree_root_t *root, exec_rbtree_node_t *x)
{
	exec_rbtree_node_t *y = x->right;

	x->right = y->left;
	if (y->left)
		y->left->parent = x;
	y->parent = x->parent;
	if (!x->parent)
		root->node = y;
	else if (x == x->parent->left)
		x->parent->left = y;
	else
		x->parent->right = y;
	y->left = x;
	x->parent = y;
}

static inline void exec_rb_rotate_right_(exec_rbtree_root_t *root, exec_rbtree_node_t *x)
{
	exec_rbtree_node_t *y = x->left;

	x->left = y->right;
	if (y->right)
		y->right->parent = x;
	y->parent = x->parent;
	if (!x->parent)
		root->node = y;
	else if (x == x->parent->right)
		x->parent->right = y;
	else
		x->parent->left = y;
	y->right = x;
	x->parent = y;
}

static inline void exec_rb_insert_color(exec_rbtree_node_t *node, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *z = node;

	while (exec_rb_is_red_(z->parent)) {
		exec_rbtree_node_t *p = z->parent;
		exec_rbtree_node_t *g = p->parent;   /* p red => root is never red => g exists */

		if (p == g->left) {
			exec_rbtree_node_t *u = g->right;

			if (exec_rb_is_red_(u)) {
				p->red = 0;
				u->red = 0;
				g->red = 1;
				z = g;
			} else {
				if (z == p->right) {
					z = p;
					exec_rb_rotate_left_(root, z);
					p = z->parent;
					g = p->parent;
				}
				p->red = 0;
				g->red = 1;
				exec_rb_rotate_right_(root, g);
			}
		} else {
			exec_rbtree_node_t *u = g->left;

			if (exec_rb_is_red_(u)) {
				p->red = 0;
				u->red = 0;
				g->red = 1;
				z = g;
			} else {
				if (z == p->left) {
					z = p;
					exec_rb_rotate_right_(root, z);
					p = z->parent;
					g = p->parent;
				}
				p->red = 0;
				g->red = 1;
				exec_rb_rotate_left_(root, g);
			}
		}
	}
	root->node->red = 0;
}

/* ---- erase ---- */
static inline void exec_rb_transplant_(exec_rbtree_root_t *root, exec_rbtree_node_t *u,
				       exec_rbtree_node_t *v)
{
	if (!u->parent)
		root->node = v;
	else if (u == u->parent->left)
		u->parent->left = v;
	else
		u->parent->right = v;
	if (v)
		v->parent = u->parent;
}

static inline exec_rbtree_node_t *exec_rb_minimum_(exec_rbtree_node_t *n)
{
	while (n->left)
		n = n->left;
	return n;
}

static inline void exec_rb_erase_fixup_(exec_rbtree_root_t *root, exec_rbtree_node_t *x,
					exec_rbtree_node_t *xparent)
{
	while (x != root->node && !exec_rb_is_red_(x)) {
		if (x == (xparent ? xparent->left : (exec_rbtree_node_t *)0)) {
			exec_rbtree_node_t *w = xparent->right;

			if (exec_rb_is_red_(w)) {
				w->red = 0;
				xparent->red = 1;
				exec_rb_rotate_left_(root, xparent);
				w = xparent->right;
			}
			if (!exec_rb_is_red_(w->left) && !exec_rb_is_red_(w->right)) {
				w->red = 1;
				x = xparent;
				xparent = x->parent;
			} else {
				if (!exec_rb_is_red_(w->right)) {
					if (w->left)
						w->left->red = 0;
					w->red = 1;
					exec_rb_rotate_right_(root, w);
					w = xparent->right;
				}
				w->red = xparent->red;
				xparent->red = 0;
				if (w->right)
					w->right->red = 0;
				exec_rb_rotate_left_(root, xparent);
				x = root->node;
				xparent = (exec_rbtree_node_t *)0;
			}
		} else {
			exec_rbtree_node_t *w = xparent->left;

			if (exec_rb_is_red_(w)) {
				w->red = 0;
				xparent->red = 1;
				exec_rb_rotate_right_(root, xparent);
				w = xparent->left;
			}
			if (!exec_rb_is_red_(w->right) && !exec_rb_is_red_(w->left)) {
				w->red = 1;
				x = xparent;
				xparent = x->parent;
			} else {
				if (!exec_rb_is_red_(w->left)) {
					if (w->right)
						w->right->red = 0;
					w->red = 1;
					exec_rb_rotate_left_(root, w);
					w = xparent->left;
				}
				w->red = xparent->red;
				xparent->red = 0;
				if (w->left)
					w->left->red = 0;
				exec_rb_rotate_right_(root, xparent);
				x = root->node;
				xparent = (exec_rbtree_node_t *)0;
			}
		}
	}
	if (x)
		x->red = 0;
}

static inline void exec_rb_erase(exec_rbtree_node_t *z, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *y = z;
	exec_rbtree_node_t *x, *xparent;
	int y_orig_red = y->red;

	if (!z->left) {
		x = z->right;
		xparent = z->parent;
		exec_rb_transplant_(root, z, z->right);
	} else if (!z->right) {
		x = z->left;
		xparent = z->parent;
		exec_rb_transplant_(root, z, z->left);
	} else {
		y = exec_rb_minimum_(z->right);
		y_orig_red = y->red;
		x = y->right;
		if (y->parent == z) {
			xparent = y;
			if (x)
				x->parent = y;
		} else {
			xparent = y->parent;
			exec_rb_transplant_(root, y, y->right);
			y->right = z->right;
			y->right->parent = y;
		}
		exec_rb_transplant_(root, z, y);
		y->left = z->left;
		y->left->parent = y;
		y->red = z->red;
	}
	if (!y_orig_red)
		exec_rb_erase_fixup_(root, x, xparent);
}

#endif /* OVMX_EXEC_RBTREE_HOST_H */
