/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_list.h - the OVMX executive intrusive doubly-linked list contract
 * (rd vms-ec4, epic vms-8e8; design record docs/design-netbsd-executive-core.md
 * §5 caveat 2, §7).
 *
 * This is the SECOND shim seam, alongside exec_kbackend.h: an intrusive,
 * embedded-node, circular doubly-linked list whose API is shaped exactly like
 * the Linux <linux/list.h> the executive already uses, but living in the OVMX
 * `exec_list_*` namespace so a substrate-agnostic facility in src/kernel-core/
 * never names a `<linux/…>` type or macro. Like exec_kbackend.h it selects a
 * concrete backend at build time:
 *
 *   Linux   (src/kernel/exec_list_linux.h)         -> macro-forwards every op
 *                                                     to the kernel's list_*
 *                                                     (zero behaviour change).
 *   NetBSD  (src/kernel-netbsd/exec_list_netbsd.h, Phase C) -> a real OVMX
 *                                                     implementation (~150-250
 *                                                     LOC), because Linux
 *                                                     list_for_each_entry and
 *                                                     BSD TAILQ have
 *                                                     incompatible shapes and
 *                                                     CANNOT be macro-bridged
 *                                                     (design record §5 #2).
 *
 * WHY A SEPARATE HEADER, NOT typedefs IN exec_kbackend.h: the containers are the
 * one place the shim ships real code on NetBSD rather than a typedef, so they
 * carry their own contract. WHY THE INTERFACE LANDS NOW, BEFORE THE NetBSD IMPL:
 * event flags (the first facility to MOVE to src/kernel-core/, rd vms-ec4) needs
 * to be written against `exec_list_*` so its body is free of `<linux/list.h>`;
 * defining the interface here lets that conversion happen while the NetBSD
 * backend is still Phase C's concern. NO NetBSD code is added in this phase.
 *
 * Clean-room (CLAUDE.md Rule 8): the container API and its semantics are OVMX's
 * own; the Linux backend maps them to the PUBLIC, documented <linux/list.h>
 * API only. No Linux, NetBSD, or VSI/HPE source or binary is copied.
 *
 * ================================================================
 * THE OPS (contract; the backend header provides the concrete impl)
 * ================================================================
 *
 * Types (concrete per substrate):
 *   exec_list_head_t   a list anchor. Embed one in the owner struct (or define
 *                      a standalone anchor with EXEC_LIST_HEAD). Linux:
 *                      struct list_head.
 *   exec_list_node_t   an intrusive link embedded in each element struct.
 *                      Same concrete type as the head on Linux; kept distinct
 *                      in the contract so a reader knows which role a field
 *                      plays. Linux: struct list_head.
 *
 * Static definition / initialization of an anchor:
 *   EXEC_LIST_HEAD(name)        define a file/function-scope anchor, initialized
 *                               empty (self-linked). Linux: LIST_HEAD(name).
 *   EXEC_LIST_HEAD_INIT(name)   the initializer expression for an anchor named
 *                               `name` (for use inside a struct initializer).
 *   void exec_list_head_init(exec_list_head_t *)   runtime-init an anchor empty.
 *
 * Mutation (all O(1); node must NOT already be on a list for the adds):
 *   void exec_list_add(exec_list_node_t *n, exec_list_head_t *h)       push front
 *   void exec_list_add_tail(exec_list_node_t *n, exec_list_head_t *h)  push back
 *   void exec_list_del(exec_list_node_t *n)   unlink n from whatever list it is on
 *   void exec_list_move(exec_list_node_t *n, exec_list_head_t *h)
 *       unlink n from its current list and push it onto the FRONT of `h` --
 *       exactly exec_list_del(n) followed by exec_list_add(n, h). Used to drain a
 *       per-process list onto a local "doomed" anchor under a lock, then free the
 *       drained nodes after the lock is dropped (vms_mbx's release_all path).
 *       Linux: list_move.
 *
 * Query:
 *   int  exec_list_empty(const exec_list_head_t *h)   nonzero iff h has no nodes
 *
 * First element (typed; `member` is the exec_list_node_t field name):
 *   exec_list_first_entry(head, type, member)
 *       the first element of a NON-EMPTY list, as a typed `type *`. UNDEFINED on
 *       an empty list -- the caller MUST establish non-emptiness first (a prior
 *       exec_list_empty() test, as vms_ast's deliver path does). A macro for the
 *       same container_of reason the iterators are. Linux: list_first_entry.
 *   exec_list_first_entry_or_null(head, type, member)
 *       the first element as a typed `type *`, or NULL if the list is EMPTY --
 *       the safe form for a caller that dequeues under the guard lock and must
 *       cope with an empty queue (vms_mbx's read path, which loops until a
 *       message is present). A macro, same container_of reason. Linux:
 *       list_first_entry_or_null.
 *
 * Iteration (typed; `member` is the exec_list_node_t field name in the element):
 *   exec_list_for_each_entry(pos, head, member)
 *       walk each element; pos is a typed element pointer. Do NOT delete `pos`.
 *   exec_list_for_each_entry_safe(pos, n, head, member)
 *       walk with a scratch cursor `n` so the body MAY exec_list_del(pos) (and
 *       free it) without corrupting the walk.
 *
 * These two are macros, not functions, by necessity: they recover the element
 * from its embedded node using the caller's element type and `member` name
 * (container_of), which only a macro can see.
 */

#ifndef OVMX_EXEC_LIST_H
#define OVMX_EXEC_LIST_H

/*
 * Backend selection -- identical scheme to exec_kbackend.h. Each substrate's
 * build defines its own macro (OVMX_KBACKEND_LINUX via src/kernel/Makefile
 * ccflags; OVMX_KBACKEND_NETBSD via the NetBSD kmodule build in Phase C).
 * __linux__/__KERNEL__ are accepted as a fallback so a stock `make -C
 * src/kernel` still resolves the Linux backend.
 */
#if defined(OVMX_KBACKEND_NETBSD)
#  include "exec_list_netbsd.h"
#elif defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__)
#  include "exec_list_linux.h"
#else
#  error "exec_list.h: no kernel backend selected (define OVMX_KBACKEND_LINUX or OVMX_KBACKEND_NETBSD)"
#endif

#endif /* OVMX_EXEC_LIST_H */
