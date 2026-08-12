/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend.h - the OVMX executive kernel-backend shim (rd vms-adb,
 * epic vms-8e8; design record docs/design-netbsd-executive-core.md).
 *
 * This is the ONE header a substrate-agnostic executive facility includes
 * for host-kernel primitives. It declares an abstract vocabulary -- locking,
 * wait/wake, user<->kernel copy, and kernel allocation -- whose concrete
 * realization is selected at build time from a per-substrate backend header:
 *
 *   Linux   (src/kernel/exec_kbackend_linux.h)   -> spinlock_t / wait_queue /
 *                                                   copy_*_user / kmalloc
 *   NetBSD  (src/kernel-netbsd/exec_kbackend_netbsd.h, Phase C) -> kmutex(9) /
 *                                                   cv(9) / copyin/copyout /
 *                                                   kmem(9)
 *
 * The point of the shim is that on Linux every op expands to EXACTLY the
 * primitive the executive uses today, so promoting a facility onto it is a
 * behaviour-preserving refactor; the same facility source then compiles
 * against the NetBSD backend without a single `#if` in the facility itself.
 *
 * PHASE A SCOPE (this landing). Only the primitives the FIRST facility to be
 * converted -- event flags (src/kernel/vms_eflag.c) -- actually calls are
 * defined here: locking, wait/wake, copyin/out, kernel alloc/free. The
 * remaining shim surface named in the design record is deliberately NOT
 * introduced yet, each with the phase that needs it first:
 *
 *   - intrusive containers  exec_list.h / exec_hash.h / exec_rbtree.h  (Phase
 *     B/C: needed when a facility MOVES to src/kernel-core/ and its
 *     <linux/list.h> etc. must go behind the shim; eflag keeps raw list_*
 *     for now because it is not moving in Phase A).
 *   - host-task binding  exec_current_* / exec_task_*  (Phase F, vms_proctab):
 *     event flags touch ZERO host-task state, so none is needed here.
 *   - deferred free  exec_free_deferred / exec_rcu_*  (Phase F): eflag frees
 *     synchronously under a lock, no RCU.
 *
 * Clean-room (CLAUDE.md Rule 8): this contract and the facility logic are
 * OVMX's own code; each backend maps it to PUBLIC, documented host kernel
 * APIs only. No Linux, NetBSD, or VSI/HPE source or binary is copied.
 *
 * ================================================================
 * THE OPS (contract; the backend header provides the concrete impl)
 * ================================================================
 *
 * Types (concrete per substrate):
 *   exec_lock_t   a plain, non-IRQ mutual-exclusion lock.
 *                 Linux: spinlock_t.  NetBSD: kmutex_t at IPL_NONE.
 *   exec_cv_t     a wait/wake rendezvous object paired with an exec_lock_t.
 *                 Linux: wait_queue_head_t.  NetBSD: kcondvar_t.
 *
 * 1. Locking
 *   void exec_lock_init(exec_lock_t *)
 *   void exec_lock(exec_lock_t *)          acquire (blocks; no sleep held)
 *   void exec_unlock(exec_lock_t *)
 *   void exec_lock_destroy(exec_lock_t *)  no-op on Linux; mutex_destroy on NetBSD
 *
 * 2. Wait / wake  --  cv(9)-SHAPED CONTRACT.  READ THIS BEFORE USING.
 *
 *   void exec_cv_init(exec_cv_t *)
 *   int  exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
 *   void exec_cv_signal(exec_cv_t *)     wake ONE waiter
 *   void exec_cv_broadcast(exec_cv_t *)  wake ALL waiters
 *   void exec_cv_destroy(exec_cv_t *)
 *
 *   The wait path is shaped like a condition variable, NOT like Linux's
 *   lock-free wait_event, because Linux can faithfully emulate a condvar but
 *   NetBSD cannot cheaply emulate wait_event. The contract, which every
 *   caller MUST honour or risk a lost wakeup, is:
 *
 *     THE WAITER holds `lk` -- the SAME lock that guards the predicate and
 *     the cv -- across the whole wait, and loops re-testing the predicate:
 *
 *         exec_lock(&x->lk);
 *         for (;;) {
 *             if (PREDICATE) break;                 // predicate has priority
 *             if (exec_cv_wait(&x->cv, &x->lk)) {   // sleeps; nonzero=interrupted
 *                 if (PREDICATE) break;             // predicate STILL has priority
 *                 ... interrupted, predicate false: unlock and bail ...
 *             }
 *         }
 *         exec_unlock(&x->lk);
 *
 *     exec_cv_wait ENQUEUES the caller on `cv` while `lk` is still held, then
 *     atomically drops `lk` and sleeps, and re-acquires `lk` before it
 *     returns. It returns 0 on a normal wake and nonzero if a signal is
 *     pending (the caller decides what an interrupt means; predicate always
 *     wins over an interrupt, matching VMS: a wait ends when its flag is
 *     set, and a signal produces no wait status -- CLAUDE.md Rule 10).
 *
 *     THE WAKER holds the SAME `lk` while it mutates the predicate and
 *     signals:
 *
 *         exec_lock(&x->lk);
 *         x->ready = 1;                 // mutate the predicate
 *         exec_cv_broadcast(&x->cv);    // (or _signal) still under lk
 *         exec_unlock(&x->lk);
 *
 *   WHY THIS IS LOST-WAKEUP-FREE: the waiter enqueues on `cv` while holding
 *   `lk`; the waker cannot mutate the predicate or signal until it acquires
 *   `lk`, which the waiter does not release until it is already enqueued
 *   (inside exec_cv_wait). So any signal a waker sends is necessarily seen by
 *   an already-enqueued waiter, and any predicate mutation a waker makes is
 *   observed by the waiter's next re-test. The one and only requirement is
 *   that waiter and waker share `lk`. A wait/wake pair whose two sides use
 *   DIFFERENT locks is NOT covered by this contract and will lose wakeups.
 *
 * 3. User <-> kernel copy  (normalized: 0 = ok, EXEC_EFAULT = fault; note
 *    this differs from Linux copy_*_user, which returns bytes-not-copied --
 *    the backend does the normalization so a facility never sees it):
 *   int exec_copyin (void *kdst, const void *usrc, size_t n)
 *   int exec_copyout(void *udst, const void *ksrc, size_t n)
 *
 * 4. Kernel memory:
 *   void *exec_zalloc(size_t n)         zeroed; MAY SLEEP (process context)
 *   void *exec_zalloc_atomic(size_t n)  zeroed; will NOT sleep (safe while a
 *                                       lock is held) -- Linux GFP_ATOMIC,
 *                                       NetBSD KM_NOSLEEP. (Extends the design
 *                                       record's single exec_zalloc, which is
 *                                       may-sleep only; event flags allocate a
 *                                       common cluster while holding a lock and
 *                                       so need the non-sleeping flavour.)
 *   void *exec_alloc(size_t n)          uninitialized; may sleep
 *   void  exec_free(void *p)            free an exec_{z,}alloc/exec_zalloc_atomic
 *                                       block (the backend recovers the size
 *                                       where its free needs one).
 */

#ifndef OVMX_EXEC_KBACKEND_H
#define OVMX_EXEC_KBACKEND_H

/* Normalized copyin/copyout fault code (== EFAULT everywhere we target). */
#define EXEC_EFAULT 14

/*
 * Backend selection. Each substrate's build defines its own macro
 * (OVMX_KBACKEND_LINUX via src/kernel/Makefile ccflags; OVMX_KBACKEND_NETBSD
 * via the NetBSD kmodule build in Phase C). __linux__/__KERNEL__ are accepted
 * as a fallback so a stock `make -C src/kernel` still resolves the Linux
 * backend.
 */
#if defined(OVMX_KBACKEND_NETBSD)
#  include "exec_kbackend_netbsd.h"
#elif defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__)
#  include "exec_kbackend_linux.h"
#else
#  error "exec_kbackend.h: no kernel backend selected (define OVMX_KBACKEND_LINUX or OVMX_KBACKEND_NETBSD)"
#endif

#endif /* OVMX_EXEC_KBACKEND_H */
