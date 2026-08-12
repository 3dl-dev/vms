/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_linux.h - the LINUX realization of the OVMX executive
 * kernel-backend shim (rd vms-adb, epic vms-8e8).
 *
 * DO NOT include this directly -- include "exec_kbackend.h", which selects
 * this file on a Linux kernel build. See that header for the op contract,
 * especially the wait/wake (cv) contract.
 *
 * Every op here is a TRIVIAL FORWARDER to the exact Linux primitive the
 * executive already uses, so a facility converted onto the shim compiles to
 * byte-identical behaviour:
 *
 *   exec_lock_t          == spinlock_t
 *   exec_cv_t            == wait_queue_head_t
 *   exec_lock/unlock     -> spin_lock / spin_unlock
 *   exec_cv_wait         -> a faithful open-coding of wait_event_interruptible's
 *                           per-iteration body (prepare_to_wait / unlock /
 *                           schedule / lock / finish_wait)
 *   exec_cv_broadcast    -> wake_up_interruptible  (wake ALL, as the executive
 *                           does today via wake_up_interruptible)
 *   exec_copyin/out      -> copy_from_user / copy_to_user, normalized to
 *                           0 / EXEC_EFAULT
 *   exec_zalloc_atomic   -> kzalloc(n, GFP_ATOMIC)
 *   exec_zalloc          -> kzalloc(n, GFP_KERNEL)
 *   exec_alloc / exec_free -> kmalloc(n, GFP_KERNEL) / kfree
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only public, documented
 * Linux kernel APIs. No code is copied from the Linux source.
 */

#ifndef OVMX_EXEC_KBACKEND_LINUX_H
#define OVMX_EXEC_KBACKEND_LINUX_H

#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>   /* __user */
#include <linux/gfp.h>

/* ---- primitive types ---- */
typedef spinlock_t        exec_lock_t;
typedef wait_queue_head_t exec_cv_t;

/* ---- 1. locking ---- */
static inline void exec_lock_init(exec_lock_t *l)    { spin_lock_init(l); }
static inline void exec_lock(exec_lock_t *l)         { spin_lock(l); }
static inline void exec_unlock(exec_lock_t *l)       { spin_unlock(l); }
static inline void exec_lock_destroy(exec_lock_t *l) { (void)l; /* no-op on Linux */ }

/* ---- 2. wait / wake (cv-shaped; see exec_kbackend.h) ----
 *
 * exec_cv_wait is a faithful open-coding of what wait_event_interruptible
 * does on each loop iteration, so that the caller's
 *
 *     for (;;) { if (PRED) break; if (exec_cv_wait(cv,lk)) {...} }
 *
 * is behaviour-identical to wait_event_interruptible(*cv, PRED) on Linux:
 *
 *   - `lk` is held on entry. prepare_to_wait() enqueues us on `cv` and sets
 *     TASK_INTERRUPTIBLE WHILE `lk` IS STILL HELD -- so a waker holding the
 *     same `lk` cannot slip a signal past us before we are on the queue.
 *   - if a signal is already pending we do NOT sleep (matches wait_event's
 *     `if (___wait_is_interruptible && __int) break;` before schedule()).
 *   - otherwise we drop `lk`, schedule(), and re-acquire `lk`.
 *   - finish_wait() dequeues and restores TASK_RUNNING.
 *   - returns nonzero iff a signal is pending. The CALLER re-tests the
 *     predicate first (predicate has priority over the interrupt), exactly
 *     as wait_event checks `condition` before the interruptible test.
 */
static inline void exec_cv_init(exec_cv_t *cv) { init_waitqueue_head(cv); }

static inline int exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
{
	DEFINE_WAIT(__w);
	int intr;

	prepare_to_wait(cv, &__w, TASK_INTERRUPTIBLE);
	if (signal_pending(current)) {
		intr = 1;                 /* do not sleep with a signal pending */
	} else {
		spin_unlock(lk);
		schedule();
		spin_lock(lk);
		intr = signal_pending(current);
	}
	finish_wait(cv, &__w);
	return intr;
}

/* wake ONE / wake ALL. The executive's event flags wake ALL waiters
 * (wake_up_interruptible), so facilities use exec_cv_broadcast; exec_cv_signal
 * is provided for completeness of the contract. */
static inline void exec_cv_signal(exec_cv_t *cv)    { wake_up_interruptible_nr(cv, 1); }
static inline void exec_cv_broadcast(exec_cv_t *cv) { wake_up_interruptible(cv); }
static inline void exec_cv_destroy(exec_cv_t *cv)   { (void)cv; /* no-op on Linux */ }

/* ---- 3. user <-> kernel copy (normalized 0 / EXEC_EFAULT) ----
 * The __user annotation is re-applied here so a portable facility can pass a
 * plain pointer (the design record drops __user in the shared core). */
static inline int exec_copyin(void *kdst, const void *usrc, size_t n)
{
	return copy_from_user(kdst, (const void __user *)usrc, n) ? EXEC_EFAULT : 0;
}

static inline int exec_copyout(void *udst, const void *ksrc, size_t n)
{
	return copy_to_user((void __user *)udst, ksrc, n) ? EXEC_EFAULT : 0;
}

/* ---- 4. kernel memory ---- */
static inline void *exec_zalloc(size_t n)        { return kzalloc(n, GFP_KERNEL); }
static inline void *exec_zalloc_atomic(size_t n) { return kzalloc(n, GFP_ATOMIC); }
static inline void *exec_alloc(size_t n)         { return kmalloc(n, GFP_KERNEL); }
static inline void  exec_free(void *p)           { kfree(p); }

#endif /* OVMX_EXEC_KBACKEND_LINUX_H */
