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
#include <sys/resourcevar.h> /* calcru, struct pstats {p_ru, p_start} (vms-6cac).
                              * rbtree-CLEAN (pulls only sys/mutex.h + sys/resource.h),
                              * so it is safe in this shared header -- unlike
                              * <uvm/uvm_extern.h>, which pulls sys/rbtree.h and
                              * collides with exec_rbtree_netbsd.h (that is why the
                              * rss read is delegated to the dedicated uvm-TU
                              * vms_acct_rss_netbsd.c, rd vms-601). */
#include <sys/kauth.h>     /* kauth_cred_get, kauth_authorize_generic (Phase F) */
#include <sys/errno.h>     /* EWOULDBLOCK, EINTR, ERESTART (Phase G cv timeout) */
#include <sys/atomic.h>    /* membar_producer / membar_consumer (vms-d61) */
#include <sys/callout.h>   /* struct callout (exec_timer_t, SS16 -- FC-P0.1) */

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
 * ACCOUNTING (rd vms-6cac, epic vms-f62). vms_proctab.c IS in this module's
 * SRCS, so SHOW SYSTEM / $GETJPI reach exec_task_read_acct on every VAX row.
 * The read is now a REAL bind: exec_task_pin snapshots the pinned proc's CPU
 * time (calcru), page faults (p_ru.ru_minflt+ru_majflt) and start wall time
 * (p_start) under proc_lock -> p_lock and sets the matching has_* flags, so
 * fill_proc_acct lights the VMS_PI_V_CPUTIM/PAGEFLTS/LOGINTIM columns. The
 * resident-set count ("Pages") is read via a dedicated uvm-only TU
 * (vms_acct_rss_netbsd.c, rd vms-601): vm_resident_count() needs
 * <uvm/uvm_extern.h>, which pulls sys/rbtree.h and collides with
 * exec_rbtree_netbsd.h in this shared header, so it cannot be read inline here
 * (same reason vms_lnm_arena_netbsd.c is its own TU). A proc with no address
 * space leaves has_rss=0 -> the column is honestly OMITTED, never a fabricated
 * zero (INV-6). */
typedef struct { pid_t pid; } exec_task_ref_t;  /* PCB pid_ref: the process id */

/*
 * The pinned-proc handle is a SNAPSHOT of the accounting captured at pin time,
 * no longer an alias of struct proc. calcru() KASSERTs p_lock owned and the read
 * runs after vms_proc_hash_lock is dropped on a handle this backend does not
 * ref-hold, so exec_task_pin reads the values while the proc is provably alive
 * and stashes them here; exec_task_read_acct copies them out later (rd vms-6cac).
 */
struct exec_task_pin {
	uint64_t cpu_ns;         /* user+system CPU time, ns (has_cpu) */
	uint64_t page_faults;    /* ru_minflt + ru_majflt (has_faults) */
	uint64_t create_wall_ns; /* p_start wall time, ns since Unix epoch (has_create) */
	uint64_t rss_pages;      /* resident pages via the uvm-TU (has_rss; vms-601) */
	int      has_cpu;
	int      has_faults;
	int      has_create;
	int      has_rss;
};
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
	int alive;

	if (ref == NULL)
		return 0;
	/*
	 * proc_find(9) REQUIRES proc_lock held: proc_find_internal() asserts
	 * KASSERT(mutex_owned(&proc_lock)) and reads the global pid_table.
	 * Calling it lock-free races the pid_table against concurrent fork/exit
	 * on another CPU and can spuriously return NULL for a still-live process
	 * -- which made vms_proc_reap_dead() reap a running, $SETPRN-named
	 * process between one reader's $GETJPI (which saw it) and the next
	 * reader's $PROCESS_SCAN (which then could not enumerate it): rd vms-f8a,
	 * the P4-A proctab flake. The Linux backend already takes the equivalent
	 * read-side lock (rcu_read_lock around pid_task); match it here so
	 * liveness is reliable and INV-DRIFT holds (thin-backend specifics only).
	 */
	mutex_enter(&proc_lock);
	alive = (proc_find(ref->pid) != NULL);
	mutex_exit(&proc_lock);
	return alive;
}

/*
 * ovmx_task_rss_pages (rd vms-601): read the resident-set size (pages) of a
 * pinned proc, p->p_vmspace->vm_rssize. DEFINED in the dedicated uvm-only TU
 * vms_acct_rss_netbsd.c -- <uvm/uvm_extern.h> cannot be included in this shared
 * header (it pulls <sys/rbtree.h>, whose rb_left/rb_right macros collide with
 * exec_rbtree_netbsd.h). The caller MUST hold p->p_lock. Returns 1 and sets
 * *pages_out when the proc has an address space; returns 0 (leaving *pages_out
 * untouched) for a kernel thread / mid-exit proc with no vmspace -> the "Pages"
 * column is then honestly omitted, never a fabricated 0.
 */
extern int ovmx_task_rss_pages(struct proc *p, uint64_t *pages_out);

/*
 * ovmx_sysmem_bytes (rd vms-a3cd): read the system-wide physical memory totals
 * SHOW MEMORY's "Physical Memory Usage" section reports -- total managed memory
 * and current free memory, in BYTES. DEFINED in the dedicated uvm-only TU
 * vms_sysmem_netbsd.c (<uvm/uvm_extern.h> cannot be included in this shared
 * header -- rbtree macro collision, same as ovmx_task_rss_pages). Needs no proc
 * and no lock: it reads the global uvmexp counters via the maintained accessor
 * uvm_availmem(true). Returns 1 and sets both out-params when uvm is
 * up; returns 0 (leaving them untouched) before uvm init -> the Physical Memory
 * section is then honestly omitted, never a fabricated 0.
 */
extern int ovmx_sysmem_bytes(uint64_t *total_bytes, uint64_t *free_bytes);

static __inline exec_task_pin_t *
exec_task_pin(exec_task_ref_t *ref)
{
	exec_task_pin_t *pin;
	struct proc *p;

	if (ref == NULL)
		return NULL;
	/*
	 * Allocate the snapshot BEFORE proc_lock. exec_task_pin runs under the
	 * caller's vms_proc_hash_lock and MUST NOT sleep (vms_proctab.c), so use the
	 * atomic (KM_NOSLEEP) allocator; a NULL here degrades to honest omission
	 * (exec_task_read_acct sees NULL -> all has_*=0), never a fabricated zero.
	 */
	pin = exec_zalloc_atomic(sizeof(*pin));
	if (pin == NULL)
		return NULL;
	/*
	 * Snapshot the real accounting while the proc is provably alive. proc_lock
	 * -> p_lock is the NetBSD lock order; the caller already holds
	 * vms_proc_hash_lock, so the full nesting is hash_lock -> proc_lock ->
	 * p_lock, a clean deepening of the proc_lock lookup exec_task_alive/pin
	 * already did. calcru() KASSERTs p_lock owned and mutates the proc, so the
	 * read MUST run here, not after the locks drop -- that seam is exactly what
	 * this bind closes (rd vms-6cac/vms-f62). Field spellings verified against
	 * the vendored NetBSD headers (sys/resourcevar.h, sys/resource.h).
	 */
	mutex_enter(&proc_lock);
	p = proc_find(ref->pid);
	if (p != NULL && p->p_stats != NULL) {
		struct rusage ru;

		/*
		 * Replicate NetBSD's getrusage(RUSAGE_SELF) aggregation
		 * (kern_resource.c getrusage1): p_ru holds rusage accumulated from
		 * ALREADY-EXITED LWPs only; calcru() overlays the real total CPU time,
		 * and rulwps() adds the RUNNING LWPs' l_ru. This is essential for the
		 * fault count: page faults are tallied per-LWP (uvm_fault.c does
		 * curlwp->l_ru.ru_minflt++), so p_ru.ru_minflt alone reads ~0 for a live
		 * process -- rulwps() is what rolls in the live counts. calcru() and
		 * rulwps() both KASSERT p_lock owned (which we hold); rulwps/ruadd are
		 * declared in <sys/resourcevar.h>.
		 */
		mutex_enter(p->p_lock);
		memcpy(&ru, &p->p_stats->p_ru, sizeof(ru));
		calcru(p, &ru.ru_utime, &ru.ru_stime, NULL, NULL);
		rulwps(p, &ru);
		pin->create_wall_ns =
		    (uint64_t)p->p_stats->p_start.tv_sec * 1000000000ULL
		    + (uint64_t)p->p_stats->p_start.tv_usec * 1000ULL;
		/*
		 * rss (resident pages -> SHOW SYSTEM "Pages" and SHOW WORKING_SET size)
		 * is p->p_vmspace->vm_rssize, which lives behind <uvm/uvm_extern.h> --
		 * that pulls <sys/rbtree.h> whose rb_left/rb_right macros collide with
		 * exec_rbtree_netbsd.h in this shared header. So the read is delegated to a
		 * dedicated uvm-only TU (vms_acct_rss_netbsd.c, the vms_lnm_arena pattern);
		 * ovmx_task_rss_pages() reads vm_rssize under the p_lock we hold and returns
		 * 0 for a proc with no address space (kernel thread / mid-exit) -> honest
		 * omission, never a fabricated 0 (rd vms-601).
		 */
		if (ovmx_task_rss_pages(p, &pin->rss_pages))
			pin->has_rss = 1;
		mutex_exit(p->p_lock);

		pin->cpu_ns = ((uint64_t)ru.ru_utime.tv_sec + (uint64_t)ru.ru_stime.tv_sec)
		                  * 1000000000ULL
		            + ((uint64_t)ru.ru_utime.tv_usec + (uint64_t)ru.ru_stime.tv_usec)
		                  * 1000ULL;
		pin->has_cpu = 1;
		pin->page_faults = (uint64_t)ru.ru_minflt + (uint64_t)ru.ru_majflt;
		pin->has_faults = 1;
		pin->has_create = 1;
	}
	/*
	 * p == NULL: the process exited between the hash snapshot and here -- leave
	 * every has_*=0 so fill_proc_acct blanks its columns (honest omission), not a
	 * fabricated zero. rss_pages/has_rss also stay 0 here: with no proc there is
	 * no address space to read vm_resident_count() from (rd vms-601).
	 */
	mutex_exit(&proc_lock);
	return pin;
}

static __inline void
exec_task_read_acct(exec_task_pin_t *pin, struct exec_proc_acct *out)
{
	/*
	 * Copy the snapshot exec_task_pin captured under proc_lock -> p_lock. A NULL
	 * pin (atomic allocation failed, or the proc had already exited at pin time)
	 * yields all-zero with every has_*=0, so fill_proc_acct (src/kernel-core/
	 * vms_proctab.c) blanks the VAX SHOW SYSTEM CPU/Page-flts/Pages columns
	 * rather than printing a fabricated 0 -- the INV-6 honest omission #887
	 * established, now backed by the real bind (rd vms-6cac/vms-601). rss/has_rss
	 * come from the uvm-TU; a proc with no address space still omits "Pages".
	 */
	if (pin == NULL) {
		memset(out, 0, sizeof(*out));
		return;
	}
	out->cpu_ns = pin->cpu_ns;
	out->has_cpu = pin->has_cpu;
	out->page_faults = pin->page_faults;
	out->has_faults = pin->has_faults;
	out->create_wall_ns = pin->create_wall_ns;
	out->has_create = pin->has_create;
	out->rss_pages = pin->rss_pages;
	out->has_rss = pin->has_rss;
}

static __inline void
exec_task_unpin(exec_task_pin_t *pin)
{
	/*
	 * Free the snapshot exec_task_pin allocated (rd vms-6cac). No proc reference
	 * is held -- the accounting was captured under proc_lock at pin time -- so
	 * there is nothing else to drop. exec_free tolerates NULL.
	 */
	exec_free(pin);
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
 * exec_lock_t here).
 *
 * THE FILE-STATIC INIT PROBLEM, and why exec_mutex_t is a struct (rd vms-ca7).
 * Linux's DEFINE_MUTEX statically initializes; a NetBSD kmutex has NO static
 * initializer -- it must be mutex_init'd at runtime. The one EXEC_DEFINE_MUTEX
 * user, vms_proctab.c, declares its reap serializer as a FILE-STATIC
 * (`static EXEC_DEFINE_MUTEX(vms_reap_mutex)'), invisible to the module glue,
 * and the shared facility is byte-identical across substrates (INV-DRIFT) so it
 * carries no NetBSD-only init hook. So the mutex must initialize ITSELF on first
 * use. exec_mutex_t therefore pairs the kmutex with a small state word and
 * exec_mutex_lock lazily mutex_init's it exactly once (atomic CAS elects the one
 * initializer; a concurrent first-caller briefly spins until it is ready).
 * First use is at the first reap -- process context, long after module attach --
 * so a lock held here may sleep, exactly the sleepable-mutex contract. An
 * explicitly exec_mutex_init'd mutex marks itself ready and skips the lazy path.
 * This is idiomatic once-init (the RUN_ONCE(9) pattern, hand-rolled per-instance
 * because RUN_ONCE's init callback takes no cookie); no NetBSD source is copied. */
typedef struct exec_mutex {
	kmutex_t              mtx;
	volatile unsigned int st;   /* 0 = uninit, 1 = initializing, 2 = ready */
} exec_mutex_t;

/* File-scope definition: storage zero-initialized (st == 0 => not yet mutex_init'd;
 * the embedded kmutex is untouched until the lazy init below runs). */
#define EXEC_DEFINE_MUTEX(name) exec_mutex_t name = { .st = 0 }

static __inline void
exec_mutex_init(exec_mutex_t *m)
{
	mutex_init(&m->mtx, MUTEX_DEFAULT, IPL_NONE);
	atomic_store_release(&m->st, 2);
}

static __inline void
exec_mutex_ensure(exec_mutex_t *m)
{
	if (__predict_true(atomic_load_acquire(&m->st) == 2))
		return;
	if (atomic_cas_uint(&m->st, 0, 1) == 0) {
		/* Won the election: this caller performs the one-time init. */
		mutex_init(&m->mtx, MUTEX_DEFAULT, IPL_NONE);
		atomic_store_release(&m->st, 2);
	} else {
		/* Another caller is initializing; wait it out (it is runnable and the
		 * init is a handful of instructions -- a bounded spin). */
		while (atomic_load_acquire(&m->st) != 2)
			continue;
	}
}

static __inline void exec_mutex_lock(exec_mutex_t *m)    { exec_mutex_ensure(m); mutex_enter(&m->mtx); }
static __inline void exec_mutex_unlock(exec_mutex_t *m)  { mutex_exit(&m->mtx); }
static __inline void exec_mutex_destroy(exec_mutex_t *m)
{
	if (atomic_load_acquire(&m->st) == 2) {
		mutex_destroy(&m->mtx);
		atomic_store_release(&m->st, 0);
	}
}

/* ---- 8. block-device resolution (vms-31b; see exec_kbackend.h) ----
 *
 * The MAJOR/MINOR
 * accessors carry their real mapping (major(9)/minor(9) are portable dev_t
 * accessors from <sys/types.h>, already included above). The PATH -> dev_t
 * RESOLUTION is the one piece with no cheap NetBSD one-liner: Linux lookup_bdev
 * walks the /dev name space, whereas the NetBSD twin resolves a device path to
 * a vnode (namei/lookup on the /dev node) and reads vp->v_rdev, or maps a device
 * NAME to its dev_t through the block devsw (bdevsw_lookup / devsw_name2blk).
 * STATUS AFTER THE DEVTAB PORT (rd vms-618). The device table IS in this
 * module's SRCS now, so vms_devtab_probe_disks() DOES call this -- 26 times, for
 * /dev/vda../dev/vdz. Every one of those correctly resolves NOTHING, because
 * that is the LINUX virtio-blk name space and it does not exist on NetBSD/vax:
 * this node's disks are MSCP units. The executive therefore enters no unit from
 * this path, and the substrate enters its OWN units by their real device-native
 * names through vms_blockdev_netbsd_register_units() ->  vms_devtab_add_disk(),
 * which opens the real block device to read its dev_t. So this remains an honest
 * "no such device" answer rather than a fabricated one (INV-6 / Rule 11): it is
 * reached, and it resolves nothing, which is the truth about /dev/vda here.
 * Binding it to namei(vp->v_rdev) / devsw_name2blk would let the shared probe
 * answer for a NetBSD name space, which no caller wants today. */
typedef dev_t exec_dev_t;

static __inline int
exec_blockdev_lookup(const char *path, exec_dev_t *out)
{
	/* Reached (vms-618) but never resolves: `path' is always a Linux
	 * /dev/vd? name, which this substrate does not have. See above. */
	(void)path;
	(void)out;
	return -1;   /* no such device */
}
static __inline unsigned int exec_blockdev_major(exec_dev_t dev) { return (unsigned int)major(dev); }
static __inline unsigned int exec_blockdev_minor(exec_dev_t dev) { return (unsigned int)minor(dev); }

/* Block I/O seam (exec_kbackend.h S8). Under OVMX_ODS2_KERNEL -- the build that
 * links the Files-11 ODS-2 ACP (kernel-core/vmsfs_acp.c) into this module
 * (vms-d5d) -- the REAL NetBSD binding lives in vms_blockdev_netbsd.c
 * (vn_bdev_openpath-cached device vnode + bread(9) / getblk(9)+bwrite(9), the
 * same primitives the sibling vmsfs.kmod uses). Declared here so the ACP core
 * links against them. Without OVMX_ODS2_KERNEL the ACP is not built in and
 * nothing calls these, so the contract-only stub below stands (INV-6 / Rule 11:
 * it reads/writes nothing and fabricates nothing). */
#if defined(OVMX_ODS2_KERNEL)
int exec_blockdev_read_block(unsigned int major_, unsigned int minor_,
			     uint64_t lbn, void *buf, size_t buflen);
int exec_blockdev_write_block(unsigned int major_, unsigned int minor_,
			      uint64_t lbn, const void *buf, size_t buflen);
#else
/* READ one 512-byte block off a backing block device (vms-127; see
 * exec_kbackend.h). CONTRACT-ONLY TWIN, the exec_blockdev_lookup precedent
 * above: the only caller (the Files-11 ODS-2 ACP $MOUNT in kernel-core/
 * vmsfs_acp.c) is NOT in this module's SRCS yet, so this is type-checked at most
 * and never run on NetBSD. The REAL NetBSD binding opens the block device by
 * dev_t (bdevsw_lookup + the devsw d_open) and reads the block through the buffer
 * cache (bread(9) on the device vnode, brelse(9)) -- the NetBSD twins of Linux
 * bdev_open_by_dev + __bread. Binding that is the same devtab-on-NetBSD proof's
 * concern; until then this compile-safe stub touches no device internals and
 * reports failure, naming its real source here (INV-6 / Rule 11: it reads
 * nothing and fabricates nothing). */
static __inline int
exec_blockdev_read_block(unsigned int major_, unsigned int minor_,
			 uint64_t lbn, void *buf, size_t buflen)
{
	(void)major_;
	(void)minor_;
	(void)lbn;
	(void)buf;
	(void)buflen;
	return -1;   /* not readable (contract-only twin) */
}

/* WRITE one 512-byte block to a backing block device (vms-c60; the write twin of
 * the read stub above and its contract-only NetBSD twin, for the same reason:
 * the only caller, the Files-11 ODS-2 ACP IO$_WRITEVBLK / implicit-extend in
 * kernel-core/vmsfs_acp.c, is not in this module's SRCS yet, so this is
 * type-checked at most and never run on NetBSD). The REAL NetBSD binding opens
 * the block device by dev_t (bdevsw_lookup + the devsw d_open) and commits the
 * block through the buffer cache (bwrite(9) on a getblk(9)'d device buffer) --
 * the NetBSD twin of Linux bdev_open_by_dev + a REQ_OP_WRITE bio. Until devtab
 * lands on NetBSD this compile-safe stub touches no device internals and reports
 * failure, naming its real source here (INV-6 / Rule 11: it writes nothing and
 * fabricates nothing). */
static __inline int
exec_blockdev_write_block(unsigned int major_, unsigned int minor_,
			  uint64_t lbn, const void *buf, size_t buflen)
{
	(void)major_;
	(void)minor_;
	(void)lbn;
	(void)buf;
	(void)buflen;
	return -1;   /* not writable (contract-only twin) */
}

#endif /* !OVMX_ODS2_KERNEL (block I/O seam) */

/* ---- 11. primary Ethernet net device (vms-9d2; see exec_kbackend.h) ----
 *
 * STATUS AFTER THE DEVTAB PORT (rd vms-618). The device table (vms_devtab.c),
 * the ONLY caller, IS in this module's SRCS now, so vms_devtab_probe_nic() DOES
 * call this once at module init. It answers "no NIC", which the executive
 * handles by entering NO ETH0: unit at all -- so SHOW DEVICE has no ETH0: row
 * and $ASSIGN/$ALLOC ETH0: is SS$_NOSUCHDEV. That is the honest "this node has
 * no ENUMERATED Ethernet controller" state, not a fake device (INV-6): the VAX
 * under SIMH may well have a DEQNA, but this backend does not yet ask the ifnet
 * list, and reporting a unit it never looked up would be the fabrication.
 * Binding it is a later item (the VAX networking lane). The REAL NetBSD binding
 * is the generic ifnet list: IFNET_LOCK() /
 * IFNET_READER_FOREACH(ifp) over the interface list, skipping ifp->if_type ==
 * IFT_LOOP and requiring IFT_ETHER, copying ifp->if_xname and reading the link
 * state through if_link_state (LINK_STATE_UP) -- the exact NetBSD twins of
 * Linux for_each_netdev / ARPHRD_ETHER / netif_carrier_ok, and just as
 * driver-agnostic. Binding that -- and registering ETH0: on NetBSD -- is the
 * devtab-on-NetBSD proof's concern (a later item, following exec_blockdev).
 * Until then this is a compile-safe documented stub that touches no ifnet
 * internals and reports "no such device", naming its real source here. It is
 * never on a live path (INV-6 / Rule 11: it fabricates nothing). */
static __inline int
exec_netdev_primary(char *name, unsigned int namesz, int *link_up)
{
	/* vms-9d2: bind to IFNET_READER_FOREACH(ifp) filtered on IFT_ETHER when
	 * the VAX networking lane needs ETH0:. Reached once per module load
	 * (vms-618); answers "no NIC", so no unit is entered. See above. */
	(void)name;
	(void)namesz;
	(void)link_up;
	return -1;   /* no such device */
}

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

/* ---- 10. userspace-publishable arena (vms-d61 contract; vms-72da binding) ----
 *
 * BOUND on NetBSD as of vms-72da (lnm joined the module's SRCS). The arena is
 * physically-backed, WIRED kernel memory: uvm_km_alloc(kernel_map, ...,
 * UVM_KMF_WIRED | UVM_KMF_ZERO) returns a zeroed, page-aligned kernel VA whose
 * pages are real RAM that never pages out -- so the char device's d_mmap can
 * resolve each page to a physical frame with pmap_extract() and publish it
 * read-only into a process (the standard NetBSD /dev/mem idiom: d_mmap returns
 * atop(pa) per page and the device pager reconstructs the frame via
 * pmap_phys_address). It is the real thing (INV-6 / Rule 11): genuine shared
 * kernel state, one writer (the executive), MMU-enforced RO for readers.
 *
 * WHY THESE ARE EXTERN, NOT INLINE (unlike every other op in this backend). The
 * uvm KPIs the arena needs (uvm_km_alloc/free, kernel_map, pmap_extract) live
 * behind <uvm/uvm_extern.h>, which transitively pulls <sys/rbtree.h> -- whose
 * rb_left/rb_right MACROS collide with OVMX's intrusive exec_rbtree_netbsd.h
 * (the DLM's lock-ID tree), a header EVERY executive TU also includes via
 * vms_internal.h. So the uvm-coupled definitions cannot live in a header shared
 * with the rbtree; they live in a DEDICATED glue TU that includes uvm but NOT
 * the executive's rbtree headers: src/kernel-netbsd/vms_lnm_arena_netbsd.c. This
 * is the MMAP-glue-stays-in-the-rind rule (design §2) taken one step further:
 * the arena's host-mm coupling is quarantined in its own rind TU. The signatures
 * here carry no uvm type, so this declaration needs no uvm header. */
typedef void *exec_arena_t;
void *exec_arena_alloc(size_t n);
void  exec_arena_free(void *arena);

/* ================================================================
 * §12  Host TCP client socket (vms-9951; BGn: INET facility).
 *
 * Substrate-neutrality anchor for the exec_socket_* seam: these declarations
 * prove the §12 contract (exec_kbackend.h) is expressible in NetBSD terms with
 * no Linux type leaking across the seam -- addresses are raw net-order scalars,
 * the handle is an opaque pointer, no struct socket / struct sockaddr in the
 * signatures. exec_socket_t wraps NetBSD's in-kernel `struct socket` (socreate/
 * soconnect/sosend/soreceive/soshutdown, socket(9)) with a refcount, exactly as
 * the Linux backend wraps its own; the readiness poll rind (exec_socket_raw) is
 * deliberately ABSENT here -- it is a Linux-only accessor (NetBSD readiness is
 * kqueue, a different rind).
 *
 * The DEFINITIONS live in a dedicated glue TU (src/kernel-netbsd/
 * vms_socket_netbsd.c) that includes <sys/socketvar.h> -- like the arena and
 * blockdev twins, the host-mm/socket-coupled bodies stay out of this shared
 * header. That TU + its four build-enum wirings (Makefile SRCS, the two cross
 * build scripts, tests/netbsd/Dockerfile guest-src) land with the vms_bg.c ->
 * kernel-core move on post-combine main; a genuinely RUNNABLE NetBSD BGn: is
 * vms-024. Until that move references them, these are unresolved-but-unreferenced
 * decls (no NetBSD executive TU calls exec_socket_* yet), so they add no link
 * dependency -- they are the type-check contract only. */
typedef struct exec_socket_holder *exec_socket_t;
int  exec_socket_create(exec_socket_t *out);
int  exec_socket_create_icmp(exec_socket_t *out);   /* raw ICMP for PING (vms-80b) */
void exec_socket_get(exec_socket_t s);
void exec_socket_release(exec_socket_t s);
int  exec_socket_connect(exec_socket_t s, uint16_t family, uint16_t port_be, uint32_t addr_be);
long exec_socket_send(exec_socket_t s, const void *buf, size_t len);
long exec_socket_recv(exec_socket_t s, void *buf, size_t len);
int  exec_socket_shutdown(exec_socket_t s);
int  exec_socket_getname(exec_socket_t s, int peer, uint16_t *family, uint16_t *port_be, uint32_t *addr_be);
int  exec_socket_setopt_int(exec_socket_t s, int level, int name, int val);
int  exec_socket_getopt_int(exec_socket_t s, int level, int name, int *out);

/* Server path (vms-698). exec_socket_accept mints a NEW holder for the accepted
 * connection (like exec_socket_create), returned via *out. Contract-only twin:
 * sobind/solisten/soaccept -- see vms_socket_netbsd.c. */
int  exec_socket_bind(exec_socket_t s, uint16_t family, uint16_t port_be, uint32_t addr_be);
int  exec_socket_listen(exec_socket_t s, int backlog);
int  exec_socket_accept(exec_socket_t s, exec_socket_t *out);

/* ================================================================
 * SS13  Host AF_PACKET raw datalink socket (vms-7eb, auth slice of vms-1e4).
 *
 * Substrate-neutrality anchor for the exec_l2_* seam -- these declarations
 * prove the SS13 contract (exec_kbackend.h) is EXPRESSIBLE in NetBSD terms
 * (the signatures name no Linux type), but the DEFINITIONS in
 * vms_socket_netbsd.c are honest CONTRACT-ONLY STUBS, not a real NetBSD
 * binding, because NetBSD has no in-kernel socket(9) domain for raw Ethernet
 * frames the way Linux has AF_PACKET -- the real NetBSD primitive for this is
 * BPF (bpfopen/bpf_setif/bpfwrite/bpfread, a wholly different attach-to-
 * interface design, not a socket at all), which is out of this increment's
 * scope. Each stub touches no device internals and reports failure (-1),
 * naming its real source here (INV-6 / Rule 11: it opens nothing and
 * fabricates nothing) -- the same posture the block-I/O and NIC-lookup
 * contract-only stubs above take. As with exec_socket_* above, the only
 * caller (src/kernel-core/vms_l2.c) is NOT in this module's SRCS (vms_l2.c
 * stays a Linux build for now, exactly as vms_bg.c does -- see vms_bg.c's own
 * header for why: struct vms_proc's per-facility channel lists that are host-
 * socket-coupled are wired substrate-by-substrate, and L2's is Linux-only
 * until a genuine NetBSD BPF binding lands), so these are unresolved-but-
 * unreferenced decls, type-check contract only. */
int  exec_l2_open(const char *ifname, uint16_t ethertype,
		  uint32_t *out_ifindex, exec_socket_t *out);
int  exec_l2_hwaddr(const char *ifname, uint8_t mac[6]);
long exec_l2_send(exec_socket_t s, int ifindex, uint16_t ethertype,
		  const uint8_t dst_mac[6], const void *frame, size_t len);
int  exec_l2_recv(exec_socket_t s, void *buf, size_t buf_len,
		  uint32_t timeout_ms, size_t *out_len);

/* ================================================================
 * SS14..SS18  The cluster seam (FC-P0.1) -- CONTRACT-ONLY STUBS.
 *
 * The NetBSD half of the contract frozen in exec_kbackend.h SS14..SS18 (read
 * CONTRACT RULES 1 and 2 there first). These declarations are the substrate-
 * neutrality proof: the whole cluster seam is expressible in NetBSD terms --
 * not one signature names a Linux type, an sk_buff, a netdev or a jiffy.
 *
 * The DEFINITIONS live in vms_lan_netbsd.c and are honest stubs returning
 * SS$_NOSUCHDEV today (the SS8/SS11/SS13 contract-only-twin posture: they open
 * nothing and fabricate nothing, INV-6 / Rule 9). FC-P0.3 records which
 * link-layer receive hook the rail's NetBSD actually has (pfil(9) on
 * ifp->if_pfil vs an ifp->if_input shim) and at which IPL qe/xq deliver; FC-P0.4
 * then lands the real binding -- if_transmit for xmit, if_mcast_op for
 * multicast, kthread(9), callout(9), getnanotime/getnanouptime, printf(9) --
 * and proves it with the rung-3 substrate contract test on the NetBSD-VAX rail.
 *
 * Types, unlike the ops, are REAL now, because the core embeds them by value and
 * their SIZE is part of the ABI FC-P0.4 must not change:
 *   exec_kthread_t  the lwp plus the stop flag / condvar kthread_join(9) needs
 *                   (NetBSD has no kthread_should_stop(9) reading `curlwp`, so
 *                   the flag is ours -- which is exactly why the portable
 *                   contract passes the handle to exec_kthread_should_stop).
 *   exec_timer_t    a callout(9) plus the {cb, ctx} pair callout_setfunc takes.
 *                   callout_destroy(9) is mandatory on this substrate, which is
 *                   why SS16 has a _destroy op at all.
 * ================================================================ */

typedef struct exec_kthread {
	struct lwp   *lwp;      /* kthread_create(9)'s lwp; NULL when not running */
	kmutex_t      mtx;      /* guards `stop` and pairs with `cv` */
	kcondvar_t    cv;       /* the fork loop sleeps here; stop wakes it */
	volatile int  stop;     /* nonzero once exec_kthread_stop was called */
} exec_kthread_t;

typedef struct exec_timer {
	struct callout co;
	void (*cb)(void *);
	void *ctx;
} exec_timer_t;

int  exec_lan_open(const char *ifname, uint16_t ethertype,
		   exec_lan_rx_cb_t rx_cb, void *ctx);
void exec_lan_close(void);
int  exec_lan_xmit(const uint8_t *frame, uint32_t len);
int  exec_lan_mc_add(const uint8_t mac[6]);
int  exec_lan_mc_del(const uint8_t mac[6]);
int  exec_lan_hwaddr(uint8_t out[6]);
int  exec_lan_mtu(uint32_t *out);
int  exec_lan_link_up(int *out);

int  exec_kthread_create(exec_kthread_t *t, int (*fn)(void *), void *arg,
			 const char *name);
void exec_kthread_stop(exec_kthread_t *t);
int  exec_kthread_should_stop(exec_kthread_t *t);

void exec_timer_init(exec_timer_t *t, void (*cb)(void *), void *ctx);
void exec_timer_arm(exec_timer_t *t, uint32_t ms);
void exec_timer_cancel(exec_timer_t *t);
void exec_timer_destroy(exec_timer_t *t);

uint64_t exec_time_now_vms(void);
uint64_t exec_ticks_ms(void);

/* SS18: a macro for the same reason the Linux side is one -- the format string
 * reaches printf(9) directly, so the compiler checks the call site. This one is
 * ALREADY the real binding (printf(9) writes the NetBSD console, which is OPA0:
 * on the VAX rail); FC-P0.4 does not need to revisit it. */
#define exec_console_printf(fmt, ...) printf(fmt, ##__VA_ARGS__)

#endif /* OVMX_EXEC_KBACKEND_NETBSD_H */
