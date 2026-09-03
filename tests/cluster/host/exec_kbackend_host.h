/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_host.h - the HOST (pthreads/malloc) realization of the OVMX
 * executive kernel-backend shim (rd FC-P4.9,
 * docs/plan-faithful-cluster-executive.md P4; design sec 3.9's rung-1 "host
 * unit" test ladder entry).
 *
 * DO NOT include this directly -- it is reached ONLY through the frozen
 * exec_kbackend.h -> exec_kbackend_linux.h chain, redirected to THIS file by
 * tests/cluster/host/lock_shim/exec_kbackend_linux.h (a 3-line forwarder; see
 * that file's header comment for why the redirection exists and why
 * exec_kbackend.h itself is never touched). See exec_kbackend.h for the op
 * contract, ESPECIALLY the wait/wake (cv) contract.
 *
 * SCOPE (deliberately minimal, same discipline exec_kbackend.h's own header
 * comment uses for every phase it lists: "each ... only because [the caller]
 * actually calls it"). This backend exists so src/kernel-core/vms_lock.c --
 * the ONLY facility that currently reaches exec_kbackend.h's locking/cv/
 * copy/alloc vocabulary directly (design sec 3.9's per-layer table: an
 * `_fsm.c` reaches exec_kbackend.h only for the container types, never these
 * ops -- it gets locking/timers through its OWN injected `ops` struct) --
 * compiles and LINKS on a plain host compiler. It implements exactly the
 * primitives vms_lock.c calls: locking (sec 1), wait/wake (sec 2), user<->
 * kernel copy (sec 3), and kernel memory (sec 4). Host-task binding (sec 5),
 * RCU-lite (sec 6), the sleepable mutex (sec 7), block-device resolution
 * (sec 8), the primary-NIC probe (sec 11), memory barriers (sec 9), the
 * publishable arena (sec 10), and the TCP/ICMP/L2 socket surfaces (sec 12-13)
 * are NOT implemented here -- nothing in vms_lock.c or (design sec 3.9's
 * per-layer table) an `_fsm.c` calls them, and fabricating host behaviour for
 * an unused primitive is the exact "toy hack that simulates ... but crumbles"
 * pattern this program's operator has ruled against. A later item that moves
 * a facility needing one of those onto this host backend extends this file
 * (the same phased-scope discipline the real Linux/NetBSD backends document).
 *
 *   exec_lock_t          == pthread_mutex_t
 *   exec_cv_t             == pthread_cond_t
 *   exec_lock/unlock      -> pthread_mutex_lock/unlock
 *   exec_trylock           -> pthread_mutex_trylock, normalized (Phase G's
 *                             deadlock walker's ABBA-avoiding probe)
 *   exec_cv_wait[_timeout] -> pthread_cond_wait / pthread_cond_timedwait,
 *                             the SAME cv contract exec_kbackend.h documents:
 *                             the caller holds `lk` across the call: pthread's
 *                             cond_wait already atomically drops+reacquires
 *                             the mutex, so the contract is met by
 *                             construction. A host unit test delivers no
 *                             signals, so these never report "interrupted"
 *                             (return 0 always) -- an honest reflection of
 *                             the host test environment, not a fabrication:
 *                             the real Linux/NetBSD backends report a signal
 *                             only when one is genuinely pending, and none
 *                             ever is here.
 *   exec_copyin/out        -> memcpy, normalized 0 / EXEC_EFAULT. A host unit
 *                             test has no separate user/kernel address space
 *                             (see exec_kbackend_netbsd.h's "COPY MODEL" note
 *                             for the same reasoning on that substrate): the
 *                             `arg` pointer vms_ioctl_enq()/_deq() receive IS
 *                             a real host pointer the test allocated, so the
 *                             copy is a REAL copy of REAL caller data (INV-6),
 *                             just with no privilege boundary to cross.
 *   exec_zalloc[_atomic]   -> calloc; exec_alloc/free -> malloc/free. The
 *                             atomic/may-sleep distinction is a kernel
 *                             scheduling-context concept with no host
 *                             analogue (a host test never runs "atomic"), so
 *                             both flavours share one calloc-based impl --
 *                             honestly documented here, not silently elided.
 *
 * Clean-room (CLAUDE.md Rule 8): these are OVMX's own primitives, mapped to
 * the PUBLIC, documented POSIX pthreads/libc API only. No Linux, NetBSD, or
 * VSI/HPE source or binary is copied.
 */

#ifndef OVMX_EXEC_KBACKEND_HOST_H
#define OVMX_EXEC_KBACKEND_HOST_H

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ---- primitive types ---- */
typedef pthread_mutex_t exec_lock_t;
typedef pthread_cond_t  exec_cv_t;

/* ---- 1. locking ---- */
static inline void exec_lock_init(exec_lock_t *l)    { pthread_mutex_init(l, NULL); }
static inline void exec_lock(exec_lock_t *l)         { pthread_mutex_lock(l); }
static inline void exec_unlock(exec_lock_t *l)       { pthread_mutex_unlock(l); }
static inline void exec_lock_destroy(exec_lock_t *l) { pthread_mutex_destroy(l); }
static inline int  exec_trylock(exec_lock_t *l)      { return pthread_mutex_trylock(l) == 0; }

/* ---- 2. wait / wake (cv-shaped; see exec_kbackend.h sec 2) ---- */
static inline void exec_cv_init(exec_cv_t *cv) { pthread_cond_init(cv, NULL); }

static inline int exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
{
	pthread_cond_wait(cv, lk);
	return 0;   /* no signal delivery in a host unit test -- never interrupted */
}

static inline int exec_cv_wait_timeout(exec_cv_t *cv, exec_lock_t *lk,
				       unsigned int ms, int *timed_out)
{
	struct timespec ts;
	int rc;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec  += (time_t)(ms / 1000u);
	ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec  += 1;
		ts.tv_nsec -= 1000000000L;
	}

	*timed_out = 0;
	rc = pthread_cond_timedwait(cv, lk, &ts);
	if (rc == ETIMEDOUT)
		*timed_out = 1;
	return 0;   /* no signal delivery in a host unit test -- never interrupted */
}

static inline void exec_cv_signal(exec_cv_t *cv)    { pthread_cond_signal(cv); }
static inline void exec_cv_broadcast(exec_cv_t *cv) { pthread_cond_broadcast(cv); }
static inline void exec_cv_destroy(exec_cv_t *cv)   { pthread_cond_destroy(cv); }

/* ---- 3. "user" <-> "kernel" copy (host: one address space; see file header
 * COPY MODEL note above) ---- */
static inline int exec_copyin(void *kdst, const void *usrc, size_t n)
{
	if (!kdst || !usrc)
		return EXEC_EFAULT;
	memcpy(kdst, usrc, n);
	return 0;
}

static inline int exec_copyout(void *udst, const void *ksrc, size_t n)
{
	if (!udst || !ksrc)
		return EXEC_EFAULT;
	memcpy(udst, ksrc, n);
	return 0;
}

/* ---- 4. memory ---- */
static inline void *exec_zalloc(size_t n)        { return calloc(1, n ? n : 1); }
static inline void *exec_zalloc_atomic(size_t n) { return calloc(1, n ? n : 1); }
static inline void *exec_alloc(size_t n)         { return malloc(n ? n : 1); }
static inline void  exec_free(void *p)           { free(p); }

#endif /* OVMX_EXEC_KBACKEND_HOST_H */
