// SPDX-License-Identifier: GPL-2.0
/*
 * vms_eflag.c - Kernel Event Flags (Phase 3c)
 *
 * Implements VMS event flags with proper kernel wait queues:
 *   - Local clusters 0-1 (EFN 0-63): per-process, kernel-managed
 *   - Common clusters 2-3 (EFN 64-127): named, shared between processes
 *
 * Unlike the pthread condvar implementation, waiting here uses
 * kernel wait_queue_head_t, making the process state visible to
 * the kernel scheduler (proper TASK_INTERRUPTIBLE sleep).
 *
 * Event flag operations:
 *   SETEF   - Set a flag (wake waiters)
 *   CLREF   - Clear a flag
 *   WAITFR  - Wait for a single flag to be set
 *   WFLOR   - Wait for any flag in mask to be set (OR wait)
 *   WFLAND  - Wait for all flags in mask to be set (AND wait)
 *   READEF  - Read cluster state without waiting
 *   ASCEFC  - Associate with a common event flag cluster
 *   DACEFC  - Disassociate from a common event flag cluster
 *   DLCEFC  - Mark a permanent common event flag cluster for deletion
 *
 * INTERRUPTED WAITS: THERE IS NO SUCH OUTCOME (vms-2a8, CLAUDE.md Rule 10).
 *
 * WAITFR, WFLOR and WFLAND used to answer SS$_NORMAL when
 * wait_event_interruptible() returned because a signal was pending:
 *
 *     ret = wait_event_interruptible(...);
 *     if (ret) { args.status = SS__NORMAL;  <- "interrupted, but still
 *                                               return normally"
 *
 * That reports "the flag is set" for a flag that is demonstrably still
 * clear, and the caller gets rc=0/errno=0 so it cannot even detect it.
 * It is the same fabricated-success class this whole facility was wired
 * to delete, one layer down.
 *
 * ORACLE-PINNED, VAX1 OpenVMS VAX V7.3, transcripts in
 * docs/oracle/vax73-event-flags.md §4:
 *   - HELP SYSTEM_SERVICES $WAITFR: "Tests a specific event flag and
 *     returns immediately if the flag is set; otherwise, the process is
 *     placed in a wait state UNTIL THE EVENT FLAG IS SET." The online help
 *     has no Condition Values Returned topic for it at all.
 *   - HELP SYSTEM_SERVICES $HIBER: a process in a wait state remains "known
 *     to the system so that it can be interrupted; for example, to receive
 *     ASTs" -- so a VMS wait IS interruptible, by an AST, and the process is
 *     still waiting when the AST finishes. The caller of the wait never sees
 *     it happen.
 *   - SEARCH of $SSDEF (STARLET.MLB) for WAIT/INTERRUPT/ABORTED returns four
 *     unrelated symbols. VMS HAS NO "WAIT WAS INTERRUPTED" STATUS TO RETURN.
 *
 * So the condition is made UNREACHABLE rather than handled. These three
 * handlers return -ERESTARTSYS and write NO status; libvmssys'
 * kif_wait_call() re-enters the wait, so no sys$ caller can observe a wait
 * that ended with its predicate false. Do not "improve" this by inventing a
 * status here -- there is not one to invent.
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "vms_internal.h"
/*
 * Event flags is the FIRST facility promoted onto the kernel-backend shim
 * (rd vms-adb, epic vms-8e8; design docs/design-netbsd-executive-core.md).
 * Every host primitive this file touches -- locking, wait/wake, copyin/out,
 * kernel alloc/free -- now goes through exec_* (defined per substrate in
 * exec_kbackend.h). On Linux each exec_* op expands to the exact primitive
 * this file used before (spin_lock, wait_event_interruptible's body,
 * copy_*_user, kzalloc/kfree), so the module is behaviour-identical; the same
 * source will compile against the NetBSD backend when it MOVES to
 * src/kernel-core/ (Phase B/C). The file does NOT move in Phase A, so its
 * intrusive-list use (list_*) stays on <linux/list.h> for now -- that is the
 * next shim seam (exec_list.h), added when the file moves.
 *
 * The one non-mechanical change here is the wait/wake conversion. Linux's
 * wait_event_interruptible is lock-free; the portable cv contract requires
 * the waiter to hold, and the waker to share, the lock that guards the flag
 * word and the wait queue (exec_kbackend.h documents why -- it is what makes
 * the wait lost-wakeup-free on a substrate without wait_event). For a LOCAL
 * flag that guard is proc->ef.lock, which the setter already held. For a
 * COMMON cluster the two sides live in DIFFERENT processes with different
 * proc->ef.lock instances, so the shared guard MUST be the cluster's own lock
 * (struct vms_common_ef_cluster::lock, previously initialized but unused for
 * the flag word). setef/clref/readef and the three wait handlers therefore
 * access a common cluster's flags+waitq under c->lock (resolving the cluster
 * pointer under proc->ef.lock first; lock order proc->ef.lock -> c->lock,
 * c->lock innermost and never held while taking another lock). This changes
 * NO VMS-observable behaviour -- same flags, same statuses, same wait/wake --
 * and removes the pre-existing cross-process data race on a common cluster's
 * flag word as a side effect. The multi-process suites
 * tests/qemu/test_{kmod,syssvc}_ef_mproc.c, which block one process on a
 * common flag another sets, are the gate that proves the wake is not lost.
 */
#include "exec_kbackend.h"


/*
 * Global common event flag cluster list.
 *
 * Lock ordering: proc->ef.lock MUST be acquired before vms_common_ef_lock.
 * Never acquire proc->ef.lock while holding vms_common_ef_lock.
 */
LIST_HEAD(vms_common_ef_list);
DEFINE_SPINLOCK(vms_common_ef_lock);

void vms_eflag_init(void)
{
    /* Nothing extra needed -- list is statically initialized */
}

void vms_eflag_cleanup(void)
{
    struct vms_common_ef_cluster *cluster, *tmp;

    exec_lock(&vms_common_ef_lock);
    list_for_each_entry_safe(cluster, tmp, &vms_common_ef_list, list) {
        list_del(&cluster->list);
        exec_free(cluster);
    }
    exec_unlock(&vms_common_ef_lock);
}

/*
 * Release common EF associations for a process being freed
 */
void vms_proc_release_common_ef(struct vms_proc *proc)
{
    int i;

    exec_lock(&proc->ef.lock);
    for (i = 0; i < 2; i++) {
        struct vms_common_ef_cluster *cluster = proc->ef.common[i];
        if (cluster) {
            exec_lock(&vms_common_ef_lock);
            cluster->refcount--;
            if (cluster->refcount <= 0 && !cluster->perm) {
                list_del(&cluster->list);
                exec_free(cluster);
            }
            exec_unlock(&vms_common_ef_lock);
            proc->ef.common[i] = NULL;
        }
    }
    exec_unlock(&proc->ef.lock);
}

/*
 * common_idx - which common cluster a flag number names, or -1.
 *
 * ORACLE-PINNED (vms-2a8), docs/oracle/vax73-event-flags.md. HELP
 * SYSTEM_SERVICES $ASCEFC Arguments on the reference lab VAX V7.3:
 *
 *   "To associate with common event flag cluster 2, specify any flag
 *    number in the cluster (64 to 95); to associate with common event
 *    flag cluster 3, specify any event flag number in the cluster (96
 *    to 127)."
 *
 * ANY flag number in the range -- not only the base numbers. ASCEFC and
 * DACEFC here used to accept exactly 64 or 96 and answer SS$_ILLEFC for
 * 65..95 and 97..127, which are legal on VMS.
 */
static int common_idx(uint32_t efn)
{
    if (efn >= 64 && efn < 96)
        return 0;
    if (efn >= 96 && efn < 128)
        return 1;
    return -1;
}

/*
 * Helper: get pointer to flag word, wait queue and GUARDING LOCK for a given
 * EFN. Returns 0 on success, fills *flags_ptr, *waitcv, *bit, *guard.
 *
 * *guard is the lock under which *flags_ptr and *waitcv are accessed and the
 * cv-wait/wake handshake is performed. For a LOCAL flag it is proc->ef.lock
 * (which the caller already holds, and which also protects local[] and the
 * association pointers). For a COMMON cluster it is the cluster's OWN lock --
 * the only lock a setter in one process and a waiter in another can share --
 * and the caller resolves the cluster pointer under proc->ef.lock before
 * switching to it. Callers must hold proc->ef.lock across this call.
 */
static int efn_resolve(struct vms_proc *proc, uint32_t efn,
                       uint32_t **flags_ptr, exec_cv_t **waitcv,
                       int *bit, exec_lock_t **guard)
{
    if (efn < 32) {
        *flags_ptr = &proc->ef.local[0];
        *waitcv = &proc->ef.waitq;
        *guard = &proc->ef.lock;
        *bit = efn;
        return 0;
    } else if (efn < 64) {
        *flags_ptr = &proc->ef.local[1];
        *waitcv = &proc->ef.waitq;
        *guard = &proc->ef.lock;
        *bit = efn - 32;
        return 0;
    } else if (efn < 96) {
        struct vms_common_ef_cluster *c = proc->ef.common[0];
        if (!c) return -1;
        *flags_ptr = &c->flags;
        *waitcv = &c->waitq;
        *guard = &c->lock;
        *bit = efn - 64;
        return 0;
    } else if (efn < 128) {
        struct vms_common_ef_cluster *c = proc->ef.common[1];
        if (!c) return -1;
        *flags_ptr = &c->flags;
        *waitcv = &c->waitq;
        *guard = &c->lock;
        *bit = efn - 96;
        return 0;
    }
    return -1;
}

/*
 * vms_ioctl_setef - Set event flag
 */
long vms_ioctl_setef(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;
    uint32_t prev;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /*
     * Set the flag and wake waiters UNDER THE PREDICATE'S GUARD LOCK, so the
     * wait/wake pair shares a lock and the wake cannot be lost (cv contract,
     * exec_kbackend.h). For a local flag the guard is proc->ef.lock, already
     * held; for a common cluster it is the cluster's own lock, nested inside
     * proc->ef.lock (which keeps the association -- and thus the cluster --
     * alive). exec_cv_broadcast wakes ALL waiters, preserving the historical
     * wake_up_interruptible().
     */
    if (guard != &proc->ef.lock)
        exec_lock(guard);

    prev = *flags & (1U << bit);
    *flags |= (1U << bit);
    exec_cv_broadcast(waitcv);

    if (guard != &proc->ef.lock)
        exec_unlock(guard);
    exec_unlock(&proc->ef.lock);

    args.status = prev ? SS__WASSET : SS__WASCLR;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_clref - Clear event flag
 */
long vms_ioctl_clref(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;
    uint32_t prev;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /* Clear under the flag word's guard (proc->ef.lock for local; the
     * cluster's own lock, nested, for a common cluster) so the word stays
     * consistently guarded against a concurrent setef. Clearing never
     * satisfies a wait-for-set, so there is no wake here. */
    if (guard != &proc->ef.lock)
        exec_lock(guard);

    prev = *flags & (1U << bit);
    *flags &= ~(1U << bit);

    if (guard != &proc->ef.lock)
        exec_unlock(guard);
    exec_unlock(&proc->ef.lock);

    args.status = prev ? SS__WASSET : SS__WASCLR;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_waitfr - Wait for single event flag
 */
long vms_ioctl_waitfr(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        /* An out-of-range flag number is ILLEFC, not UNASEFC: 200 is not
         * "a common cluster you have not associated with", it is not an
         * event flag at all. Matches SETEF/CLREF above. */
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /*
     * Move onto the predicate's guard lock for the wait. A common cluster's
     * guard is its own lock, which the setter also holds across set+wake, so
     * the two sides share a lock; drop proc->ef.lock first so we never sleep
     * holding it. For a local flag the guard already IS proc->ef.lock, so we
     * keep holding it. The resolved cluster stays alive because this process
     * remains associated with it for the duration of the wait.
     */
    if (guard != &proc->ef.lock) {
        exec_unlock(&proc->ef.lock);
        exec_lock(guard);
    }

    /*
     * Wait until the flag is set. The predicate is re-tested under the guard
     * on every wake and has PRIORITY over an interrupt (matching
     * wait_event_interruptible, which checks the condition before the signal
     * test): a wait that ends with its flag set returns SS$_NORMAL even if a
     * signal is also pending.
     *
     * A SIGNAL WITH THE FLAG STILL CLEAR DOES NOT END THE WAIT, AND PRODUCES
     * NO STATUS. See the INTERRUPTED WAITS note at the top of this file. This
     * path used to write SS$_NORMAL for an interrupted wait, telling the
     * caller "the flag is set" about a flag that was demonstrably still clear.
     * -ERESTARTSYS is returned instead with NO status written, so there is
     * nothing for a caller to misread, and libvmssys' vms_kif_waitfr()
     * re-enters the wait. Semantics unchanged from the
     * wait_event_interruptible() this replaces.
     */
    for (;;) {
        if (READ_ONCE(*flags) & (1U << bit))
            break;
        if (exec_cv_wait(waitcv, guard)) {
            if (READ_ONCE(*flags) & (1U << bit))
                break;
            exec_unlock(guard);
            return -ERESTARTSYS;
        }
    }
    exec_unlock(guard);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_wflor - Wait for any flag in mask (OR wait)
 *
 * The EFN specifies the cluster base (0, 32, 64, or 96).
 * The mask specifies which flags within that cluster to check.
 * Returns when ANY of the masked flags are set.
 */
long vms_ioctl_wflor(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_wait_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    /*
     * NO "efn must be a cluster base" check. ORACLE-PINNED (vms-2a8):
     * HELP SYSTEM_SERVICES $WFLOR Arguments on the reference lab VAX V7.3
     * says the efn argument is the "Number of any event flag within the
     * event flag cluster to be used ... Specifying the number of an event
     * flag within the cluster serves to identify the event flag cluster."
     * This rejected everything but 0/32/64/96 with SS$_ILLEFC, so a wait
     * on cluster 1 expressed as $WFLOR(40, mask) -- legal VMS -- was an
     * illegal event flag cluster. efn_resolve() already selects the
     * cluster from any flag number in it.
     */
    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /* Move onto the predicate's guard lock -- see vms_ioctl_waitfr above. */
    if (guard != &proc->ef.lock) {
        exec_unlock(&proc->ef.lock);
        exec_lock(guard);
    }

    /* Return when ANY masked flag is set. Predicate has priority over an
     * interrupt; no status on the interrupted path -- see vms_ioctl_waitfr
     * above and the INTERRUPTED WAITS note at the top of this file. */
    for (;;) {
        if (READ_ONCE(*flags) & args.mask)
            break;
        if (exec_cv_wait(waitcv, guard)) {
            if (READ_ONCE(*flags) & args.mask)
                break;
            exec_unlock(guard);
            return -ERESTARTSYS;
        }
    }
    exec_unlock(guard);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_wfland - Wait for all flags in mask (AND wait)
 */
long vms_ioctl_wfland(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_wait_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    /* No cluster-base check -- see the note in vms_ioctl_wflor above. */
    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /* Move onto the predicate's guard lock -- see vms_ioctl_waitfr above. */
    if (guard != &proc->ef.lock) {
        exec_unlock(&proc->ef.lock);
        exec_lock(guard);
    }

    /* Return when ALL masked flags are set. Predicate has priority over an
     * interrupt; no status on the interrupted path -- see vms_ioctl_waitfr
     * above and the INTERRUPTED WAITS note at the top of this file. */
    for (;;) {
        if ((READ_ONCE(*flags) & args.mask) == args.mask)
            break;
        if (exec_cv_wait(waitcv, guard)) {
            if ((READ_ONCE(*flags) & args.mask) == args.mask)
                break;
            exec_unlock(guard);
            return -ERESTARTSYS;
        }
    }
    exec_unlock(guard);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_readef - Read event flag cluster state
 */
long vms_ioctl_readef(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_read_args args;
    uint32_t *flags;
    exec_cv_t *waitcv;
    exec_lock_t *guard;
    int bit;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitcv, &bit, &guard) < 0) {
        exec_unlock(&proc->ef.lock);
        args.state = 0;
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    /* Read the flag word under its guard (proc->ef.lock for local; the
     * cluster's own lock, nested, for a common cluster) so the read is
     * consistent with a concurrent setef/clref on the same word. */
    if (guard != &proc->ef.lock)
        exec_lock(guard);
    args.state = *flags;
    if (guard != &proc->ef.lock)
        exec_unlock(guard);
    exec_unlock(&proc->ef.lock);

    /* Return WASSET/WASCLR based on the specific flag */
    args.status = (args.state & (1U << bit)) ? SS__WASSET : SS__WASCLR;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_ascefc - Associate with common event flag cluster
 *
 * Associates the process with a named common event flag cluster.
 * If the cluster doesn't exist, it's created. The EFN must be
 * 64 (cluster 2) or 96 (cluster 3).
 */
long vms_ioctl_ascefc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_common_args args;
    struct vms_common_ef_cluster *cluster;
    int idx;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    idx = common_idx(args.efn);
    if (idx < 0) {
        args.status = SS__ILLEFC;
        goto out;
    }

    /* Ensure null termination */
    args.name[31] = '\0';

    /* Search for existing cluster */
    exec_lock(&vms_common_ef_lock);
    list_for_each_entry(cluster, &vms_common_ef_list, list) {
        if (strncmp(cluster->name, args.name, 32) == 0) {
            /* Found it -- associate */
            cluster->refcount++;
            exec_unlock(&vms_common_ef_lock);

            exec_lock(&proc->ef.lock);
            /* Release old association if any */
            if (proc->ef.common[idx]) {
                struct vms_common_ef_cluster *old = proc->ef.common[idx];
                exec_lock(&vms_common_ef_lock);
                old->refcount--;
                if (old->refcount <= 0 && !old->perm) {
                    list_del(&old->list);
                    exec_free(old);
                }
                exec_unlock(&vms_common_ef_lock);
            }
            proc->ef.common[idx] = cluster;
            exec_unlock(&proc->ef.lock);

            args.status = SS__NORMAL;
            goto out;
        }
    }

    /* Create new cluster. Allocated while holding vms_common_ef_lock, so the
     * allocation must NOT sleep -- exec_zalloc_atomic (Linux GFP_ATOMIC). */
    cluster = exec_zalloc_atomic(sizeof(*cluster));
    if (!cluster) {
        exec_unlock(&vms_common_ef_lock);
        args.status = SS__INSFMEM;
        goto out;
    }

    strscpy(cluster->name, args.name, sizeof(cluster->name));
    cluster->flags = 0;
    cluster->prot = args.prot;
    cluster->perm = args.perm;
    cluster->refcount = 1;
    exec_cv_init(&cluster->waitq);
    exec_lock_init(&cluster->lock);
    list_add_tail(&cluster->list, &vms_common_ef_list);
    exec_unlock(&vms_common_ef_lock);

    exec_lock(&proc->ef.lock);
    proc->ef.common[idx] = cluster;
    exec_unlock(&proc->ef.lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_dacefc - Disassociate from common event flag cluster
 */
long vms_ioctl_dacefc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_args args;
    int idx;
    struct vms_common_ef_cluster *cluster;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    idx = common_idx(args.efn);
    if (idx < 0) {
        args.status = SS__ILLEFC;
        goto out;
    }

    exec_lock(&proc->ef.lock);
    cluster = proc->ef.common[idx];
    if (!cluster) {
        exec_unlock(&proc->ef.lock);
        args.status = SS__UNASEFC;
        goto out;
    }

    proc->ef.common[idx] = NULL;
    exec_unlock(&proc->ef.lock);

    exec_lock(&vms_common_ef_lock);
    cluster->refcount--;
    if (cluster->refcount <= 0 && !cluster->perm) {
        list_del(&cluster->list);
        exec_free(cluster);
    }
    exec_unlock(&vms_common_ef_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_dlcefc - Mark a permanent common event flag cluster for deletion
 *
 * ORACLE-PINNED (vms-2a8), docs/oracle/vax73-event-flags.md. HELP
 * SYSTEM_SERVICES $DLCEFC on the reference lab VAX V7.3:
 *
 *   "Marks a permanent common event flag cluster for deletion."
 *
 * "Marks ... for deletion" is the whole semantic: the cluster is not torn
 * out from under the processes still associated with it. Clearing the
 * permanent bit puts it back under the ordinary temporary-cluster lifetime
 * this file already implements -- freed by DACEFC (or by process teardown,
 * vms_proc_release_common_ef) when the last association goes away -- and if
 * nobody is associated any more it goes immediately.
 *
 * This ioctl did not exist. sys$dlcefc in src/libvms/syssvc/sys_event.c was
 * `return SS$_NORMAL;` with no side effect: a caller was told its permanent
 * cluster had been marked for deletion, and nothing had happened. That is
 * the fabricated success Rule 10 forbids, and it is the same defect as
 * sys$ascefc's, in the same file.
 *
 * The cluster is named, not numbered: $DLCEFC takes only a name, so it can
 * delete a cluster this process never associated with.
 */
long vms_ioctl_dlcefc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ef_common_args args;
    struct vms_common_ef_cluster *cluster, *tmp;

    (void)proc;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    args.name[31] = '\0';

    args.status = SS__UNASEFC;

    exec_lock(&vms_common_ef_lock);
    list_for_each_entry_safe(cluster, tmp, &vms_common_ef_list, list) {
        if (strncmp(cluster->name, args.name, 32) != 0)
            continue;

        cluster->perm = 0;
        if (cluster->refcount <= 0) {
            list_del(&cluster->list);
            exec_free(cluster);
        }
        args.status = SS__NORMAL;
        break;
    }
    exec_unlock(&vms_common_ef_lock);

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
