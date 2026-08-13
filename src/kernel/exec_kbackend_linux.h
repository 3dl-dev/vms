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
#include <linux/jiffies.h>    /* msecs_to_jiffies (exec_cv_wait_timeout) */
/* Phase F (host-task / RCU-lite / sleepable mutex) backing headers. */
#include <linux/pid.h>            /* struct pid, pid_task, PIDTYPE_PID */
#include <linux/capability.h>     /* capable, CAP_SYS_ADMIN */
#include <linux/mutex.h>          /* struct mutex, DEFINE_MUTEX, mutex_* */
#include <linux/rcupdate.h>       /* rcu_read_lock/unlock, call_rcu, rcu_head */
#include <linux/sched/cputime.h>  /* task cputime fields */
#include <linux/sched/mm.h>       /* get_task_mm, mmput */
#include <linux/mm.h>             /* get_mm_rss */
#include <linux/timekeeping.h>    /* ktime_get_boottime_ns, ktime_get_real_ts64 */
/* vms-31b (exec_current_uid/gid + block-device resolution) backing headers. */
#include <linux/cred.h>           /* current_uid, current_gid */
#include <linux/uidgid.h>         /* from_kuid, from_kgid, init_user_ns */
#include <linux/blkdev.h>         /* lookup_bdev (resolve /dev/vdX to a dev_t) */
#include <linux/kdev_t.h>         /* MAJOR / MINOR */

/* ---- primitive types ---- */
typedef spinlock_t        exec_lock_t;
typedef wait_queue_head_t exec_cv_t;

/* ---- 1. locking ---- */
static inline void exec_lock_init(exec_lock_t *l)    { spin_lock_init(l); }
static inline void exec_lock(exec_lock_t *l)         { spin_lock(l); }
static inline void exec_unlock(exec_lock_t *l)       { spin_unlock(l); }
static inline void exec_lock_destroy(exec_lock_t *l) { (void)l; /* no-op on Linux */ }
/* exec_trylock: acquire without blocking. Nonzero iff acquired (Phase G; the
 * lock manager's deadlock walker takes other processes' lock-list locks with a
 * trylock to avoid an ABBA inversion). Linux: spin_trylock. */
static inline int exec_trylock(exec_lock_t *l)       { return spin_trylock(l); }

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

/* exec_cv_wait_timeout (Phase G) -- exec_cv_wait with a bounded sleep, the
 * faithful one-iteration expansion of wait_event_interruptible_TIMEOUT. Same cv
 * contract (caller holds `lk`, loops re-testing the predicate); additionally,
 * `*timed_out` is set nonzero iff the full `ms` elapsed with no wake (so the
 * caller can run a bounded re-scan on a real timeout, as enq_wait_sync does for
 * deadlock detection). Returns nonzero iff interrupted, exactly like
 * exec_cv_wait. Each call re-arms the full `ms`; that matches the executive's
 * former outer wait_event_interruptible_timeout re-arm, and is sound because the
 * sole waker (try_grant_waiters) always mutates the predicate before signalling,
 * so a wake with the predicate still false is not a normal event. */
static inline int exec_cv_wait_timeout(exec_cv_t *cv, exec_lock_t *lk,
				       unsigned int ms, int *timed_out)
{
	DEFINE_WAIT(__w);
	long ret;
	int intr;

	*timed_out = 0;
	prepare_to_wait(cv, &__w, TASK_INTERRUPTIBLE);
	if (signal_pending(current)) {
		intr = 1;                 /* do not sleep with a signal pending */
	} else {
		spin_unlock(lk);
		ret = schedule_timeout(msecs_to_jiffies(ms));
		spin_lock(lk);
		if (ret == 0)
			*timed_out = 1;   /* the full timeout elapsed, no wake */
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

/* ---- 5. host-task binding (Phase F; see exec_kbackend.h) ----
 * The opaque handle is the thread group's struct pid (task_tgid); the pinned
 * handle is a reference-counted struct task_struct. Both forward to exactly the
 * primitives vms_proctab.c used before this seam existed, so the module is
 * behaviour-identical. */
typedef struct pid          exec_task_ref_t;   /* PCB pid_ref: the tgid's pid */
typedef struct task_struct  exec_task_pin_t;   /* a pinned (referenced) task */

static inline int exec_current_is_privileged(void) { return capable(CAP_SYS_ADMIN); }

/* exec_current_uid/gid (vms-31b): the REAL uid/gid of `current`, mapped into the
 * initial user namespace -- exactly the reads the device table's caller_uic()
 * did before this seam existed, so the module is behaviour-identical. */
static inline uint32_t exec_current_uid(void)
{
	return (uint32_t)from_kuid(&init_user_ns, current_uid());
}
static inline uint32_t exec_current_gid(void)
{
	return (uint32_t)from_kgid(&init_user_ns, current_gid());
}

static inline int exec_task_alive(exec_task_ref_t *ref)
{
	struct task_struct *task;

	if (!ref)
		return 0;
	rcu_read_lock();
	task = pid_task(ref, PIDTYPE_PID);
	rcu_read_unlock();
	return task != NULL;
}

static inline exec_task_pin_t *exec_task_pin(exec_task_ref_t *ref)
{
	struct task_struct *task;

	if (!ref)
		return NULL;
	rcu_read_lock();
	task = pid_task(ref, PIDTYPE_PID);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	return task;
}

static inline void exec_task_unpin(exec_task_pin_t *pin) { put_task_struct(pin); }

/*
 * exec_task_read_acct - the exact reads vms_proctab.c's fill_proc_acct did,
 * moved behind the seam so the facility keeps only the VMS unit/field mapping.
 * cpu_ns = utime+stime (ns); page_faults = min+maj faults; create_wall_ns is
 * the boot-clock start converted to a Unix-epoch wall time via the current
 * boot/real pair; rss via get_task_mm/get_mm_rss (get_task_mm may sleep, so
 * this whole op may sleep and must run with no lock held). NUL of a kernel
 * thread's mm leaves has_rss 0, as before.
 */
static inline void exec_task_read_acct(exec_task_pin_t *pin,
				       struct exec_proc_acct *out)
{
	struct task_struct *task = pin;
	u64 created_boot_ns, now_boot_ns, wall_now_ns;
	struct timespec64 now_wall;
	struct mm_struct *mm;

	out->cpu_ns      = task->utime + task->stime;
	out->page_faults = (u64)task->min_flt + (u64)task->maj_flt;

	created_boot_ns = task->start_boottime;
	now_boot_ns     = ktime_get_boottime_ns();
	ktime_get_real_ts64(&now_wall);
	wall_now_ns     = (u64)now_wall.tv_sec * NSEC_PER_SEC + now_wall.tv_nsec;
	out->create_wall_ns = (now_boot_ns >= created_boot_ns)
				? wall_now_ns - (now_boot_ns - created_boot_ns)
				: wall_now_ns;

	out->rss_pages = 0;
	out->has_rss   = 0;
	mm = get_task_mm(task);
	if (mm) {
		out->rss_pages = (u64)get_mm_rss(mm);
		out->has_rss   = 1;
		mmput(mm);
	}
}

/* ---- 6. RCU-lite deferred reclaim (Phase F; see exec_kbackend.h) ---- */
typedef struct rcu_head exec_rcu_head_t;

static inline void exec_rcu_read_lock(void)   { rcu_read_lock(); }
static inline void exec_rcu_read_unlock(void) { rcu_read_unlock(); }
static inline void exec_free_deferred(exec_rcu_head_t *h,
				      void (*fn)(exec_rcu_head_t *))
{
	call_rcu(h, fn);
}

/* ---- 7. sleepable mutex (Phase F; see exec_kbackend.h) ---- */
typedef struct mutex exec_mutex_t;

#define EXEC_DEFINE_MUTEX(name) DEFINE_MUTEX(name)
static inline void exec_mutex_init(exec_mutex_t *m)    { mutex_init(m); }
static inline void exec_mutex_lock(exec_mutex_t *m)    { mutex_lock(m); }
static inline void exec_mutex_unlock(exec_mutex_t *m)  { mutex_unlock(m); }
static inline void exec_mutex_destroy(exec_mutex_t *m) { mutex_destroy(m); }

/* ---- 8. block-device resolution (vms-31b; see exec_kbackend.h) ----
 * Trivial forwarders to the exact block-layer primitives the device table used
 * before this seam existed: lookup_bdev() resolves a /dev path to a dev_t
 * without opening the device, and MAJOR()/MINOR() split it -- so the converted
 * vms_devtab.c compiles to byte-identical behaviour. */
typedef dev_t exec_dev_t;

static inline int exec_blockdev_lookup(const char *path, exec_dev_t *out)
{
	return lookup_bdev(path, out) ? -1 : 0;
}
static inline unsigned int exec_blockdev_major(exec_dev_t dev) { return MAJOR(dev); }
static inline unsigned int exec_blockdev_minor(exec_dev_t dev) { return MINOR(dev); }

#endif /* OVMX_EXEC_KBACKEND_LINUX_H */
