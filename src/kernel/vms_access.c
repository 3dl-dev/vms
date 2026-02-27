// SPDX-License-Identifier: GPL-2.0
/*
 * vms_access.c - Access mode enforcement (Phase 3a)
 *
 * Implements VMS access modes (kernel/exec/super/user) with
 * kernel-enforced mode transitions and privilege checking.
 *
 * Key VMS semantics:
 *   - Mode transitions to more privileged modes require CMKRNL/CMEXEC
 *   - Privilege changes require appropriate mode or SETPRV privilege
 *   - Privilege state is stored in kernel memory, not userspace
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "vms_internal.h"

/* VMS privilege bits (matching PRV$M_* in starlet.h) */
#define PRV_M_CMKRNL    (1ULL << 0)
#define PRV_M_CMEXEC    (1ULL << 1)
#define PRV_M_SETPRV    (1ULL << 5)

/* Status codes are in vms_internal.h */

/*
 * vms_ioctl_setmode - Set access mode (VMS $SETMOD equivalent)
 *
 * Transitions the process to a new access mode. Moving to a more
 * privileged mode requires the appropriate privilege:
 *   - To kernel mode: PRV$M_CMKRNL
 *   - To exec mode: PRV$M_CMEXEC (or CMKRNL)
 *   - To super mode: from user only, no special priv (less privileged)
 *   - To user mode: always allowed (least privileged)
 *
 * Actually in VMS, you can only go to more privileged modes with
 * change-mode instructions. Going to less privileged is via REI.
 * We enforce: you can go to equal or more privileged if you have
 * the right privilege. Going less privileged is always OK.
 */
long vms_ioctl_setmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mode_args args;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.mode > PSL_C_USER) {
        args.status = SS__BADPARAM;
        goto out;
    }

    spin_lock(&proc->mode_lock);

    /* Check if we're going to a more privileged mode */
    if (args.mode < proc->current_mode) {
        /* More privileged = lower number */
        if (args.mode == PSL_C_KERNEL) {
            if (!(proc->cur_privs & PRV_M_CMKRNL)) {
                spin_unlock(&proc->mode_lock);
                args.status = SS__NOPRIV;
                goto out;
            }
        } else if (args.mode == PSL_C_EXEC) {
            if (!(proc->cur_privs & (PRV_M_CMEXEC | PRV_M_CMKRNL))) {
                spin_unlock(&proc->mode_lock);
                args.status = SS__NOPRIV;
                goto out;
            }
        }
        /* super mode (2) from user (3) is OK without special priv */
    }

    proc->current_mode = args.mode;
    spin_unlock(&proc->mode_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_getmode - Get current access mode and privileges
 */
long vms_ioctl_getmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getmode_args args;

    spin_lock(&proc->mode_lock);
    args.mode = proc->current_mode;
    args.cur_privs = proc->cur_privs;
    args.perm_privs = proc->perm_privs;
    spin_unlock(&proc->mode_lock);

    /* Zero padding */
    args.pad[0] = args.pad[1] = args.pad[2] = 0;

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_setprv - Set/clear privileges ($SETPRV equivalent)
 *
 * VMS semantics:
 *   - Enabling privileges requires SETPRV or being in kernel mode
 *   - Disabling privileges is always allowed
 *   - If 'permanent' flag is set, changes permanent mask too
 *   - Returns previous privilege mask
 */
long vms_ioctl_setprv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_priv_args args;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->mode_lock);

    /* Save previous state */
    args.prev = proc->cur_privs;

    if (args.enable) {
        /* Enabling privileges requires SETPRV or kernel mode */
        if (proc->current_mode != PSL_C_KERNEL &&
            !(proc->cur_privs & PRV_M_SETPRV)) {
            /* Can only re-enable permanent privileges */
            uint64_t allowed = args.mask & proc->perm_privs;
            proc->cur_privs |= allowed;
            if (allowed != args.mask) {
                spin_unlock(&proc->mode_lock);
                args.status = SS__NOPRIV;
                goto out;
            }
        } else {
            proc->cur_privs |= args.mask;
        }

        if (args.permanent)
            proc->perm_privs |= args.mask;
    } else {
        /* Disabling is always allowed */
        proc->cur_privs &= ~args.mask;
        if (args.permanent)
            proc->perm_privs &= ~args.mask;
    }

    spin_unlock(&proc->mode_lock);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_chkpriv - Check if privileges are held
 *
 * Returns SS$_NORMAL if all requested privileges are held,
 * SS$_NOPRIV otherwise. Does not modify anything.
 */
long vms_ioctl_chkpriv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_priv_args args;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->mode_lock);
    args.prev = proc->cur_privs;

    if ((proc->cur_privs & args.mask) == args.mask)
        args.status = SS__NORMAL;
    else
        args.status = SS__NOPRIV;

    spin_unlock(&proc->mode_lock);

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
