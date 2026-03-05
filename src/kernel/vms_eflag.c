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
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "vms_internal.h"

/* VMS status codes */
#define SS__NORMAL      0x00000001
#define SS__WASSET      9
#define SS__WASCLR      5
#define SS__ILLEFC      44  /* illegal event flag number */
#define SS__BADPARAM    20
#define SS__UNASEFC     48  /* unassociated common EFC */
#define SS__INSFMEM     20

/* Global common event flag cluster list */
LIST_HEAD(vms_common_ef_list);
DEFINE_SPINLOCK(vms_common_ef_lock);

void vms_eflag_init(void)
{
    /* Nothing extra needed -- list is statically initialized */
}

void vms_eflag_cleanup(void)
{
    struct vms_common_ef_cluster *cluster, *tmp;

    spin_lock(&vms_common_ef_lock);
    list_for_each_entry_safe(cluster, tmp, &vms_common_ef_list, list) {
        list_del(&cluster->list);
        kfree(cluster);
    }
    spin_unlock(&vms_common_ef_lock);
}

/*
 * Release common EF associations for a process being freed
 */
void vms_proc_release_common_ef(struct vms_proc *proc)
{
    int i;

    spin_lock(&proc->ef.lock);
    for (i = 0; i < 2; i++) {
        struct vms_common_ef_cluster *cluster = proc->ef.common[i];
        if (cluster) {
            spin_lock(&vms_common_ef_lock);
            cluster->refcount--;
            if (cluster->refcount <= 0 && !cluster->perm) {
                list_del(&cluster->list);
                kfree(cluster);
            }
            spin_unlock(&vms_common_ef_lock);
            proc->ef.common[i] = NULL;
        }
    }
    spin_unlock(&proc->ef.lock);
}

/*
 * Helper: get pointer to flag word and wait queue for a given EFN
 * Returns 0 on success, fills *flags_ptr, *waitq, *bit
 */
static int efn_resolve(struct vms_proc *proc, uint32_t efn,
                       uint32_t **flags_ptr, wait_queue_head_t **waitq,
                       int *bit)
{
    if (efn < 32) {
        *flags_ptr = &proc->ef.local[0];
        *waitq = &proc->ef.waitq;
        *bit = efn;
        return 0;
    } else if (efn < 64) {
        *flags_ptr = &proc->ef.local[1];
        *waitq = &proc->ef.waitq;
        *bit = efn - 32;
        return 0;
    } else if (efn < 96) {
        struct vms_common_ef_cluster *c = proc->ef.common[0];
        if (!c) return -1;
        *flags_ptr = &c->flags;
        *waitq = &c->waitq;
        *bit = efn - 64;
        return 0;
    } else if (efn < 128) {
        struct vms_common_ef_cluster *c = proc->ef.common[1];
        if (!c) return -1;
        *flags_ptr = &c->flags;
        *waitq = &c->waitq;
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
    wait_queue_head_t *waitq;
    int bit;
    uint32_t prev;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    prev = *flags & (1U << bit);
    *flags |= (1U << bit);
    spin_unlock(&proc->ef.lock);

    /* Wake any waiters */
    wake_up_interruptible(waitq);

    args.status = prev ? SS__WASSET : SS__WASCLR;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    wait_queue_head_t *waitq;
    int bit;
    uint32_t prev;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.status = (args.efn >= 128) ? SS__ILLEFC :
                      (args.efn >= 64)  ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    prev = *flags & (1U << bit);
    *flags &= ~(1U << bit);
    spin_unlock(&proc->ef.lock);

    args.status = prev ? SS__WASSET : SS__WASCLR;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    wait_queue_head_t *waitq;
    int bit;
    int ret;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.status = (args.efn >= 64) ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }
    spin_unlock(&proc->ef.lock);

    /* Wait until the flag is set */
    ret = wait_event_interruptible(*waitq, (*flags & (1U << bit)));
    if (ret) {
        args.status = SS__NORMAL; /* interrupted, but still return normally */
        goto out;
    }

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    wait_queue_head_t *waitq;
    int bit;
    int ret;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    /* EFN must be cluster base */
    if (args.efn != 0 && args.efn != 32 && args.efn != 64 && args.efn != 96) {
        args.status = SS__ILLEFC;
        goto out;
    }

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.status = (args.efn >= 64) ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }
    spin_unlock(&proc->ef.lock);

    ret = wait_event_interruptible(*waitq, (*flags & args.mask));
    if (ret) {
        args.status = SS__NORMAL;
        goto out;
    }

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    wait_queue_head_t *waitq;
    int bit;
    int ret;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.efn != 0 && args.efn != 32 && args.efn != 64 && args.efn != 96) {
        args.status = SS__ILLEFC;
        goto out;
    }

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.status = (args.efn >= 64) ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }
    spin_unlock(&proc->ef.lock);

    ret = wait_event_interruptible(*waitq,
                                   ((*flags & args.mask) == args.mask));
    if (ret) {
        args.status = SS__NORMAL;
        goto out;
    }

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    wait_queue_head_t *waitq;
    int bit;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->ef.lock);
    if (efn_resolve(proc, args.efn, &flags, &waitq, &bit) < 0) {
        spin_unlock(&proc->ef.lock);
        args.state = 0;
        args.status = (args.efn >= 64) ? SS__UNASEFC : SS__ILLEFC;
        goto out;
    }

    args.state = *flags;
    spin_unlock(&proc->ef.lock);

    /* Return WASSET/WASCLR based on the specific flag */
    args.status = (args.state & (1U << bit)) ? SS__WASSET : SS__WASCLR;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.efn == 64)
        idx = 0;
    else if (args.efn == 96)
        idx = 1;
    else {
        args.status = SS__ILLEFC;
        goto out;
    }

    /* Ensure null termination */
    args.name[31] = '\0';

    /* Search for existing cluster */
    spin_lock(&vms_common_ef_lock);
    list_for_each_entry(cluster, &vms_common_ef_list, list) {
        if (strncmp(cluster->name, args.name, 32) == 0) {
            /* Found it -- associate */
            cluster->refcount++;
            spin_unlock(&vms_common_ef_lock);

            spin_lock(&proc->ef.lock);
            /* Release old association if any */
            if (proc->ef.common[idx]) {
                struct vms_common_ef_cluster *old = proc->ef.common[idx];
                spin_lock(&vms_common_ef_lock);
                old->refcount--;
                if (old->refcount <= 0 && !old->perm) {
                    list_del(&old->list);
                    kfree(old);
                }
                spin_unlock(&vms_common_ef_lock);
            }
            proc->ef.common[idx] = cluster;
            spin_unlock(&proc->ef.lock);

            args.status = SS__NORMAL;
            goto out;
        }
    }

    /* Create new cluster */
    cluster = kzalloc(sizeof(*cluster), GFP_ATOMIC);
    if (!cluster) {
        spin_unlock(&vms_common_ef_lock);
        args.status = SS__INSFMEM;
        goto out;
    }

    strscpy(cluster->name, args.name, sizeof(cluster->name));
    cluster->flags = 0;
    cluster->prot = args.prot;
    cluster->perm = args.perm;
    cluster->refcount = 1;
    init_waitqueue_head(&cluster->waitq);
    spin_lock_init(&cluster->lock);
    list_add_tail(&cluster->list, &vms_common_ef_list);
    spin_unlock(&vms_common_ef_lock);

    spin_lock(&proc->ef.lock);
    proc->ef.common[idx] = cluster;
    spin_unlock(&proc->ef.lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.efn == 64)
        idx = 0;
    else if (args.efn == 96)
        idx = 1;
    else {
        args.status = SS__ILLEFC;
        goto out;
    }

    spin_lock(&proc->ef.lock);
    cluster = proc->ef.common[idx];
    if (!cluster) {
        spin_unlock(&proc->ef.lock);
        args.status = SS__UNASEFC;
        goto out;
    }

    proc->ef.common[idx] = NULL;
    spin_unlock(&proc->ef.lock);

    spin_lock(&vms_common_ef_lock);
    cluster->refcount--;
    if (cluster->refcount <= 0 && !cluster->perm) {
        list_del(&cluster->list);
        kfree(cluster);
    }
    spin_unlock(&vms_common_ef_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
