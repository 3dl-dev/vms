// SPDX-License-Identifier: GPL-2.0
/*
 * vms_proctab.c - Executive-resident process table (vms-8019)
 *
 * On OpenVMS the process name is not a property a process keeps to
 * itself: it lives in the executive's process database. $SETPRN writes
 * it there, $GETJPI resolves a process by it, and SHOW SYSTEM
 * enumerates the database. That shared residency is the entire meaning
 * of a VMS process name -- a name only this process can see is not a
 * process name at all.
 *
 * This file gives OVMX the same property. The name is stored in
 * struct vms_proc, which lives in kernel memory and is keyed by the
 * Linux pid. Because execve() does not change the pid, the name
 * survives image activation with no userspace carrier of any kind.
 *
 * Scoping follows VMS: a process name is unique within, and resolved
 * within, the caller's UIC group (OpenVMS System Services Reference,
 * $SETPRN and $GETJPI). OVMX maps UIC [group,member] onto the task's
 * [gid,uid], the same packing sys$getjpi's JPI$_UIC item returns.
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/pid.h>

#include "vms_internal.h"

/* Serializes reaping so two callers cannot claim the same victim. */
static DEFINE_MUTEX(vms_reap_mutex);

/*
 * uic_group - the [group] half of a packed UIC.
 *
 * VMS scopes process-name uniqueness and lookup to the UIC group;
 * everything in this file that compares names compares groups first.
 */
static inline uint32_t uic_group(uint32_t uic)
{
    return uic >> 16;
}

/*
 * vms_proc_task_alive - does the task backing this entry still exist?
 *
 * pid_task() returns NULL once the task has been released, which is
 * the point at which the VMS process has ceased to exist and its slot
 * may be reused.
 */
static bool vms_proc_task_alive(struct vms_proc *proc)
{
    struct task_struct *task;

    if (!proc->pid_ref)
        return false;

    rcu_read_lock();
    task = pid_task(proc->pid_ref, PIDTYPE_PID);
    rcu_read_unlock();

    return task != NULL;
}

/*
 * vms_proc_reap_dead - remove entries whose process has exited.
 *
 * The PCB is owned by the process, not by an open /dev/vms channel, so
 * closing the channel does not delete it (see vms_dev_release). Entries
 * are therefore reclaimed here, lazily, on every operation that reads
 * or mutates the table.
 *
 * One victim per pass: the teardown below the hash removal can sleep on
 * nothing but must not run under the hash spinlock, so the scan unlinks
 * one entry and then leaves the lock to tear it down. The table is small
 * (VMS SHOW SYSTEM walks the PCB vector linearly too) and reaping only
 * runs on process table operations.
 *
 * The victim is UNLINKED WHILE THE LOCK IS STILL HELD, and that removal
 * is the ownership claim (see vms_proc_free()). Selecting a victim under
 * the lock and only claiming it afterwards would be a use-after-free: a
 * concurrent vms_dev_release() could claim and kfree_rcu() the same
 * entry in the gap, and the claim itself reads proc->hash_node.
 */
void vms_proc_reap_dead(void)
{
    struct vms_proc *proc, *victim;
    struct hlist_node *tmp;
    int bkt;

    mutex_lock(&vms_reap_mutex);

    for (;;) {
        victim = NULL;

        spin_lock(&vms_proc_hash_lock);
        hash_for_each_safe(vms_proc_hash, bkt, tmp, proc, hash_node) {
            if (!vms_proc_task_alive(proc)) {
                hash_del_rcu(&proc->hash_node);
                victim = proc;
                break;
            }
        }
        spin_unlock(&vms_proc_hash_lock);

        if (!victim)
            break;

        vms_proc_free_claimed(victim);
    }

    mutex_unlock(&vms_reap_mutex);
}

/*
 * proc_fill_info - snapshot one table row.
 *
 * Called with vms_proc_hash_lock held: the caller copies to userspace
 * after dropping the lock, since copy_to_user() may sleep.
 */
static void proc_fill_info(const struct vms_proc *proc,
                           struct vms_procinfo *info)
{
    memset(info, 0, sizeof(*info));
    info->vms_pid      = proc->vms_pid;
    info->linux_pid    = (uint32_t)proc->linux_pid;
    info->uic          = proc->uic;
    info->current_mode = proc->current_mode;
    info->cur_privs    = proc->cur_privs;
    memcpy(info->prcnam, proc->prcnam, VMS_PRCNAM_SIZE);
    info->prcnam[VMS_PRCNAM_SIZE - 1] = '\0';
}

/*
 * name_is_valid - reject malformed name strings from userspace.
 *
 * This is a trust-boundary check on an untrusted buffer AND the length
 * rule VMS enforces: the executive must never index a string that is
 * not NUL-terminated inside the buffer it was given, and a name that
 * does not fit in VMS_PRCNAM_SIZE is not a legal VMS process name. A
 * zero-length name is also rejected -- an unnamed process is expressed
 * by never calling SETPRN, not by setting the empty name.
 *
 * name is an inbound VMS_PRCNAM_XFER buffer, so an oversized name is
 * VISIBLE here (no NUL within the first VMS_PRCNAM_SIZE bytes) and is
 * rejected rather than truncated -- which is what the oracle does; see
 * the VMS_PRCNAM_XFER comment in vms_ioctl.h for the transcript.
 */
static bool name_is_valid(const char *name)
{
    size_t i;

    for (i = 0; i < VMS_PRCNAM_SIZE; i++) {
        if (name[i] == '\0')
            return i > 0;
    }
    return false;
}

/*
 * find_by_name - locate a live process by name within a UIC group.
 *
 * Caller must hold vms_proc_hash_lock. Name comparison is exact; VMS
 * process names are stored as given (DCL upcases them before they get
 * here, as it does for every other unquoted DCL token).
 */
static struct vms_proc *find_by_name(uint32_t group, const char *name)
{
    struct vms_proc *proc;
    int bkt;

    hash_for_each(vms_proc_hash, bkt, proc, hash_node) {
        if (uic_group(proc->uic) != group)
            continue;
        if (proc->prcnam[0] == '\0')
            continue;
        if (strncmp(proc->prcnam, name, VMS_PRCNAM_SIZE) == 0)
            return proc;
    }
    return NULL;
}

/*
 * find_by_vms_pid - locate a process by its VMS process ID.
 *
 * Caller must hold vms_proc_hash_lock.
 */
static struct vms_proc *find_by_vms_pid(uint32_t vms_pid)
{
    struct vms_proc *proc;
    int bkt;

    hash_for_each(vms_proc_hash, bkt, proc, hash_node) {
        if (proc->vms_pid == vms_pid)
            return proc;
    }
    return NULL;
}

/*
 * vms_ioctl_setprn - set the calling process's name ($SETPRN).
 *
 * VMS returns SS$_DUPLNAM when the name is already in use within the
 * UIC group; the name is otherwise recorded in the executive, where
 * every other process can see it.
 */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setprn_args args;
    struct vms_proc *clash;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (!name_is_valid(args.prcnam)) {
        args.status = SS__IVLOGNAM;
        goto out;
    }

    /* A name freed by an exited process must be available again. */
    vms_proc_reap_dead();

    spin_lock(&vms_proc_hash_lock);
    clash = find_by_name(uic_group(proc->uic), args.prcnam);
    if (clash && clash != proc) {
        spin_unlock(&vms_proc_hash_lock);
        args.status = SS__DUPLNAM;
        goto out;
    }
    memcpy(proc->prcnam, args.prcnam, VMS_PRCNAM_SIZE);
    proc->prcnam[VMS_PRCNAM_SIZE - 1] = '\0';
    spin_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_getjpi - resolve a process and return its table row.
 *
 * Selects the caller, a process by VMS PID, or -- the case that makes
 * a process name mean anything -- a process by name within the
 * caller's UIC group. VMS returns SS$_NONEXPR when no such process
 * exists.
 */
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getjpi_args args;
    struct vms_proc *target;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.select == VMS_JPI_SEL_PRCNAM && !name_is_valid(args.sel_prcnam)) {
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__IVLOGNAM;
        goto out;
    }

    vms_proc_reap_dead();

    spin_lock(&vms_proc_hash_lock);
    switch (args.select) {
    case VMS_JPI_SEL_SELF:
        target = proc;
        break;
    case VMS_JPI_SEL_PID:
        target = find_by_vms_pid(args.info.vms_pid);
        break;
    case VMS_JPI_SEL_PRCNAM:
        target = find_by_name(uic_group(proc->uic), args.sel_prcnam);
        break;
    default:
        target = NULL;
        spin_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__BADPARAM;
        goto out;
    }

    if (!target) {
        spin_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NONEXPR;
        goto out;
    }

    proc_fill_info(target, &args.info);
    spin_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_procscan - enumerate the process table (SHOW SYSTEM's reader).
 *
 * Returns the row at the incoming cursor and advances it. The scan ends
 * with SS$_NONEXPR, matching what $PROCESS_SCAN returns once a wildcard
 * search is exhausted.
 *
 * The cursor is an ordinal over the hash walk, so a row can be missed or
 * repeated if the table changes mid-scan. VMS has the same property --
 * SHOW SYSTEM is a sample of a live system, not a transaction.
 */
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg)
{
    struct vms_procscan_args args;
    struct vms_proc *cur, *target = NULL;
    uint32_t ordinal = 0;
    int bkt;

    (void)proc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    vms_proc_reap_dead();

    spin_lock(&vms_proc_hash_lock);
    hash_for_each(vms_proc_hash, bkt, cur, hash_node) {
        if (ordinal == args.index) {
            target = cur;
            break;
        }
        ordinal++;
    }

    if (!target) {
        spin_unlock(&vms_proc_hash_lock);
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NONEXPR;
        goto out;
    }

    proc_fill_info(target, &args.info);
    spin_unlock(&vms_proc_hash_lock);

    args.index++;
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
