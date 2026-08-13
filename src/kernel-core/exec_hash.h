/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_hash.h - the OVMX executive intrusive hash-table contract (rd vms-846b,
 * epic vms-8e8; design record docs/design-netbsd-executive-core.md §4, §5
 * caveat 2). The THIRD shim seam, alongside exec_kbackend.h and exec_list.h.
 *
 * This is the intrusive, embedded-node hash table the executive keys its
 * process database on (vms_proc_hash, keyed by host pid). Its API is shaped
 * exactly like the Linux <linux/hashtable.h> the executive already uses, but
 * lives in the OVMX `exec_hash_*` namespace so a substrate-agnostic facility in
 * src/kernel-core/ never names a `<linux/…>` type or macro. Like the other two
 * seams it selects a concrete backend at build time:
 *
 *   Linux   (src/kernel/exec_hash_linux.h)          -> macro-forwards every op
 *                                                      to <linux/hashtable.h>
 *                                                      (zero behaviour change).
 *   NetBSD  (src/kernel-netbsd/exec_hash_netbsd.h)  -> a real OVMX / hashinit(9)
 *                                                      implementation with the
 *                                                      same signatures, because
 *                                                      Linux hash_for_each and
 *                                                      BSD LIST/hashinit have
 *                                                      incompatible shapes and
 *                                                      CANNOT be macro-bridged
 *                                                      (design record §5 #2).
 *
 * WHY IT LANDS NOW: vms_proctab.c (the process table, rd vms-846b, Phase F) is
 * the first core facility to MOVE to src/kernel-core/ that walks the process
 * hash, so it must be written against `exec_hash_*` to be free of
 * <linux/hashtable.h>. Locks (Phase G, vms_lock.c) bring the rbtree + the
 * resource hash; this header is deliberately MINIMAL -- only the ops
 * vms_proctab.c actually calls -- and Phase G extends it if it needs more.
 *
 * SCOPE (Phase F, what vms_proctab.c calls): the node type, locked full-table
 * iteration (plain + delete-safe), and RCU-safe deletion. The PROCESS TABLE
 * ITSELF is declared/defined/inited in the per-substrate struct header + module
 * glue (DECLARE_HASHTABLE(vms_proc_hash,...) in the Linux vms_internal.h,
 * DEFINE_HASHTABLE + hash_init + hash_add_rcu in vms_module.c -- all Linux glue
 * that keeps raw primitives), so the core facility only references the extern
 * table by name and never spells a table macro. That split exists because the
 * process table has LOCKLESS RCU readers in the glue; its hash_add_rcu /
 * possible-key lookup live in that glue, not here.
 *
 * SCOPE (Phase G additions, what vms_lock.c calls): the lock manager's RESOURCE
 * hash (vms_res_hash) is the OPPOSITE case -- it has no lockless reader (every
 * walk is under vms_res_hash_lock), no file but vms_lock.c touches it, and its
 * add is facility logic on the enqueue path, not a module-lifecycle event. So
 * its whole NON-RCU table vocabulary lives HERE, in the shim, and the resource
 * hash lives wholly inside the core facility:
 *   EXEC_DECLARE_HASHTABLE / EXEC_DEFINE_HASHTABLE   declare / define a table.
 *   void  exec_hash_init(name)                       init an EXEC_DEFINE'd table.
 *   void  exec_hash_add(name, node, key)             insert `node` under `key`.
 *   exec_hash_for_each_possible(name, obj, member, key)
 *                                                    walk the `key` bucket, typed.
 *   void  exec_hash_del(exec_hash_node_t *n)         plain (non-RCU) unlink.
 *   uint32_t exec_jhash(const void *key, uint32_t len, uint32_t initval)
 *                                                    hash a byte range to a bucket
 *                                                    key (the resource-name hash;
 *                                                    Linux: jhash). Its VALUE is
 *                                                    used only for bucketing and a
 *                                                    membership modulo, never for a
 *                                                    correctness decision (name
 *                                                    matches are by strncmp), so a
 *                                                    substrate whose exec_jhash
 *                                                    differs is still correct.
 *
 * RCU PAIRING: exec_hash_del_rcu removes a node so that a read section started
 * AFTER the unlink cannot reach it while a reader ALREADY traversing walks off
 * cleanly; the object must then be reclaimed with exec_kbackend.h's
 * exec_free_deferred (a grace period), never freed synchronously. The two are
 * one idiom -- see the GRACE-PERIOD CONTRACT in exec_kbackend.h §6.
 *
 * Clean-room (CLAUDE.md Rule 8): the container API and semantics are OVMX's
 * own; the Linux backend maps them to the PUBLIC, documented <linux/hashtable.h>
 * API only. No Linux, NetBSD, or VSI/HPE source or binary is copied.
 *
 * ================================================================
 * THE OPS (contract; the backend header provides the concrete impl)
 * ================================================================
 *
 * Types (concrete per substrate):
 *   exec_hash_node_t   an intrusive link embedded in each element struct.
 *                      Linux: struct hlist_node.
 *
 * Deletion:
 *   void exec_hash_del_rcu(exec_hash_node_t *n)
 *       unlink `n` RCU-safely (see RCU PAIRING above). Linux: hash_del_rcu.
 *
 * Iteration (typed; `member` is the exec_hash_node_t field name in the element;
 * both run under the table's guard lock -- these are the LOCKED readers, not the
 * lockless RCU readers that live in the module glue):
 *   exec_hash_for_each(name, bkt, obj, member)
 *       walk every element of table `name`; `bkt` is an int cursor, `obj` a
 *       typed element pointer. Do NOT delete `obj`. Linux: hash_for_each.
 *   exec_hash_for_each_safe(name, bkt, tmp, obj, member)
 *       walk with a scratch cursor `tmp` (an exec_hash_node_t *) so the body MAY
 *       exec_hash_del_rcu(obj) mid-walk without corrupting it. Linux:
 *       hash_for_each_safe.
 *
 * These are macros, not functions, by necessity: they recover the element from
 * its embedded node with the caller's element type + `member` name
 * (container_of), which only a macro can see -- the same reason exec_list.h's
 * iterators, and the NetBSD backend's need to SHIP an implementation, exist.
 */

#ifndef OVMX_EXEC_HASH_H
#define OVMX_EXEC_HASH_H

/*
 * Backend selection -- identical scheme to exec_kbackend.h / exec_list.h.
 */
#if defined(OVMX_KBACKEND_NETBSD)
#  include "exec_hash_netbsd.h"
#elif defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__)
#  include "exec_hash_linux.h"
#else
#  error "exec_hash.h: no kernel backend selected (define OVMX_KBACKEND_LINUX or OVMX_KBACKEND_NETBSD)"
#endif

#endif /* OVMX_EXEC_HASH_H */
