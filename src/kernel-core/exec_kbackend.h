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
 *
 *   int exec_blockdev_read_block(unsigned int major, unsigned int minor,
 *                                uint64_t lbn, void *buf, size_t buflen)
 *        READ exactly one 512-byte logical block at `lbn` from the block device
 *        named by (major, minor) into `buf` (at least ODS2_BLOCK_SIZE == 512
 *        bytes). Returns 0 on success, nonzero on any failure (device not
 *        openable, short/failed transfer, buffer too small). This is the read
 *        twin the Files-11 ODS-2 ACP $MOUNT needs (vms-127): the executive
 *        already resolves a VMS disk unit to its backing (major,minor) via the
 *        lookup+major+minor ops above, and MOUNT must read the home block (LBN
 *        1) + SCB off that device to VALIDATE the volume is genuine ODS-2 before
 *        recording it in the executive-global mounted table -- so the read lives
 *        in the executive, not in a process (CLAUDE.md Rule 11 / INV-6). MAY
 *        SLEEP (opens the device + reads through the host buffer cache); call it
 *        only from process context with no exec_lock held. Linux: bdev opened
 *        read-only, non-exclusively (version-guarded across the bdev_open_by_dev
 *        / bdev_file_open_by_dev API split) + a SYNCHRONOUS bio (submit_bio_wait)
 *        rather than the buffer cache, which assumes a mounted-filesystem block
 *        size the ACP does not have when it reads a raw disk pre-mount. NetBSD:
 *        the documented contract-only twin until devtab joins the module's SRCS
 *        (following the exec_blockdev_lookup precedent -- type-checked, never
 *        run, and names its real source in the backend comment).
 *
 *   int exec_blockdev_write_block(unsigned int major, unsigned int minor,
 *                                 uint64_t lbn, const void *buf, size_t buflen)
 *        WRITE exactly one 512-byte logical block at `lbn` to (major, minor)
 *        from `buf` (>= ODS2_BLOCK_SIZE == 512 bytes). Returns 0 on success,
 *        nonzero on any failure. The write twin of exec_blockdev_read_block, for
 *        the Files-11 ODS-2 ACP IO$_WRITEVBLK and implicit-extend path (vms-c60):
 *        the executive writes a file's mapped LBNs, the BITMAP.SYS storage
 *        bitmap, and the FH2 header back to the RAW backing device -- the write
 *        half of the ACP the read half already does in the executive (CLAUDE.md
 *        Rule 11 / INV-6, never in a process). MAY SLEEP; call it only from
 *        process context with no exec_lock held. Linux: bdev opened READ-WRITE,
 *        non-exclusively (same version-guarded open split as the read twin) + a
 *        SYNCHRONOUS write bio (submit_bio_wait, REQ_OP_WRITE), not the buffer
 *        cache, for the same raw-disk / no-mounted-super_block reason. NetBSD:
 *        the documented contract-only twin until devtab joins the module's SRCS.
 *
 * 9. Store/load memory barriers  (vms-d61; called ONLY from a facility that
 *    publishes a lock-free snapshot to userspace -- today the logical-name
 *    arena's seqlock in vms_lnm.c). These are the ordering primitives a
 *    seqlock needs: the WRITER bumps the generation counter odd, does its
 *    stores, bumps it even, and fences so a userspace READER never sees the
 *    counter move before the stores it guards. No lock is involved (the reader
 *    is in another address space over the mmap), so a plain memory barrier --
 *    NOT an exec_lock -- is what orders the two sides.
 *
 *   void exec_membar_producer(void)
 *        a STORE-STORE barrier: every store BEFORE it is globally visible
 *        before any store AFTER it. The seqlock writer calls it between the
 *        odd-bump and the entry stores, and again between the entry stores and
 *        the even-bump, so the generation transitions bracket the payload.
 *        Linux: smp_wmb(). NetBSD: membar_producer().
 *   void exec_membar_consumer(void)
 *        the LOAD-LOAD twin: every load AFTER it happens after every load
 *        BEFORE it. Provided for contract symmetry -- the seqlock READ side
 *        lives in userspace (vms_kif's arena reader), so no in-kernel facility
 *        calls this today; it documents the barrier the reader pairs with.
 *        Linux: smp_rmb(). NetBSD: membar_consumer().
 *
 * 10. Userspace-publishable arena  (vms-d61; called ONLY from a facility that
 *    owns a kernel data structure the host char device maps read-only into
 *    every process -- today the logical-name arena, vms_lnm.c). This is the
 *    ALLOCATION half of that seam and NOTHING MORE. The FACILITY allocates the
 *    arena here, writes it under its own exec_lock, and publishes updates with
 *    exec_membar_producer; it hands the base+size to the host module's mmap
 *    handler through a plain accessor.
 *
 *   void *exec_arena_alloc(size_t n)
 *        allocate a ZEROED region of n bytes whose pages the host char
 *        device's mmap handler can later publish read-only to a process.
 *        Returns a kernel virtual address the facility writes through with
 *        ordinary stores (under its lock), or NULL on failure. The SAME
 *        pointer is the handle the mmap glue maps. Linux: vmalloc_user(n)
 *        (page-aligned, zeroed, and the one allocation remap_vmalloc_range
 *        accepts). NetBSD: a uvm-anonymous-object-backed allocation
 *        (contract-only twin, exec_rbtree/blockdev precedent -- vms_lnm.c is
 *        not in the NetBSD module's SRCS, so this side is type-checked-at-most,
 *        never run, and names its real source in the backend comment).
 *   void exec_arena_free(void *arena)
 *        free an exec_arena_alloc block. Linux: vfree(). NetBSD: release the
 *        uvm object (contract-only twin).
 *
 *   WHERE THE MMAP-TIME MAPPING IS **NOT**. Publishing the arena INTO a
 *   process -- remap_vmalloc_range + clearing VM_MAYWRITE on Linux; uao_map /
 *   uvm_map on NetBSD -- is HOST CHAR-DEVICE GLUE, not facility logic, and does
 *   NOT cross this seam. It lives in the host module's /dev mmap handler
 *   (src/kernel/vms_module.c's vms_lnm_mmap, the Linux rind), which asks the
 *   facility for the arena base+size and does the substrate-specific mapping
 *   with the raw host primitives. The seam is deliberately "give me an arena I
 *   can write and a base the mmap handler can map" and stops there: the
 *   facility never names struct vm_area_struct, remap_vmalloc_range, VM_* or
 *   PAGE_* (design record docs/design-netbsd-executive-core.md §2, the
 *   host-mm coupling stays in the rind like registration does).
 *
 * 11. Primary Ethernet net device  (vms-9d2; called ONLY from the device table,
 *    which surfaces the node's primary Ethernet controller as the VMS device
 *    ETH0:). The block-layer seam's network twin: a small set of host
 *    PRIMITIVES, no container, so it lands here rather than in a container-style
 *    header. NIC-AGNOSTIC BY CONSTRUCTION (operator, 2026-08-14): the executive
 *    names NO driver -- it asks the host for its primary non-loopback Ethernet
 *    net device through the GENERIC netdev abstraction, so ETH0: backs whatever
 *    that device happens to be (virtio-net in the QEMU runtime, e1000, a real
 *    NIC on bare metal) with no code path keyed on any one driver. The interface
 *    NAME the host uses (eth0/enp0s1/...) is a host detail the executive records
 *    for its own binding and NEVER surfaces to a VMS program (INV-4).
 *
 *   int exec_netdev_primary(char *name, unsigned int namesz, int *link_up)
 *        find the host's PRIMARY (first, in kernel enumeration order)
 *        non-loopback Ethernet net device. Returns 0 and, when name/namesz are
 *        given, copies the host interface name (NUL-terminated, truncated to
 *        namesz-1) and sets *link_up (nonzero iff the carrier is up) when one
 *        exists; returns nonzero when the node has NO such device -- the honest
 *        "no NIC" case, in which the executive registers no ETH0: at all (INV-6:
 *        no fake device for a NIC that is not there). Linux: for_each_netdev over
 *        init_net under rtnl_lock, skipping IFF_LOOPBACK and requiring
 *        ARPHRD_ETHER, taking the first match. NetBSD: the documented
 *        contract-only twin until devtab joins the NetBSD module's SRCS
 *        (following the exec_blockdev precedent -- type-checked, never run, and
 *        names its real source in the backend comment).
 *
 * 12. Host TCP client socket  (vms-9951; called ONLY from the BGn: INET facility,
 *    src/kernel-core/vms_bg.c -- the executive-resident network pseudo-device,
 *    the block-layer seam's transport twin). OVMX never reimplements a transport:
 *    the IP stack is the host kernel's, reached through this small set of host
 *    PRIMITIVES over an OPAQUE socket handle. The client path only (connect / send
 *    / recv / shutdown / getname / sockopt); listen/accept are declared contract-
 *    only (the bgsock veneer returns ENOSYS for the server path in this increment).
 *
 *   Type (concrete per substrate):
 *     exec_socket_t   an opaque, REFERENCE-COUNTED host TCP socket handle. The
 *                     refcount is encapsulated in the type BECAUSE the Linux
 *                     readiness poll fd (VMS_IOCTL_BG_POLLFD, a Linux rind, no
 *                     NetBSD kqueue analogue) must outlive the channel's $DASSGN.
 *                     Linux: a kref'd holder over `struct socket *`. NetBSD: a
 *                     `struct socket *` (no poll fd, so the refcount is trivial).
 *
 *   Addresses cross the seam as RAW network-order fields (family, port, v4 addr),
 *   NOT struct sockaddr_in, so kernel-core names no networking header; the backend
 *   builds the host sockaddr.
 *
 *   int  exec_socket_create(exec_socket_t *out)
 *        create an AF_INET/SOCK_STREAM/IPPROTO_TCP host socket, ref count 1.
 *        Returns 0 + *out on success, nonzero on failure. MAY SLEEP.
 *        Linux: sock_create_kern(&init_net, ...) into a kref'd holder.
 *        NetBSD: socreate(AF_INET, ..., curlwp, NULL) (contract-only twin).
 *   void exec_socket_get(exec_socket_t s)
 *        take an ADDITIONAL reference (the Linux poll fd rind's second ref).
 *        Linux: kref_get. NetBSD: trivial (never called -- no poll fd).
 *   void exec_socket_release(exec_socket_t s)
 *        drop one reference; the last drop closes the host socket.
 *        Linux: kref_put -> sock_release. NetBSD: soclose.
 *   int  exec_socket_connect(exec_socket_t s, uint16_t family,
 *                            uint16_t port_be, uint32_t addr_be)
 *        connect to the peer (fields already in network byte order). 0/nonzero.
 *        MAY SLEEP. Linux: kernel_connect(...,0) (blocking). NetBSD: soconnect +
 *        wait for soisconnected (contract-only twin; async -- see vms-024).
 *   long exec_socket_send(exec_socket_t s, const void *buf, size_t len)
 *        send up to len bytes; returns the count sent, or negative on error.
 *        MAY SLEEP. Linux: kernel_sendmsg (kvec). NetBSD: uio -> sosend.
 *   long exec_socket_recv(exec_socket_t s, void *buf, size_t len)
 *        receive up to len bytes; returns the count (0 == orderly peer close /
 *        EOF), or negative on error. MAY SLEEP. Linux: kernel_recvmsg. NetBSD:
 *        uio -> soreceive.
 *   int  exec_socket_shutdown(exec_socket_t s)
 *        shut both directions down. Linux: kernel_sock_shutdown(SHUT_RDWR).
 *        NetBSD: soshutdown(SHUT_RDWR).
 *   int  exec_socket_getname(exec_socket_t s, int peer, uint16_t *family,
 *                            uint16_t *port_be, uint32_t *addr_be)
 *        report the socket's local (peer==0) or peer (peer!=0) AF_INET address.
 *        Returns 0 (+ fields) on an AF_INET socket, nonzero otherwise (the honest
 *        "not an IPv4 tuple" case, never a fabricated address -- the de-veneer
 *        crux, vms-4bf). Linux: kernel_get{sock,peer}name. NetBSD: the pr_usrreqs
 *        sockaddr op (contract-only twin).
 *   int  exec_socket_setopt_int(exec_socket_t s, int level, int name, int val)
 *        set one integer socket option on the REAL host socket. 0/nonzero.
 *        Linux: sock_setsockopt (SOL_SOCKET) or ops->setsockopt, KERNEL_SOCKPTR.
 *        NetBSD: sockopt_setint + sosetopt (contract-only twin).
 *   int  exec_socket_getopt_int(exec_socket_t s, int level, int name, int *out)
 *        read one integer socket option back off the LIVE socket. Returns 0 (+
 *        *out) for the supported whitelist (SO_KEEPALIVE/SO_REUSEADDR/SO_ERROR,
 *        TCP_NODELAY, IP_TOS -- exactly what OpenSSH probes), nonzero for anything
 *        outside it (honest "unsupported", never a faked value). The whitelist
 *        logic itself is BACKEND code (Linux reads raw struct sock; NetBSD
 *        sogetopt), so no raw-struct-sock read leaks into shared core.
 *
 *   Contract-only (server path, bgsock returns ENOSYS today): exec_socket_bind /
 *   exec_socket_listen / exec_socket_accept -> Linux kernel_bind/listen/accept,
 *   NetBSD sobind/solisten/soaccept.
 *
 *   WHERE THE READINESS POLL FD IS **NOT**. VMS_IOCTL_BG_POLLFD hands userspace a
 *   real Linux fd whose .poll delegates to the host socket's ->poll (OpenSSH's
 *   event loop, vms-22a). anon_inode_getfile / fd_install / EPOLL / ->ops->poll
 *   are pure Linux fd machinery with NO NetBSD analogue (NetBSD uses kqueue), so
 *   the poll fd stays a LINUX RIND over this seam (src/kernel/vms_bg_pollfd.c): it
 *   holds an extra exec_socket_get reference and reaches the raw struct socket
 *   through a Linux-only accessor. NetBSD BGn: is therefore NOT feature-complete
 *   for a poll()-driven client until a kqueue equivalent lands (vms-024); the
 *   NetBSD backend is the type-checked contract-only twin until then.
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
