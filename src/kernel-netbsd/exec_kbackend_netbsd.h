/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_netbsd.h - the NetBSD realization of the OVMX executive
 * kernel-backend shim (rd vms-4b4, epic vms-8e8; design record
 * docs/design-netbsd-executive-core.md).
 *
 * DO NOT include this directly -- include "exec_kbackend.h", which selects this
 * file on a NetBSD kernel build (OVMX_KBACKEND_NETBSD is defined by the NetBSD
 * kmodule build). See that header for the op contract, ESPECIALLY the wait/wake
 * (cv) contract -- this backend is the one the contract was SHAPED for, since a
 * NetBSD kcondvar cannot cheaply emulate Linux wait_event and so the portable
 * facility waits like a condition variable.
 *
 * Every op maps the abstract vocabulary to a PUBLIC, documented NetBSD kernel
 * primitive:
 *
 *   exec_lock_t          == kmutex_t            (kmutex(9), IPL_NONE/adaptive)
 *   exec_cv_t            == kcondvar_t          (cv(9))
 *   exec_lock/unlock     -> mutex_enter / mutex_exit
 *   exec_lock_destroy    -> mutex_destroy       (NOT a no-op here: a kmutex owns
 *                                                resources and MUST be destroyed)
 *   exec_cv_wait         -> cv_wait_sig, which atomically drops the passed
 *                           kmutex, sleeps on the condvar, and re-acquires the
 *                           kmutex before returning -- exactly the cv contract's
 *                           "enqueue under lk, drop lk, sleep, re-acquire lk".
 *                           Returns nonzero (EINTR/ERESTART) iff interrupted.
 *   exec_cv_signal       -> cv_signal            (wake ONE)
 *   exec_cv_broadcast    -> cv_broadcast         (wake ALL, as the facility does)
 *   exec_copyin/out      -> in-kernel copies (memcpy); the framework did the
 *                           real user boundary crossing -- see COPY MODEL below
 *   exec_zalloc[_atomic] -> kmem_zalloc/alloc(KM_SLEEP/KM_NOSLEEP) + a size hdr
 *   exec_alloc / free    -> kmem_alloc / kmem_free(p, size), size recovered from
 *                           the header (kmem_free needs the length; kfree/Linux
 *                           does not -- this is the size-tracking the design
 *                           record §5 caveat 1 calls out).
 *
 * WHY THE CV WAIT IS LOST-WAKEUP-FREE HERE. The facility holds the guard kmutex
 * `lk` while it enqueues on the condvar and re-tests the predicate; cv_wait_sig
 * releases `lk` only AFTER the caller is on the condvar's sleep queue, and
 * re-acquires it before returning. A waker holding the SAME `lk` (which the
 * facility guarantees -- for a common cluster the guard is the cluster's own
 * lock, shared by set-side and wait-side) cannot mutate the flag word or
 * cv_broadcast until it holds `lk`, i.e. until the waiter is already enqueued.
 * So no wakeup can be lost. This is the whole reason the shim's wait op is
 * cv-shaped rather than Linux-wait_event-shaped.
 *
 * COPY MODEL -- READ THIS. The facility owns its copy: it calls exec_copyin at
 * entry and exec_copyout at exit on the `arg' pointer it was handed. On NetBSD
 * that pointer is NOT a user address -- it is the kernel buffer the cdevsw ioctl
 * framework already filled by copying the caller's _IOWR argument in (and which
 * it will copy back out to userspace after the driver returns). The single REAL
 * user<->kernel boundary crossing therefore happens ONCE, in the framework, at
 * the syscall edge; exec_copyin/exec_copyout here are in-kernel copies (memcpy)
 * between that framework buffer and the facility's stack locals. This is the
 * ONE place the NetBSD backend's copy op differs in MECHANISM from Linux's
 * (memcpy vs copy_*_user), and it is deliberate: it is the honest, idiomatic
 * NetBSD cdevsw integration (the alternative -- encoding the ioctls IOC_VOID to
 * force the raw user pointer through so exec_copyin could be a literal copyin --
 * fights the ABI and buys nothing, since the data still crosses the boundary
 * exactly once). No data is fabricated: a real copy of real caller data occurs;
 * a bad user address is rejected by the framework's copyin BEFORE the facility
 * ever runs, so exec_copyin here never faults. See vms_netbsd.c's dispatch.
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only public, documented
 * NetBSD kernel APIs (mutex(9), condvar(9), copy(9), kmem(9)). No NetBSD source
 * is copied.
 */

#ifndef OVMX_EXEC_KBACKEND_NETBSD_H
#define OVMX_EXEC_KBACKEND_NETBSD_H

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>     /* copyin, copyout, memset */
#include <sys/mutex.h>     /* kmutex_t, mutex_* , IPL_NONE */
#include <sys/condvar.h>   /* kcondvar_t, cv_* */
#include <sys/kmem.h>      /* kmem_alloc/zalloc/free, KM_SLEEP/KM_NOSLEEP */
#include <sys/proc.h>      /* struct proc, curproc, proc_find, p_pid (Phase F) */
#include <sys/kauth.h>     /* kauth_cred_get, kauth_authorize_generic (Phase F) */
#include <sys/errno.h>     /* EWOULDBLOCK, EINTR, ERESTART (Phase G cv timeout) */
#include <sys/atomic.h>    /* membar_producer / membar_consumer (vms-d61) */

/* ---- primitive types ---- */
typedef kmutex_t   exec_lock_t;
typedef kcondvar_t exec_cv_t;

/* ---- 1. locking ----
 * A plain, non-IPL mutual-exclusion lock: an adaptive kmutex at IPL_NONE, since
 * the executive's event-flag paths run only in process context (an ioctl), the
 * same class as the Linux backend's plain spin_lock. exec_lock_destroy is a
 * REAL destroy here (a kmutex must be torn down), unlike the Linux no-op. */
static __inline void
exec_lock_init(exec_lock_t *l)
{
	mutex_init(l, MUTEX_DEFAULT, IPL_NONE);
}

static __inline void
exec_lock(exec_lock_t *l)
{
	mutex_enter(l);
}

static __inline void
exec_unlock(exec_lock_t *l)
{
	mutex_exit(l);
}

static __inline void
exec_lock_destroy(exec_lock_t *l)
{
	mutex_destroy(l);
}

/* exec_trylock (Phase G): non-blocking acquire, nonzero iff acquired. NetBSD
 * mutex_tryenter returns nonzero on success, matching the contract. Type-checked
 * by the event-flag build; called only once locks join this module's SRCS
 * (rd vms-ff7). */
static __inline int
exec_trylock(exec_lock_t *l)
{
	return mutex_tryenter(l);
}

/* ---- 2. wait / wake (cv-shaped; see exec_kbackend.h) ---- */
static __inline void
exec_cv_init(exec_cv_t *cv)
{
	cv_init(cv, "vmsexec");
}

/*
 * exec_cv_wait - enqueue on `cv', drop `lk', sleep, re-acquire `lk'. cv_wait_sig
 * does all of that atomically w.r.t. `lk' and returns 0 on a normal wake or a
 * nonzero errno (EINTR/ERESTART) if a signal is pending. We return that value
 * directly: the caller re-tests its predicate first (predicate has priority),
 * and only treats a nonzero return as "interrupted" when the predicate is still
 * false -- matching the contract and the Linux backend.
 */
static __inline int
exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
{
	return cv_wait_sig(cv, lk);
}

/*
 * exec_cv_wait_timeout (Phase G) - exec_cv_wait with a bounded sleep. cv_timedwait_sig
 * atomically drops `lk', sleeps up to `timo' ticks, and re-acquires `lk', returning
 * EWOULDBLOCK on timeout, EINTR/ERESTART on a signal, or 0 on a cv wake. We map
 * EWOULDBLOCK to *timed_out (return 0, not an interrupt) and a signal to a nonzero
 * return, matching the Linux backend and the contract. mstohz floors at 1 tick so a
 * sub-tick `ms' never degenerates into cv_timedwait_sig's timo==0 "wait forever".
 * Type-checked by the event-flag build; called only once locks join this module's
 * SRCS (rd vms-ff7).
 */
static __inline int
exec_cv_wait_timeout(exec_cv_t *cv, exec_lock_t *lk, unsigned int ms, int *timed_out)
{
	int timo = mstohz(ms);
	int rv;

	if (timo < 1)
		timo = 1;
	*timed_out = 0;
	rv = cv_timedwait_sig(cv, lk, timo);
	if (rv == EWOULDBLOCK) {
		*timed_out = 1;
		return 0;
	}
	return (rv == EINTR || rv == ERESTART) ? 1 : 0;
}

static __inline void
exec_cv_signal(exec_cv_t *cv)
{
	cv_signal(cv);
}

static __inline void
exec_cv_broadcast(exec_cv_t *cv)
{
	cv_broadcast(cv);
}

static __inline void
exec_cv_destroy(exec_cv_t *cv)
{
	cv_destroy(cv);
}

/* ---- 3. user <-> kernel copy ----
 * In-kernel copies: the `arg' the facility hands us is the framework's kernel
 * buffer (the cdevsw path already crossed the user boundary for the _IOWR
 * argument), so the move is kernel<->kernel and cannot fault. We always return
 * 0. See the COPY MODEL note above for why this is the honest NetBSD mechanism.
 */
static __inline int
exec_copyin(void *kdst, const void *usrc, size_t n)
{
	memcpy(kdst, usrc, n);
	return 0;
}

static __inline int
exec_copyout(void *udst, const void *ksrc, size_t n)
{
	memcpy(udst, ksrc, n);
	return 0;
}

/* ---- 4. kernel memory ----
 * kmem_free needs the block size, which kfree()/Linux does not; so exec_alloc
 * prepends a small, max-aligned header carrying the TOTAL allocation size, and
 * exec_free recovers it. The header union forces 8-byte alignment so the
 * returned pointer is suitably aligned for any executive struct. */
union exec_alloc_hdr {
	size_t   size;      /* total bytes handed to kmem_alloc (header + payload) */
	uint64_t _a64;      /* alignment floors */
	void    *_aptr;
};

static __inline void *
__exec_alloc(size_t n, km_flag_t kf, int zero)
{
	size_t tot = sizeof(union exec_alloc_hdr) + n;
	union exec_alloc_hdr *h = kmem_alloc(tot, kf);

	if (h == NULL)
		return NULL;                    /* only possible under KM_NOSLEEP */
	h->size = tot;
	void *p = (void *)(h + 1);
	if (zero)
		memset(p, 0, n);
	return p;
}

static __inline void *
exec_zalloc(size_t n)
{
	return __exec_alloc(n, KM_SLEEP, 1);
}

static __inline void *
exec_zalloc_atomic(size_t n)
{
	return __exec_alloc(n, KM_NOSLEEP, 1);
}

static __inline void *
exec_alloc(size_t n)
{
	return __exec_alloc(n, KM_SLEEP, 0);
}

static __inline void
exec_free(void *p)
{
	union exec_alloc_hdr *h;

	if (p == NULL)
		return;
	h = (union exec_alloc_hdr *)p - 1;
	kmem_free(h, h->size);
}

/* ---- 5. host-task binding (Phase F; see exec_kbackend.h) ----
 *
 * The privilege gate and liveness handle map to documented NetBSD KPIs
 * (kauth(9), proc(9)). exec_current_is_privileged is the real "is superuser"
 * authorization -- the analogue of Linux capable(CAP_SYS_ADMIN). The liveness
 * handle is the process id; exec_task_alive/pin resolve it with proc_find(9).
 *
 * COMPILE STATUS: vms_proctab.c is not yet in this module's SRCS (only
 * vms_eflag.c is), so these are TYPE-CHECKED but never called on NetBSD. The
 * identity/liveness/privilege ops carry their real mapping; the ACCOUNTING read
 * is a compile-safe documented stub -- it deliberately touches NO struct proc
 * internals (whose exact field spellings the proctab-on-NetBSD/VAX proof, rd
 * vms-9dc, will bind) and returns zeros, naming the real sources in the
 * comment. That is the "stub that is clearly a real mapping" the phase allows;
 * accounting is not part of the design record's shim sketch and is never
 * fabricated on a live path. */
typedef struct { pid_t pid; } exec_task_ref_t;  /* PCB pid_ref: the process id */
struct exec_task_pin;                            /* opaque pinned-proc handle */
typedef struct exec_task_pin exec_task_pin_t;

static __inline int
exec_current_is_privileged(void)
{
	return kauth_authorize_generic(kauth_cred_get(),
	    KAUTH_GENERIC_ISSUSER, NULL) == 0;
}

/* exec_current_uid/gid (vms-31b): the real uid/gid of the current LWP's
 * credentials -- the kauth(9) twin of Linux from_kuid(current_uid()). Real
 * mapping (not a stub); type-checked here and live once devtab joins SRCS. */
static __inline uint32_t
exec_current_uid(void)
{
	return (uint32_t)kauth_cred_getuid(kauth_cred_get());
}
static __inline uint32_t
exec_current_gid(void)
{
	return (uint32_t)kauth_cred_getgid(kauth_cred_get());
}

static __inline int
exec_task_alive(exec_task_ref_t *ref)
{
	if (ref == NULL)
		return 0;
	return proc_find(ref->pid) != NULL;
}

static __inline exec_task_pin_t *
exec_task_pin(exec_task_ref_t *ref)
{
	if (ref == NULL)
		return NULL;
	/* vms-9dc: take a real proc reference (proc_find under proc_lock, hold
	 * it across the accounting read); the cast keeps the handle opaque. */
	return (exec_task_pin_t *)proc_find(ref->pid);
}

static __inline void
exec_task_read_acct(exec_task_pin_t *pin, struct exec_proc_acct *out)
{
	/*
	 * vms-9dc binds these to the real NetBSD sources on the pinned proc:
	 *   cpu_ns        <- calcru()/p_rusage user+system time
	 *   page_faults   <- p_stats->p_ru.ru_minflt + ru_majflt
	 *   create_wall_ns<- p_stats->p_start (a struct timeval, already wall)
	 *   rss_pages     <- p_vmspace->vm_rssize
	 * Until then, a compile-safe zero-fill (no struct proc deref, so no
	 * field-spelling risk on the live event-flag build). Never reached today.
	 */
	(void)pin;
	out->cpu_ns = 0;
	out->page_faults = 0;
	out->create_wall_ns = 0;
	out->rss_pages = 0;
	out->has_rss = 0;
}

static __inline void
exec_task_unpin(exec_task_pin_t *pin)
{
	(void)pin;   /* vms-9dc: release the proc reference taken by exec_task_pin */
}

/* ---- 6. RCU-lite deferred reclaim (Phase F; see exec_kbackend.h) ----
 * NetBSD has no lockless hash readers (the walks run under the hash kmutex --
 * design record §5 caveat 3's blessed fallback), so a read section is a no-op
 * and a deferred free runs immediately: by the time the object is unlinked and
 * exec_free_deferred is called, no reader can still hold it. */
typedef struct { void *_unused; } exec_rcu_head_t;

static __inline void exec_rcu_read_lock(void)   { /* no lockless readers */ }
static __inline void exec_rcu_read_unlock(void) { /* no lockless readers */ }

static __inline void
exec_free_deferred(exec_rcu_head_t *h, void (*fn)(exec_rcu_head_t *))
{
	fn(h);   /* immediate: safe, no grace period needed on this substrate */
}

/* ---- 7. sleepable mutex (Phase F; see exec_kbackend.h) ----
 * A process-context adaptive kmutex at IPL_NONE (same primitive class as
 * exec_lock_t here). No static initializer exists on NetBSD, so EXEC_DEFINE_MUTEX
 * declares storage that exec_mutex_init must initialize at runtime -- never
 * expanded on NetBSD today (its only user, vms_proctab.c, is Linux-built). */
typedef kmutex_t exec_mutex_t;

#define EXEC_DEFINE_MUTEX(name) exec_mutex_t name   /* + runtime exec_mutex_init */
static __inline void exec_mutex_init(exec_mutex_t *m)    { mutex_init(m, MUTEX_DEFAULT, IPL_NONE); }
static __inline void exec_mutex_lock(exec_mutex_t *m)    { mutex_enter(m); }
static __inline void exec_mutex_unlock(exec_mutex_t *m)  { mutex_exit(m); }
static __inline void exec_mutex_destroy(exec_mutex_t *m) { mutex_destroy(m); }

/* ---- 8. block-device resolution (vms-31b; see exec_kbackend.h) ----
 *
 * COMPILE STATUS, and why this side is a contract-only twin (the exec_rbtree
 * precedent). vms_devtab.c is NOT in this module's SRCS yet -- only vms_eflag.c
 * is -- so these are TYPE-CHECKED but never called on NetBSD. The MAJOR/MINOR
 * accessors carry their real mapping (major(9)/minor(9) are portable dev_t
 * accessors from <sys/types.h>, already included above). The PATH -> dev_t
 * RESOLUTION is the one piece with no cheap NetBSD one-liner: Linux lookup_bdev
 * walks the /dev name space, whereas the NetBSD twin resolves a device path to
 * a vnode (namei/lookup on the /dev node) and reads vp->v_rdev, or maps a device
 * NAME to its dev_t through the block devsw (bdevsw_lookup / devsw_name2blk).
 * Binding that -- and enumerating the node's disks the way the executive does on
 * Linux -- is the devtab-on-NetBSD proof's concern (a later item); until then
 * this is a compile-safe documented stub that touches NO struct internals and
 * reports "no such device", naming its real source here. It is never on a live
 * path (INV-6 / Rule 11: it fabricates nothing -- it resolves nothing). */
typedef dev_t exec_dev_t;

static __inline int
exec_blockdev_lookup(const char *path, exec_dev_t *out)
{
	/* vms-31b: bind to namei(vp->v_rdev) or devsw_name2blk on the NetBSD
	 * devtab proof (rd, later). Never reached today (devtab is Linux-built). */
	(void)path;
	(void)out;
	return -1;   /* no such device */
}
static __inline unsigned int exec_blockdev_major(exec_dev_t dev) { return (unsigned int)major(dev); }
static __inline unsigned int exec_blockdev_minor(exec_dev_t dev) { return (unsigned int)minor(dev); }

/* ---- 9. store/load memory barriers (vms-d61; see exec_kbackend.h) ----
 * Real mapping: membar_producer/membar_consumer are the portable NetBSD
 * store-store / load-load barriers (membar_ops(3), <sys/atomic.h> included
 * above) -- the exact twins of Linux smp_wmb/smp_rmb, and precisely
 * the fences a seqlock producer/consumer pair needs. Type-checked here; the
 * logical-name facility that calls exec_membar_producer (vms_lnm.c) is not in
 * this module's SRCS yet (only vms_eflag.c is), so the producer is never called
 * on NetBSD today, but the mapping is real, not a stub. */
static __inline void exec_membar_producer(void) { membar_producer(); }
static __inline void exec_membar_consumer(void) { membar_consumer(); }

/* ---- 10. userspace-publishable arena (vms-d61; see exec_kbackend.h) ----
 *
 * COMPILE STATUS, and why this side is a contract-only twin (the exec_rbtree /
 * exec_blockdev precedent). vms_lnm.c -- the only facility that allocates an
 * arena -- is NOT in this module's SRCS yet (only vms_eflag.c is), so these are
 * type-checked-at-most and never called on NetBSD. The REAL NetBSD binding is a
 * uvm anonymous object: exec_arena_alloc would uao_create(n, 0) and map its
 * pages into the kernel (uvm_map with UVM_ADV_NORMAL) to get a writable kernel
 * VA, taking a uao_reference the char-device mmap glue later hands to a process
 * with uvm_map + uvm_map_pageable read-only (the uvm twin of Linux
 * remap_vmalloc_range + clearing VM_MAYWRITE); exec_arena_free would uvm_unmap
 * the kernel range and uao_detach. Binding that -- and the mmap-time publish in
 * the NetBSD char-device rind -- is the lnm-on-NetBSD proof's concern (a later
 * item, following exec_blockdev). Until then this is a compile-safe documented
 * stub that touches no uvm internals and reports failure, naming its real
 * source here. It is never on a live path (INV-6 / Rule 11: it allocates
 * nothing and fabricates nothing -- it fails honestly). */
typedef void *exec_arena_t;
static __inline void *
exec_arena_alloc(size_t n)
{
	/* vms-d61: bind to uao_create(n,0) + uvm_map for a writable kernel VA on
	 * the lnm-on-NetBSD proof (rd, later). Never reached today (vms_lnm.c is
	 * not in this module's SRCS). */
	(void)n;
	return NULL;
}
static __inline void
exec_arena_free(void *arena)
{
	(void)arena;   /* vms-d61: uvm_unmap the kernel range + uao_detach. */
}

#endif /* OVMX_EXEC_KBACKEND_NETBSD_H */
