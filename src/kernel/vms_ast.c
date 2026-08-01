// SPDX-License-Identifier: GPL-2.0
/*
 * vms_ast.c - 4-Level AST Delivery (Phase 3b)
 *
 * Implements VMS Asynchronous System Traps (ASTs) with four priority
 * levels corresponding to access modes:
 *   - Kernel mode ASTs (highest priority, preempt all)
 *   - Exec mode ASTs
 *   - Super mode ASTs
 *   - User mode ASTs (lowest priority)
 *
 * ASTs are queued in kernel memory per access mode. Delivery is
 * triggered by the userspace runtime via VMS_IOCTL_DELIVERAST,
 * which returns the highest-priority pending AST for execution.
 *
 * An AST at a more privileged mode preempts ASTs at less privileged
 * modes. Each mode can independently enable/disable delivery.
 *
 * Delivery mechanism:
 *   Userspace calls DELIVERAST ioctl. Kernel picks the highest-priority
 *   enabled AST and returns {astadr, astprm, acmode}. Userspace runtime
 *   dispatches the call. This avoids signal-based delivery (no SIGUSR1).
 *
 *   For interrupt-driven delivery, the kernel can also send a real-time
 *   signal (SIGRTMIN+mode) to wake the process if it's blocked.
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>

#include "vms_internal.h"

/* Privilege bits */
#define PRV_M_CMKRNL    (1ULL << 0)
#define PRV_M_CMEXEC    (1ULL << 1)

/*
 * vms_ioctl_dclast - Declare AST ($DCLAST equivalent)
 *
 * Queues an AST for delivery at the specified access mode.
 * The AST is not delivered immediately -- it waits until
 * DELIVERAST is called and the mode queue is enabled.
 *
 * Access mode checking: you can only declare ASTs at your
 * current mode or less privileged. Declaring at a more
 * privileged mode requires CMKRNL/CMEXEC.
 */
long vms_ioctl_dclast(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ast_args args;
    struct vms_ast_entry *entry;
    struct vms_ast_state *ast_state;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.acmode > PSL_C_USER) {
        args.status = SS__BADPARAM;
        goto out;
    }

    if (args.astadr == 0) {
        args.status = SS__BADPARAM;
        goto out;
    }

    /* Check access mode privilege */
    spin_lock(&proc->mode_lock);
    if (args.acmode < proc->current_mode) {
        /* Declaring at more privileged mode */
        if (args.acmode == PSL_C_KERNEL && !(proc->cur_privs & PRV_M_CMKRNL)) {
            spin_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
        if (args.acmode == PSL_C_EXEC && !(proc->cur_privs & (PRV_M_CMEXEC | PRV_M_CMKRNL))) {
            spin_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
    }
    spin_unlock(&proc->mode_lock);

    ast_state = &proc->ast[args.acmode];

    /* Allocate before taking spinlock to avoid GFP_KERNEL in atomic context */
    entry = kmalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        args.status = SS__INSFMEM;
        goto out;
    }

    /* Check quota */
    spin_lock(&ast_state->lock);
    if (ast_state->count >= VMS_AST_MAX_PER_MODE) {
        spin_unlock(&ast_state->lock);
        kfree(entry);
        args.status = SS__EXASTLM;
        goto out;
    }

    entry->astadr = args.astadr;
    entry->astprm = args.astprm;
    entry->acmode = args.acmode;
    list_add_tail(&entry->list, &ast_state->pending);
    ast_state->count++;
    spin_unlock(&ast_state->lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_setast - Enable/disable AST delivery ($SETAST equivalent)
 *
 * Enables or disables AST delivery for the current access mode.
 * Returns whether ASTs were previously enabled (SS$_WASSET) or
 * disabled (SS$_WASCLR).
 */
long vms_ioctl_setast(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setast_args args;
    struct vms_ast_state *ast_state;
    uint8_t mode;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->mode_lock);
    mode = proc->current_mode;
    spin_unlock(&proc->mode_lock);

    ast_state = &proc->ast[mode];

    spin_lock(&ast_state->lock);
    args.prev_state = ast_state->enabled;
    ast_state->enabled = args.enable ? 1 : 0;
    spin_unlock(&ast_state->lock);

    args.status = args.prev_state ? SS__WASSET : SS__WASCLR;

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_deliverast - Deliver next pending AST
 *
 * Called by the userspace runtime to fetch the next deliverable AST.
 * Scans from kernel mode (0) to user mode (3), returning the first
 * AST from an enabled queue. The AST is removed from the kernel queue.
 *
 * Returns the AST entry via the shared vms_ast_args structure.
 * If no ASTs are pending, returns status indicating nothing to deliver.
 *
 * This is the polling interface. The kernel can also proactively
 * notify via signals when ASTs are queued (future enhancement).
 */
long vms_ioctl_deliverast(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ast_args args;
    struct vms_ast_entry *entry;
    int mode;

    memset(&args, 0, sizeof(args));

    /* Scan from most privileged to least */
    for (mode = PSL_C_KERNEL; mode <= PSL_C_USER; mode++) {
        struct vms_ast_state *ast_state = &proc->ast[mode];

        spin_lock(&ast_state->lock);
        if (!ast_state->enabled || list_empty(&ast_state->pending)) {
            spin_unlock(&ast_state->lock);
            continue;
        }

        /* Dequeue first entry */
        entry = list_first_entry(&ast_state->pending,
                                 struct vms_ast_entry, list);
        list_del(&entry->list);
        ast_state->count--;
        spin_unlock(&ast_state->lock);

        /* Return to userspace */
        args.astadr = entry->astadr;
        args.astprm = entry->astprm;
        args.acmode = entry->acmode;
        args.status = SS__NORMAL;
        kfree(entry);

        /* VMS_IOCTL_DELIVERAST is _IOR: `arg` is a userspace pointer to a
         * struct vms_ast_args that receives the delivered AST entry. Return
         * 0 to signal "an AST was delivered"; the caller reads astadr/astprm/
         * acmode from the buffer. */
        if (copy_to_user((void __user *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    /* No ASTs to deliver */
    return -EAGAIN;
}
