/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_rbtree_netbsd.c - the NetBSD realization of the OVMX executive intrusive
 * red-black tree (rd vms-ff7, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md §3 row `exec_rbtree_*`, §5 caveat 2).
 *
 * The balancing half of the tree contract declared in exec_rbtree_netbsd.h. The
 * descent/read ops (exec_rbtree_first, exec_rb_left/right, exec_rb_entry) and the
 * writable link-slot accessors are macros/inlines in that header because they
 * must be visible at every call site (exec_rb_entry recovers the element type via
 * container_of); the three splice/rebalance/erase operations are ordinary
 * functions and live here, exactly as exec_list_netbsd.{h,c} splits its query/
 * iterator macros from its O(1) mutators.
 *
 * WHY A REAL OVMX TREE AND NOT rb_tree(3). The shared lock manager
 * (src/kernel-core/vms_lock.c) drives a LOW-LEVEL, caller-descended tree: it
 * walks exec_rb_left/right itself, splices a new node with exec_rb_link_node +
 * exec_rb_insert_color, and removes with exec_rb_erase (lock_find_by_id /
 * lock_insert_id / lock_remove_id). NetBSD's rb_tree(3) hides balancing behind a
 * comparator callback and exposes no rb_link_node/rb_insert_color equivalent, so
 * it CANNOT back this contract (design record §5 #2). This file therefore ships
 * OVMX's OWN intrusive red-black tree, the analogue of exec_list_netbsd.c's OVMX
 * list and exec_hash_netbsd.c's OVMX hash. The node the header defines carries
 * rb_left, rb_right and a packed parent+colour word with the SAME names the
 * Linux <linux/rbtree.h> the executive already uses exposes, so the identical
 * src/kernel-core/vms_lock.c compiles and behaves the same on either substrate.
 *
 * THE ALGORITHM is the standard textbook red-black tree (Cormen/Leiserson/
 * Rivest/Stein, "Introduction to Algorithms", ch. Red-Black Trees): insert as a
 * red leaf then recolour/rotate up (RB-INSERT-FIXUP), and erase by splicing out
 * the node or its in-order successor then repairing the black-height deficit
 * (RB-DELETE / RB-DELETE-FIXUP). The parent pointer and colour are packed into a
 * single word (low bit = colour, RED == 0 so a zeroed word is a red node with a
 * NULL parent), the customary intrusive-tree encoding, which is why
 * exec_rb_link_node can seed parent and colour in one field the core never reads.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own implementation of the public,
 * textbook red-black tree data structure. No Linux, NetBSD, or VSI/HPE source or
 * binary is copied.
 *
 * Invariants maintained (the red-black properties):
 *   1. every node is red or black;
 *   2. the root is black;
 *   3. a red node has no red child;
 *   4. every root->NULL path crosses the same number of black nodes.
 */

#include <sys/param.h>
#include <sys/systm.h>     /* NULL */
#include "exec_rbtree_netbsd.h"

/* Colour lives in the low bit of __rb_parent_color; the parent pointer occupies
 * the rest (nodes are at least word-aligned, so the low bit is always free).
 * RED is 0 so a zeroed word denotes a red node with a NULL parent -- the exact
 * state exec_rb_link_node wants for a fresh leaf below a NULL (root) parent. */
#define RB_RED    0UL
#define RB_BLACK  1UL

static __inline exec_rbtree_node_t *
rb_parent(const exec_rbtree_node_t *n)
{
	return (exec_rbtree_node_t *)(n->__rb_parent_color & ~1UL);
}

static __inline unsigned long
rb_color(const exec_rbtree_node_t *n)
{
	return n->__rb_parent_color & 1UL;
}

static __inline int
rb_is_red(const exec_rbtree_node_t *n)
{
	return rb_color(n) == RB_RED;
}

static __inline int
rb_is_black(const exec_rbtree_node_t *n)
{
	return rb_color(n) == RB_BLACK;
}

/* Set parent, preserving the current colour bit. */
static __inline void
rb_set_parent(exec_rbtree_node_t *n, exec_rbtree_node_t *p)
{
	n->__rb_parent_color = rb_color(n) | (unsigned long)p;
}

/* Set colour, preserving the current parent bits. */
static __inline void
rb_set_color(exec_rbtree_node_t *n, unsigned long c)
{
	n->__rb_parent_color = (n->__rb_parent_color & ~1UL) | (c & 1UL);
}

static __inline void
rb_set_red(exec_rbtree_node_t *n)
{
	n->__rb_parent_color &= ~1UL;
}

static __inline void
rb_set_black(exec_rbtree_node_t *n)
{
	n->__rb_parent_color |= 1UL;
}

/*
 * rb_rotate_left - pivot `x' down-left: x's right child `y' rises into x's slot
 * and x becomes y's left child. Colours are untouched (rb_set_parent preserves
 * them); only the shape changes. `x->rb_right' (y) must be non-NULL.
 */
static void
rb_rotate_left(exec_rbtree_node_t *x, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *y = x->rb_right;
	exec_rbtree_node_t *xp = rb_parent(x);

	x->rb_right = y->rb_left;
	if (y->rb_left != NULL)
		rb_set_parent(y->rb_left, x);

	rb_set_parent(y, xp);
	if (xp == NULL)
		root->rb_node = y;
	else if (x == xp->rb_left)
		xp->rb_left = y;
	else
		xp->rb_right = y;

	y->rb_left = x;
	rb_set_parent(x, y);
}

/*
 * rb_rotate_right - mirror of rb_rotate_left: x's left child `y' rises into x's
 * slot and x becomes y's right child. `x->rb_left' (y) must be non-NULL.
 */
static void
rb_rotate_right(exec_rbtree_node_t *x, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *y = x->rb_left;
	exec_rbtree_node_t *xp = rb_parent(x);

	x->rb_left = y->rb_right;
	if (y->rb_right != NULL)
		rb_set_parent(y->rb_right, x);

	rb_set_parent(y, xp);
	if (xp == NULL)
		root->rb_node = y;
	else if (x == xp->rb_right)
		xp->rb_right = y;
	else
		xp->rb_left = y;

	y->rb_right = x;
	rb_set_parent(x, y);
}

/*
 * exec_rb_link_node - splice `node' in at the empty `link' slot below `parent'
 * as a RED leaf (no children, parent recorded, colour red). The caller (the
 * facility's open-coded insert descent) has already walked to the empty slot and
 * passes &parent's left/right pointer (or &root->rb_node when the tree is empty,
 * parent == NULL) as `link'. exec_rb_insert_color then repairs the tree. This is
 * the shape of the Linux rb_link_node the shared facility calls.
 */
void
exec_rb_link_node(exec_rbtree_node_t *node, exec_rbtree_node_t *parent,
		  exec_rbtree_node_t **link)
{
	node->__rb_parent_color = (unsigned long)parent;  /* parent, colour RED */
	node->rb_left = NULL;
	node->rb_right = NULL;
	*link = node;
}

/*
 * exec_rb_insert_color - restore the red-black properties after `node' was
 * linked in red (RB-INSERT-FIXUP). The only property a red leaf can violate is
 * "a red node has no red child" (property 3), and only when node's parent is
 * also red; the loop recolours/rotates that violation up the tree until it is
 * resolved or reaches the root, then paints the root black (property 2).
 */
void
exec_rb_insert_color(exec_rbtree_node_t *node, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *parent, *gparent, *uncle;

	while ((parent = rb_parent(node)) != NULL && rb_is_red(parent)) {
		/* A red parent cannot be the root (the root is black), so it has a
		 * parent: gparent is non-NULL. */
		gparent = rb_parent(parent);

		if (parent == gparent->rb_left) {
			uncle = gparent->rb_right;
			if (uncle != NULL && rb_is_red(uncle)) {
				/* Case 1: red uncle -- recolour and climb. */
				rb_set_black(parent);
				rb_set_black(uncle);
				rb_set_red(gparent);
				node = gparent;
				continue;
			}
			if (node == parent->rb_right) {
				/* Case 2: node is an inner grandchild -- rotate to
				 * reduce to case 3. */
				node = parent;
				rb_rotate_left(node, root);
				parent = rb_parent(node);
			}
			/* Case 3: node is an outer grandchild -- recolour + rotate. */
			rb_set_black(parent);
			rb_set_red(gparent);
			rb_rotate_right(gparent, root);
		} else {
			/* Mirror image of the above (parent is the right child). */
			uncle = gparent->rb_left;
			if (uncle != NULL && rb_is_red(uncle)) {
				rb_set_black(parent);
				rb_set_black(uncle);
				rb_set_red(gparent);
				node = gparent;
				continue;
			}
			if (node == parent->rb_left) {
				node = parent;
				rb_rotate_right(node, root);
				parent = rb_parent(node);
			}
			rb_set_black(parent);
			rb_set_red(gparent);
			rb_rotate_left(gparent, root);
		}
	}

	rb_set_black(root->rb_node);
}

/*
 * rb_erase_fixup - repair a black-height deficit after a black node was removed
 * (RB-DELETE-FIXUP). `node' is the (possibly NULL) child that moved into the
 * removed node's place and now carries an extra "doubly-black" token; `parent'
 * is its parent (passed explicitly because `node' may be NULL and cannot supply
 * it). The loop pushes the token up or resolves it with a rotation. In a valid
 * red-black tree a doubly-black node always has a non-NULL sibling (`other'),
 * because the sibling subtree must supply the black height the deficit side
 * lost -- so the sibling dereferences here are safe.
 */
static void
rb_erase_fixup(exec_rbtree_node_t *node, exec_rbtree_node_t *parent,
	       exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *other;

	while ((node == NULL || rb_is_black(node)) && node != root->rb_node) {
		if (parent->rb_left == node) {
			other = parent->rb_right;
			if (rb_is_red(other)) {
				/* Case 1: red sibling -- rotate to make it black. */
				rb_set_black(other);
				rb_set_red(parent);
				rb_rotate_left(parent, root);
				other = parent->rb_right;
			}
			if ((other->rb_left == NULL || rb_is_black(other->rb_left)) &&
			    (other->rb_right == NULL || rb_is_black(other->rb_right))) {
				/* Case 2: sibling with two black children -- recolour
				 * it red and push the token up to the parent. */
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			} else {
				if (other->rb_right == NULL ||
				    rb_is_black(other->rb_right)) {
					/* Case 3: sibling's far child is black --
					 * rotate the sibling to make case 4. */
					if (other->rb_left != NULL)
						rb_set_black(other->rb_left);
					rb_set_red(other);
					rb_rotate_right(other, root);
					other = parent->rb_right;
				}
				/* Case 4: sibling's far child is red -- recolour +
				 * rotate to clear the deficit. */
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_right != NULL)
					rb_set_black(other->rb_right);
				rb_rotate_left(parent, root);
				node = root->rb_node;
				break;
			}
		} else {
			/* Mirror image (node is the right child). */
			other = parent->rb_left;
			if (rb_is_red(other)) {
				rb_set_black(other);
				rb_set_red(parent);
				rb_rotate_right(parent, root);
				other = parent->rb_left;
			}
			if ((other->rb_left == NULL || rb_is_black(other->rb_left)) &&
			    (other->rb_right == NULL || rb_is_black(other->rb_right))) {
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			} else {
				if (other->rb_left == NULL ||
				    rb_is_black(other->rb_left)) {
					if (other->rb_right != NULL)
						rb_set_black(other->rb_right);
					rb_set_red(other);
					rb_rotate_left(other, root);
					other = parent->rb_left;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_left != NULL)
					rb_set_black(other->rb_left);
				rb_rotate_right(parent, root);
				node = root->rb_node;
				break;
			}
		}
	}

	if (node != NULL)
		rb_set_black(node);
}

/*
 * exec_rb_erase - remove `node' from the tree and rebalance (RB-DELETE). A node
 * with fewer than two children is spliced out directly; a node with two children
 * is replaced by its in-order successor (the leftmost node of its right subtree),
 * which itself has at most one child. If the node actually removed from the tree
 * shape was black, a black-height deficit is left behind and rb_erase_fixup
 * repairs it. This is the shape of the Linux rb_erase the shared facility calls.
 */
void
exec_rb_erase(exec_rbtree_node_t *node, exec_rbtree_root_t *root)
{
	exec_rbtree_node_t *child, *parent;
	unsigned long color;

	if (node->rb_left == NULL) {
		child = node->rb_right;
	} else if (node->rb_right == NULL) {
		child = node->rb_left;
	} else {
		/* Two children: find the in-order successor `old' -> `node's
		 * replacement, the leftmost node of the right subtree. */
		exec_rbtree_node_t *old = node;
		exec_rbtree_node_t *left;

		node = node->rb_right;
		while ((left = node->rb_left) != NULL)
			node = left;

		/* Relink old's parent to point at the successor `node'. */
		parent = rb_parent(old);
		if (parent != NULL) {
			if (parent->rb_left == old)
				parent->rb_left = node;
			else
				parent->rb_right = node;
		} else {
			root->rb_node = node;
		}

		child = node->rb_right;
		parent = rb_parent(node);
		color = rb_color(node);

		if (parent == old) {
			/* The successor is old's direct right child. */
			parent = node;
		} else {
			if (child != NULL)
				rb_set_parent(child, parent);
			parent->rb_left = child;

			node->rb_right = old->rb_right;
			rb_set_parent(old->rb_right, node);
		}

		/* The successor inherits old's slot, colour and both subtrees. */
		node->__rb_parent_color = old->__rb_parent_color;
		node->rb_left = old->rb_left;
		rb_set_parent(old->rb_left, node);

		if (color == RB_BLACK)
			rb_erase_fixup(child, parent, root);
		return;
	}

	/* Zero or one child: splice `node' out, `child' takes its place. */
	parent = rb_parent(node);
	color = rb_color(node);

	if (child != NULL)
		rb_set_parent(child, parent);
	if (parent != NULL) {
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	} else {
		root->rb_node = child;
	}

	if (color == RB_BLACK)
		rb_erase_fixup(child, parent, root);
}
