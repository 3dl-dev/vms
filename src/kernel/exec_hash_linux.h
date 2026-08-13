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
 * The for_each forms MUST stay macros: they resolve the element from its
 * embedded node with the caller's element type + member name (container_of),
 * which a function cannot see -- the exact reason the NetBSD backend has to
 * SHIP an implementation rather than macro-bridge to hashinit(9).
 *
 * The TABLE declaration/definition/init and the add / possible-key lookup are
 * NOT here: they stay as raw <linux/hashtable.h> in the Linux vms_internal.h
 * and vms_module.c (module glue), which the core facility never spells. This
 * header carries only the ops vms_proctab.c calls (design record: keep it
 * minimal).
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only the public,
 * documented Linux <linux/hashtable.h> API. No code is copied from the Linux
 * source.
 */

#ifndef OVMX_EXEC_HASH_LINUX_H
#define OVMX_EXEC_HASH_LINUX_H

#include <linux/hashtable.h>
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

#endif /* OVMX_EXEC_HASH_LINUX_H */
