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
 * PHASE F ADDITIONS (this landing -- rd vms-846b, the process table).
 * vms_proctab.c is the FIRST facility to bind the host task, so it is where
 * the host-task / RCU-lite / sleepable-mutex seam lands. Added below, each
 * only because vms_proctab.c actually calls it (design rule: keep it minimal):
 *
 *   5. host-task binding   exec_current_is_privileged() (its only current->
 *      read is capable(CAP_SYS_ADMIN); the UIC/username DERIVATION from
 *      current's credentials lives in vms_module.c's registration, not here,
 *      and lands with the vms_module split), and the liveness/accounting
 *      handle ops exec_task_alive / exec_task_pin / exec_task_read_acct /
 *      exec_task_unpin over an opaque exec_task_ref_t (the PCB's pid_ref).
 *   6. RCU-lite            exec_rcu_read_lock / exec_rcu_read_unlock and the
 *      deferred free exec_free_deferred, plus exec_rcu_head_t. RCU has no
 *      NetBSD twin (design record §2/§3/§5 caveat 3): the Linux backend maps
 *      to real RCU; the NetBSD backend uses the design's blessed lock-based
 *      fallback (no lockless readers there, so free is immediate under the
 *      hash lock). The intrusive hash itself is a SEPARATE seam, exec_hash.h.
 *   7. sleepable mutex     exec_mutex_t + EXEC_DEFINE_MUTEX / exec_mutex_lock
 *      / exec_mutex_unlock / exec_mutex_init / exec_mutex_destroy. DISTINCT
 *      from exec_lock_t: exec_lock is a non-sleeping spin/adaptive lock;
 *      exec_mutex is a process-context lock that MAY be held across a
 *      sleeping operation (vms_proctab's reap serializer holds it across the
 *      per-victim teardown). Linux: struct mutex. NetBSD: an adaptive
 *      kmutex(9) at IPL_NONE (process context).
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
 *   int  exec_trylock(exec_lock_t *)       (Phase G) try to acquire without
 *                                          blocking; nonzero iff acquired. Used by
 *                                          the lock manager's deadlock walker,
 *                                          which must probe another process's
 *                                          lock-list lock while holding one and
 *                                          cannot risk an ABBA block. Linux:
 *                                          spin_trylock. NetBSD: mutex_tryenter.
 *
 * 2. Wait / wake  --  cv(9)-SHAPED CONTRACT.  READ THIS BEFORE USING.
 *
 *   void exec_cv_init(exec_cv_t *)
 *   int  exec_cv_wait(exec_cv_t *cv, exec_lock_t *lk)
 *   int  exec_cv_wait_timeout(exec_cv_t *cv, exec_lock_t *lk, unsigned int ms,
 *                             int *timed_out)      (Phase G) exec_cv_wait with a
 *        bounded sleep: identical cv contract (caller holds `lk`, loops
 *        re-testing the predicate), but the sleep ends after `ms` milliseconds if
 *        no wake arrives, and `*timed_out` is set nonzero on exactly that timeout
 *        (0 on a wake or a signal). Returns nonzero iff interrupted, like
 *        exec_cv_wait. The lock manager's synchronous $ENQW uses it so a bounded
 *        timeout can drive a deadlock re-scan for a request that only becomes
 *        part of a cycle after it queued. Linux: the one-iteration expansion of
 *        wait_event_interruptible_TIMEOUT (schedule_timeout). NetBSD:
 *        cv_timedwait_sig (EWOULDBLOCK => *timed_out, ERESTART/EINTR => return
 *        nonzero). Each call re-arms the full `ms`.
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
 *
 * 5. Host-task binding  (Phase F; called ONLY from the process table +, later,
 *    the device/module glue -- the host-task coupling is concentrated in those
 *    two files, design record §2). "current" is the host task issuing the ioctl.
 *
 *   int  exec_current_is_privileged(void)
 *        nonzero iff the CURRENT host task is privileged enough to construct a
 *        VMS identity out of nothing. Linux: capable(CAP_SYS_ADMIN). NetBSD:
 *        kauth "is-superuser". This is a REAL host credential, not a value a
 *        process can grant itself (vms_ioctl_establish_system's gate).
 *
 *   uint32_t exec_current_uid(void)
 *   uint32_t exec_current_gid(void)
 *        the CURRENT host task's REAL user / group id, mapped into the host's
 *        INITIAL id namespace, as a plain uint32_t (vms-31b; the device table's
 *        caller_uic() packs [gid,uid] into a VMS UIC exactly the way sys$getjpi's
 *        JPI$_UIC item does). Like exec_current_is_privileged these are real host
 *        credentials read off `current`, not values a process can grant itself.
 *        Linux: from_kuid(&init_user_ns, current_uid()) / from_kgid(&init_user_ns,
 *        current_gid()) -- the real, not effective, id, matching what the
 *        executive read before this seam existed. NetBSD: kauth_cred_getuid /
 *        kauth_cred_getgid on kauth_cred_get(). The [group,member] -> UIC PACKING
 *        stays in the portable facility; only the raw id read crosses the seam.
 *
 *   The PCB stores an OPAQUE liveness handle, exec_task_ref_t (the executive's
 *   pid_ref: on Linux the thread group's struct pid, taken at registration).
 *   A facility never dereferences it -- it only passes it to these ops:
 *
 *   int  exec_task_alive(exec_task_ref_t *ref)
 *        nonzero iff the PROCESS backing `ref` still exists. This is a
 *        whole-process test (the thread-group leader), NOT a per-thread one, so
 *        a thread exiting out of a live multithreaded image does not make the
 *        process reapable. Linux: pid_task(ref, PIDTYPE_PID) != NULL under an
 *        RCU read section (the RCU is INSIDE this op, not the caller's concern).
 *
 *   exec_task_pin_t *exec_task_pin(exec_task_ref_t *ref)
 *        pin the backing task so it survives past a lock drop, returning an
 *        opaque pinned handle, or NULL if the process has already exited. MUST
 *        NOT sleep, so it is safe to call while holding an exec_lock (Linux:
 *        rcu-guarded pid_task + get_task_struct). The caller pins UNDER the
 *        table lock, drops the lock, then reads accounting off the pinned
 *        handle -- because that read MAY sleep and must not run under the lock.
 *
 *   void exec_task_read_acct(exec_task_pin_t *pin, struct exec_proc_acct *out)
 *        fill `out` with the pinned process's REAL accounting -- CPU time, page
 *        faults, creation time, resident pages (JPI$_CPUTIM / PAGEFLTS /
 *        LOGINTIM / PPGCNT). MAY SLEEP; call it only AFTER dropping the table
 *        lock, on a handle from exec_task_pin. These are real properties of a
 *        real process measured by the host kernel (INV-6 / Rule 11: not a
 *        fabrication); the VMS unit conversions and field mapping stay in the
 *        portable facility. This is a host-task-PROPERTY read (a scalar RSS/
 *        cputime read), NOT the userspace-arena MAPPING seam (vmalloc_user /
 *        remap_vmalloc_range) that vms_lnm.c waits on -- a different, later seam.
 *
 *   void exec_task_unpin(exec_task_pin_t *pin)   drop the exec_task_pin ref.
 *
 * 6. RCU-lite deferred reclaim  (Phase F). The process hash has LOCKLESS
 *    readers (vms_module.c's vms_proc_find walks it under an RCU read section,
 *    no table lock), so an unlinked PCB must not be freed until every such
 *    reader that could still see it has finished -- a grace period.
 *
 *   void exec_rcu_read_lock(void)     enter a read section (a lockless reader
 *   void exec_rcu_read_unlock(void)   may traverse the hash between these).
 *   void exec_free_deferred(struct exec_rcu_head *h, void (*fn)(struct exec_rcu_head *))
 *        free (via `fn`) AFTER a grace period, once no read section that began
 *        before the unlink is still running. `h` is an exec_rcu_head_t embedded
 *        in the freed object. Linux: call_rcu. NetBSD: the design's blessed
 *        fallback -- there are no lockless readers on that substrate (the hash
 *        walks run under the hash mutex), so `fn` runs immediately.
 *
 *   GRACE-PERIOD CONTRACT, and how it pairs with exec_hash.h. The unlink is
 *   exec_hash_del_rcu (exec_hash.h): it removes the node so no read section
 *   STARTED AFTER the unlink can reach it, while leaving the node's forward
 *   pointer valid so a reader ALREADY traversing it walks off cleanly. The
 *   object is then reclaimed with exec_free_deferred, which waits out the
 *   readers that began before the unlink. Unlink-with-exec_hash_del_rcu and
 *   free-with-exec_free_deferred are therefore two halves of ONE idiom; using
 *   exec_hash_del_rcu and then freeing synchronously is a use-after-free, and
 *   using a plain exec_hash_del with lockless readers is list corruption.
 *
 * 7. Sleepable mutex  (Phase F) -- see the PHASE F ADDITIONS note above for why
 *    it is distinct from exec_lock_t.
 *   EXEC_DEFINE_MUTEX(name)             define a file-scope mutex (Linux static
 *                                       init; NetBSD requires exec_mutex_init).
 *   void exec_mutex_init(exec_mutex_t *)
 *   void exec_mutex_lock(exec_mutex_t *)     acquire; MAY sleep
 *   void exec_mutex_unlock(exec_mutex_t *)
 *   void exec_mutex_destroy(exec_mutex_t *)  no-op on Linux; mutex_destroy on NetBSD
 *
 * 8. Block-device resolution  (vms-31b; called ONLY from the device table, whose
 *    disk units are enumerated from the node's backing block devices). This is
 *    the block-layer twin of the host-task seam: a small set of host PRIMITIVES
 *    (no container), so it lands here in exec_kbackend.h rather than in a
 *    container-style header like exec_list/hash/rbtree. The executive names a
 *    block device by its host /dev PATH, resolves it to an opaque device id
 *    WITHOUT opening it, and reads the id's major/minor to report to a process
 *    that must open the backing device (MOUNT).
 *
 *   Type (concrete per substrate):
 *     exec_dev_t   an opaque block-device identifier. Linux: dev_t. NetBSD: dev_t.
 *
 *   int exec_blockdev_lookup(const char *path, exec_dev_t *out)
 *        resolve host device PATH (e.g. "/dev/vda") to *out WITHOUT opening the
 *        device. Returns 0 on success, nonzero if no such device (the ordinary
 *        end-of-run case as the executive probes vda..vdz -- not an error).
 *        Linux: lookup_bdev(path, out), normalized to 0 / nonzero. NetBSD: the
 *        documented contract-only twin until the devsw/dev_t mapping lands with
 *        devtab-on-NetBSD (following the exec_rbtree precedent -- devtab is not
 *        in the NetBSD module's SRCS yet, so this side is type-checked, never
 *        run, and names its real source in the backend comment).
 *   unsigned int exec_blockdev_major(exec_dev_t dev)
 *   unsigned int exec_blockdev_minor(exec_dev_t dev)
 *        the major / minor of a resolved exec_dev_t. Linux: MAJOR() / MINOR().
 *        NetBSD: major() / minor() (real -- these are portable dev_t accessors).
 */

#ifndef OVMX_EXEC_KBACKEND_H
#define OVMX_EXEC_KBACKEND_H

/* Normalized copyin/copyout fault code (== EFAULT everywhere we target). */
#define EXEC_EFAULT 14

/*
 * Portable process-accounting payload (Phase F). exec_task_read_acct() fills
 * this from the host task; the facility maps it to VMS JPI items. Substrate-
 * neutral by construction -- host-clock and mm machinery stay in the backend,
 * only these already-meaningful scalars cross the seam. uint64_t is provided by
 * the includer (vms_internal.h pulls the host's fixed-width types before this).
 */
struct exec_proc_acct {
	uint64_t cpu_ns;         /* total CPU time (user+system), nanoseconds */
	uint64_t page_faults;    /* soft + hard page faults taken by the process */
	uint64_t create_wall_ns; /* creation time, ns since the Unix epoch (wall) */
	uint64_t rss_pages;      /* resident set size in pages (valid iff has_rss) */
	int      has_rss;        /* nonzero iff the process has a user address space */
};

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
