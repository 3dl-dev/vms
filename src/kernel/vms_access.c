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

/* PRV_M_* privilege bits come from vms_ioctl.h (oracle-pinned, shared with
 * userspace). They are deliberately NOT redefined here -- a local copy is
 * how PRV_M_SETPRV came to be bit 5 (DETACH) instead of bit 14. */

/* VMS status codes come from vms_internal.h (canonical, oracle-pinned) */

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

    memset(&args, 0, sizeof(args));
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
 * VMS semantics (observed on the reference lab, see SS__NOTALLPRIV in
 * vms_internal.h):
 *   - Disabling privileges is always allowed.
 *   - Enabling privileges the caller is AUTHORIZED for (i.e. already in the
 *     permanent/authorized mask) is always allowed -- that is how a process
 *     re-enables a privilege it turned off, and it needs no SETPRV.
 *   - Enabling privileges OUTSIDE the authorized mask requires SETPRV, or
 *     kernel mode. The subset that IS authorized is still enabled, and the
 *     call returns SS$_NOTALLPRIV rather than enabling nothing.
 *   - WIDENING the permanent (authorized) mask is a strictly privileged
 *     operation: without SETPRV / kernel mode it is refused outright, and
 *     no part of it is applied. This is the escalation-critical branch --
 *     if an unprivileged process could add to perm_privs, it could then
 *     "re-enable" anything on the next call, defeating the check above.
 *   - Returns the previous CURRENT privilege mask.
 */
long vms_ioctl_setprv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_priv_args args;
    int authorized;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->mode_lock);

    /* Save previous state */
    args.prev = proc->cur_privs;

    /* May this process grant itself privileges it is not authorized for? */
    authorized = (proc->current_mode == PSL_C_KERNEL) ||
                 ((proc->cur_privs & PRV_M_SETPRV) != 0);

    if (args.enable) {
        if (authorized) {
            proc->cur_privs |= args.mask;
            if (args.permanent)
                proc->perm_privs |= args.mask;
            args.status = SS__NORMAL;
        } else {
            /*
             * Unprivileged enable: only the authorized subset is granted.
             * perm_privs is NOT widened here under any circumstances --
             * doing so is what would turn this branch into an escalation.
             */
            uint64_t allowed = args.mask & proc->perm_privs;

            proc->cur_privs |= allowed;
            args.status = (allowed == args.mask) ? SS__NORMAL
                                                 : SS__NOTALLPRIV;
        }
    } else {
        /* Disabling is always allowed, current and permanent alike. */
        proc->cur_privs &= ~args.mask;
        if (args.permanent)
            proc->perm_privs &= ~args.mask;
        args.status = SS__NORMAL;
    }

    spin_unlock(&proc->mode_lock);

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
