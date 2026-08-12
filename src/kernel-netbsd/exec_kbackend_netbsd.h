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
 *   exec_copyin/out      -> copyin / copyout, normalized to 0 / EXEC_EFAULT
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
 * COPY MODEL -- READ THIS. The facility owns its user<->kernel copy: it calls
 * exec_copyin at entry and exec_copyout at exit on the `arg' pointer it was
 * handed. For that to be a REAL copyin/copyout here, the NetBSD `vms'
 * pseudo-device MUST hand the facility a genuine USERSPACE pointer -- which it
 * does by encoding the event-flag ioctls with IOC_VOID (the _IO() form, size 0)
 * so NetBSD's generic ioctl path does NOT pre-copy the argument and instead
 * passes the raw user address through (see src/kernel-netbsd/vms_netbsd.c). The
 * facility's exec_copyin/out then perform the one real user boundary crossing,
 * exactly as on Linux. (An _IOWR encoding would make NetBSD pre-copy into a
 * kernel buffer, and exec_copyin would be a kernel memcpy of an
 * already-copied struct -- honest but a double copy; IOC_VOID keeps the single
 * real copy and matches the Linux backend's behaviour op-for-op.)
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

/* ---- 3. user <-> kernel copy (normalized 0 / EXEC_EFAULT) ----
 * NetBSD copyin(uaddr, kaddr, len) / copyout(kaddr, uaddr, len) already return
 * 0 / EFAULT; we only re-map EFAULT onto the shim's EXEC_EFAULT so a facility
 * never sees a substrate errno. The argument pointer is a genuine user address
 * (the driver uses IOC_VOID so NetBSD does not pre-copy) -- see the COPY MODEL
 * note above. */
static __inline int
exec_copyin(void *kdst, const void *usrc, size_t n)
{
	return copyin(usrc, kdst, n) ? EXEC_EFAULT : 0;
}

static __inline int
exec_copyout(void *udst, const void *ksrc, size_t n)
{
	return copyout(ksrc, udst, n) ? EXEC_EFAULT : 0;
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

#endif /* OVMX_EXEC_KBACKEND_NETBSD_H */
