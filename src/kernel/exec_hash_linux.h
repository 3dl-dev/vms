/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_hash_linux.h - the LINUX realization of the OVMX executive intrusive
 * hash-table contract (rd vms-846b, epic vms-8e8).
 *
 * DO NOT include this directly -- include "exec_hash.h", which selects this
 * file on a Linux kernel build. See that header for the op contract.
 *
 * Every op macro-forwards to the EXACT <linux/hashtable.h> primitive the
 * executive already used, so vms_proctab.c converted onto exec_hash_* compiles
 * to byte-identical behaviour:
 *
 *   exec_hash_node_t              == struct hlist_node
 *   exec_hash_del_rcu             -> hash_del_rcu
 *   exec_hash_for_each            -> hash_for_each
 *   exec_hash_for_each_safe       -> hash_for_each_safe
 *
 * PHASE G ADDITIONS (rd vms-84a, the lock manager). The lock manager's RESOURCE
 * hash (vms_res_hash) lives WHOLLY inside the core facility -- unlike proctab's
 * process hash, which is split into module glue because it has lockless RCU
 * readers in vms_module.c. The resource hash has no lockless reader (every walk
 * runs under vms_res_hash_lock), it is touched by no file but vms_lock.c, and
 * its add happens on the enqueue path (resource creation), which is facility
 * logic, not a module-lifecycle event. So its whole non-RCU table vocabulary is
 * added here, exactly as exec_hash.h's own header foretold ("Locks (Phase G)
 * bring ... the resource hash; Phase G extends it if it needs more"):
 *
 *   EXEC_DECLARE_HASHTABLE          -> DECLARE_HASHTABLE
 *   EXEC_DEFINE_HASHTABLE           -> DEFINE_HASHTABLE
 *   exec_hash_init                  -> hash_init
 *   exec_hash_add                   -> hash_add          (non-RCU)
 *   exec_hash_for_each_possible     -> hash_for_each_possible
 *   exec_hash_del                   -> hash_del          (non-RCU, plain unlink)
 *   exec_jhash                      -> jhash             (key hashing)
 *
 * The for_each forms MUST stay macros: they resolve the element from its
 * embedded node with the caller's element type + member name (container_of),
 * which a function cannot see -- the exact reason the NetBSD backend has to
 * SHIP an implementation rather than macro-bridge to hashinit(9).
 *
 * The PROCESS table (vms_proc_hash) declaration/definition/init and its RCU
 * add / possible-key lookup are still NOT here: they stay as raw
 * <linux/hashtable.h> in the Linux vms_internal.h and vms_module.c (module
 * glue) because that table has lockless readers. This header carries the ops
 * vms_proctab.c and vms_lock.c call (design record: keep it minimal).
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only the public,
 * documented Linux <linux/hashtable.h> / <linux/jhash.h> API. No code is copied
 * from the Linux source.
 */

#ifndef OVMX_EXEC_HASH_LINUX_H
#define OVMX_EXEC_HASH_LINUX_H

#include <linux/hashtable.h>
#include <linux/jhash.h>   /* jhash */
#include <linux/types.h>   /* struct hlist_node */

/* ---- node type ---- */
typedef struct hlist_node exec_hash_node_t;

/* ---- deletion (RCU-safe; free-defer with exec_free_deferred) ---- */
static inline void exec_hash_del_rcu(exec_hash_node_t *n) { hash_del_rcu(n); }

/* ---- iteration (typed; run under the table's guard lock) ---- */
#define exec_hash_for_each(name, bkt, obj, member) \
	hash_for_each(name, bkt, obj, member)
#define exec_hash_for_each_safe(name, bkt, tmp, obj, member) \
	hash_for_each_safe(name, bkt, tmp, obj, member)

/* ---- Phase G: non-RCU table vocabulary (the lock manager's resource hash) ---- */
#define EXEC_DECLARE_HASHTABLE(name, bits)  DECLARE_HASHTABLE(name, bits)
#define EXEC_DEFINE_HASHTABLE(name, bits)   DEFINE_HASHTABLE(name, bits)
#define exec_hash_init(name)                hash_init(name)
#define exec_hash_add(name, node, key)      hash_add(name, node, key)
#define exec_hash_for_each_possible(name, obj, member, key) \
	hash_for_each_possible(name, obj, member, key)
static inline void exec_hash_del(exec_hash_node_t *n) { hash_del(n); }

/* ---- Phase G: key hashing (the resource-name and directory-node hash) ---- */
static inline uint32_t exec_jhash(const void *key, uint32_t length, uint32_t initval)
{
	return jhash(key, length, initval);
}

#endif /* OVMX_EXEC_HASH_LINUX_H */
