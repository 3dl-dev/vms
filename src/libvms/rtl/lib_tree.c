/*
 * lib_tree.c - LIB$ binary tree routines
 *
 * Implements:
 *
 *   LIB$INSERT_TREE    - Insert a node into a binary tree
 *   LIB$LOOKUP_TREE    - Look up a node in a binary tree
 *   LIB$TRAVERSE_TREE  - Visit every node of a binary tree in key order
 *
 * These routines maintain a binary search tree whose ordering is defined
 * entirely by a caller-supplied comparison routine, and whose nodes are
 * created by a caller-supplied allocation routine.  The library owns only
 * the first two pointer-sized fields of each node (the left and right
 * links); everything else in the node (the key, counters, ...) belongs to
 * the caller and is reached only through the comparison and action
 * routines.  This matches the LIB$xxx_TREE contract: the node layout is
 *
 *     llink   (address)   <- managed by the library
 *     rlink   (address)   <- managed by the library
 *     reserved (word)     <- VMS balance factor; left zero here
 *     ... caller's data ...
 *
 * NOTE ON BALANCING: VMS builds a height-balanced (AVL) tree, using the
 * reserved word for balance factors.  OVMX builds an ordered but
 * unbalanced binary search tree.  Every result this API exposes -- the
 * key found / not found, the duplicate-key detection, and the in-order
 * traversal order -- is identical either way; only the internal tree
 * height differs, which this API does not expose.  The reserved word is
 * left zero.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$INSERT_TREE,
 *            LIB$LOOKUP_TREE, LIB$TRAVERSE_TREE.
 */

#include <stddef.h>
#include <stdint.h>
#include "ssdef.h"
#include "libdef.h"
#include "lib$routines.h"

/* The library-owned links: llink at offset 0, rlink one pointer later. */
static inline void **t_llink(void *node) { return (void **)node; }
static inline void **t_rlink(void *node)
{
    return (void **)((char *)node + sizeof(void *));
}

typedef int (*tree_compare_fn)(void *symbol, void *node);
typedef int (*tree_alloc_fn)(void *symbol, void **node_out, void *user_data);
typedef int (*tree_action_fn)(void *node, void *user_data);

/* VMS success codes are odd. */
static inline int tree_ok(uint32_t st) { return (st & 1u) != 0; }

/*
 * lib$insert_tree - Insert a node into the tree.
 *
 * Walks the tree using compare_rtn(symbol, node).  On an exact match the
 * existing node is returned in *newnode; unless duplicate keys are
 * enabled (flags bit 0) that is reported as LIB$_KEYALRINS.  At an empty
 * link the allocate routine is called to build the node, which is then
 * linked in and returned in *newnode.
 */
uint32_t lib$insert_tree(void *treehead, void *symbol, const uint32_t *flags,
                         int (*compare_rtn)(void), int (*allocate_rtn)(void),
                         void *newnode, void *user_data)
{
    if (!treehead || !compare_rtn || !allocate_rtn || !newnode)
        return SS$_BADPARAM;

    tree_compare_fn compare = (tree_compare_fn)compare_rtn;
    tree_alloc_fn allocate = (tree_alloc_fn)allocate_rtn;
    uint32_t fl = flags ? *flags : 0;

    void **linkp = (void **)treehead;   /* cell holding the child pointer */
    while (*linkp != NULL) {
        void *cur = *linkp;
        int c = compare(symbol, cur);
        if (c == 0 && !(fl & 1u)) {
            *(void **)newnode = cur;
            return LIB$_KEYALRINS;
        }
        linkp = (c < 0) ? t_llink(cur) : t_rlink(cur);
    }

    void *node = NULL;
    uint32_t st = allocate(symbol, &node, user_data);
    if (!tree_ok(st))
        return st;

    *t_llink(node) = NULL;
    *t_rlink(node) = NULL;
    *linkp = node;
    *(void **)newnode = node;
    return SS$_NORMAL;
}

/*
 * lib$lookup_tree - Find a node by key.
 */
uint32_t lib$lookup_tree(void *treehead, void *symbol,
                         int (*compare_rtn)(void), void *node)
{
    if (!treehead || !compare_rtn || !node)
        return SS$_BADPARAM;

    tree_compare_fn compare = (tree_compare_fn)compare_rtn;
    void *cur = *(void **)treehead;
    while (cur != NULL) {
        int c = compare(symbol, cur);
        if (c == 0) {
            *(void **)node = cur;
            return SS$_NORMAL;
        }
        cur = (c < 0) ? *t_llink(cur) : *t_rlink(cur);
    }
    return LIB$_KEYNOTFOU;
}

/* In-order traversal; stops early (and propagates the status) if the
 * action routine returns a non-success (even) status. */
static uint32_t tree_walk(void *node, tree_action_fn action, void *user_data)
{
    if (node == NULL)
        return SS$_NORMAL;

    uint32_t st = tree_walk(*t_llink(node), action, user_data);
    if (!tree_ok(st))
        return st;
    st = action(node, user_data);
    if (!tree_ok(st))
        return st;
    return tree_walk(*t_rlink(node), action, user_data);
}

/*
 * lib$traverse_tree - Visit every node in ascending key order.
 */
uint32_t lib$traverse_tree(void *treehead, int (*action_rtn)(void),
                           void *user_data)
{
    if (!treehead || !action_rtn)
        return SS$_BADPARAM;

    return tree_walk(*(void **)treehead, (tree_action_fn)action_rtn,
                     user_data);
}
