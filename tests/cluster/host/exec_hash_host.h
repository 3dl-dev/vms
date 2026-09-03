/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_hash_host.h - the HOST realization of the OVMX executive intrusive
 * hash-table contract (rd FC-P4.9; see exec_kbackend_host.h's header comment
 * for how/why the host build reaches this file instead of exec_hash_linux.h
 * without exec_hash.h itself being touched).
 *
 * Implements the FULL exec_hash.h contract (same "vms_lock.c and the FSMs"
 * scope reasoning as exec_list_host.h): the Phase-G non-RCU table vocabulary
 * (EXEC_DECLARE/DEFINE_HASHTABLE, exec_hash_init/add/del,
 * exec_hash_for_each[_possible][_safe], exec_jhash) that vms_lock.c's
 * resource hash (vms_res_hash) uses, plus the RCU-labelled exec_hash_del_rcu
 * for contract completeness (unused on the host: nothing here has lockless
 * readers, so it behaves exactly like exec_hash_del -- honestly documented,
 * not a silent divergence).
 *
 * A plain array of intrusive singly-linked chains (the same shape
 * <linux/list.h>'s hlist_node uses, and exec_hash.h's own contract comment
 * documents): each bucket is a `next` chain with a `pprev` back-pointer for
 * O(1) unlink without a separate head scan. Standard, public data-structure
 * technique, implemented fresh (Rule 8: no Linux/NetBSD source read/copied).
 *
 * exec_jhash is OVMX's OWN hash function (FNV-1a, public-domain, unrelated to
 * Linux's jhash) -- exec_hash.h's contract is explicit that its VALUE is used
 * only for bucketing/membership modulo, never a correctness decision (name
 * matches are by strncmp), so any well-distributed hash is contract-correct.
 */

#ifndef OVMX_EXEC_HASH_HOST_H
#define OVMX_EXEC_HASH_HOST_H

#include <stddef.h>    /* size_t */
#include <stdint.h>    /* uint32_t */
#include "exec_host_common.h"   /* EXEC_CONTAINER_OF */

/* ---- node type ---- */
typedef struct exec_hash_node {
	struct exec_hash_node  *next;
	struct exec_hash_node **pprev;
} exec_hash_node_t;

/* ---- table declaration / definition (bucket count == 1 << bits, a plain
 * array of chain heads; static storage is zero-initialized, matching
 * DEFINE_HASHTABLE's all-empty initial state) ---- */
#define EXEC_DECLARE_HASHTABLE(name, bits) exec_hash_node_t *name[1u << (bits)]
#define EXEC_DEFINE_HASHTABLE(name, bits)  exec_hash_node_t *name[1u << (bits)]

#define EXEC_HASH_SIZE(name) (sizeof(name) / sizeof((name)[0]))

static inline void exec_hash_init_impl(exec_hash_node_t **tbl, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		tbl[i] = (exec_hash_node_t *)0;
}
#define exec_hash_init(name) exec_hash_init_impl((name), EXEC_HASH_SIZE(name))

/* ---- insert (masks the key into the table's bucket count; power-of-2
 * sized, exactly like Linux's hash_min) ---- */
static inline void exec_hash_add_impl(exec_hash_node_t **tbl, size_t n,
				      exec_hash_node_t *node, uint32_t key)
{
	size_t idx = (size_t)key & (n - 1);
	exec_hash_node_t **head = &tbl[idx];

	node->next = *head;
	if (*head)
		(*head)->pprev = &node->next;
	node->pprev = head;
	*head = node;
}
#define exec_hash_add(name, node, key) \
	exec_hash_add_impl((name), EXEC_HASH_SIZE(name), (node), (key))

/* ---- deletion ---- */
static inline void exec_hash_del(exec_hash_node_t *n)
{
	*n->pprev = n->next;
	if (n->next)
		n->next->pprev = n->pprev;
}

/* RCU-labelled twin: no lockless readers on the host, so this behaves
 * exactly like the plain unlink above (see file header). */
static inline void exec_hash_del_rcu(exec_hash_node_t *n) { exec_hash_del(n); }

/* ---- iteration helpers (typed; container_of with a NULL guard) ---- */
#define EXEC_HASH_ENTRY_SAFE(ptr, type, member) \
	((ptr) ? EXEC_CONTAINER_OF((ptr), type, member) : (type *)0)

#define exec_hash_for_each_possible(name, obj, member, key)                        \
	for (obj = EXEC_HASH_ENTRY_SAFE((name)[(size_t)(key) & (EXEC_HASH_SIZE(name) - 1)], \
					 __typeof__(*obj), member);                 \
	     obj;                                                                   \
	     obj = EXEC_HASH_ENTRY_SAFE((obj)->member.next, __typeof__(*obj), member))

#define exec_hash_for_each(name, bkt, obj, member)                                 \
	for ((bkt) = 0; (size_t)(bkt) < EXEC_HASH_SIZE(name); (bkt)++)              \
		for (obj = EXEC_HASH_ENTRY_SAFE((name)[bkt], __typeof__(*obj), member); \
		     obj;                                                           \
		     obj = EXEC_HASH_ENTRY_SAFE((obj)->member.next, __typeof__(*obj), member))

#define exec_hash_for_each_safe(name, bkt, tmp, obj, member)                       \
	for ((bkt) = 0; (size_t)(bkt) < EXEC_HASH_SIZE(name); (bkt)++)              \
		for (obj = EXEC_HASH_ENTRY_SAFE((name)[bkt], __typeof__(*obj), member), \
		     tmp = (obj) ? (obj)->member.next : (exec_hash_node_t *)0;     \
		     obj;                                                           \
		     obj = EXEC_HASH_ENTRY_SAFE(tmp, __typeof__(*obj), member),    \
		     tmp = (obj) ? (obj)->member.next : (exec_hash_node_t *)0)

/* ---- key hashing (OVMX's own FNV-1a; see file header) ---- */
static inline uint32_t exec_jhash(const void *key, uint32_t length, uint32_t initval)
{
	const unsigned char *p = (const unsigned char *)key;
	uint32_t hash = 2166136261u ^ initval;   /* FNV-1a offset basis, salted */
	uint32_t i;

	for (i = 0; i < length; i++) {
		hash ^= p[i];
		hash *= 16777619u;   /* FNV-1a prime */
	}
	return hash;
}

#endif /* OVMX_EXEC_HASH_HOST_H */
