/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_list_linux.h - the LINUX realization of the OVMX executive intrusive
 * list contract (rd vms-ec4, epic vms-8e8).
 *
 * DO NOT include this directly -- include "exec_list.h", which selects this
 * file on a Linux kernel build. See that header for the op contract.
 *
 * Every op here macro-forwards (or trivially inlines) to the EXACT
 * <linux/list.h> primitive the executive already used, so a facility converted
 * onto exec_list_* compiles to byte-identical behaviour:
 *
 *   exec_list_head_t / exec_list_node_t    == struct list_head
 *   EXEC_LIST_HEAD(name)                   -> LIST_HEAD(name)
 *   EXEC_LIST_HEAD_INIT(name)              -> LIST_HEAD_INIT(name)
 *   exec_list_head_init                    -> INIT_LIST_HEAD
 *   exec_list_add / exec_list_add_tail     -> list_add / list_add_tail
 *   exec_list_del                          -> list_del
 *   exec_list_move                         -> list_move
 *   exec_list_empty                        -> list_empty
 *   exec_list_first_entry                  -> list_first_entry
 *   exec_list_first_entry_or_null          -> list_first_entry_or_null
 *   exec_list_for_each_entry[_safe]        -> list_for_each_entry[_safe]
 *
 * The for_each_entry forms MUST stay macros: they resolve the element from its
 * embedded node with the caller's element type + member name (container_of),
 * which a function cannot see. This is the exact reason the NetBSD backend
 * (Phase C) has to SHIP an implementation rather than macro-bridge to TAILQ
 * (design record §5 caveat 2).
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only the public,
 * documented Linux <linux/list.h> API. No code is copied from the Linux source.
 */

#ifndef OVMX_EXEC_LIST_LINUX_H
#define OVMX_EXEC_LIST_LINUX_H

#include <linux/list.h>

/* ---- container types ---- */
typedef struct list_head exec_list_head_t;
typedef struct list_head exec_list_node_t;

/* ---- static definition / initialization ---- */
#define EXEC_LIST_HEAD(name)       LIST_HEAD(name)
#define EXEC_LIST_HEAD_INIT(name)  LIST_HEAD_INIT(name)
static inline void exec_list_head_init(exec_list_head_t *h) { INIT_LIST_HEAD(h); }

/* ---- mutation ---- */
static inline void exec_list_add(exec_list_node_t *n, exec_list_head_t *h)      { list_add(n, h); }
static inline void exec_list_add_tail(exec_list_node_t *n, exec_list_head_t *h) { list_add_tail(n, h); }
static inline void exec_list_del(exec_list_node_t *n)                           { list_del(n); }
static inline void exec_list_move(exec_list_node_t *n, exec_list_head_t *h)      { list_move(n, h); }

/* ---- query ---- */
static inline int exec_list_empty(const exec_list_head_t *h) { return list_empty(h); }

/* ---- first element (typed; UNDEFINED on an empty list -- see exec_list.h) ---- */
#define exec_list_first_entry(head, type, member) \
	list_first_entry(head, type, member)

/* ---- first element or NULL on empty (typed; see exec_list.h) ---- */
#define exec_list_first_entry_or_null(head, type, member) \
	list_first_entry_or_null(head, type, member)

/* ---- iteration (typed; `member` is the exec_list_node_t field name) ---- */
#define exec_list_for_each_entry(pos, head, member) \
	list_for_each_entry(pos, head, member)
#define exec_list_for_each_entry_safe(pos, n, head, member) \
	list_for_each_entry_safe(pos, n, head, member)

#endif /* OVMX_EXEC_LIST_LINUX_H */
