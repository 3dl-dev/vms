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

/*
 * Privilege bits and status codes come from vms_internal.h /
 * vms_ioctl.h (vms-2b8). This file used to define its own:
 *
 *   #define PRV_M_SETPRV    (1ULL << 5)
 *
 * Bit 5 is DETACH. SETPRV is bit 14 -- measured on the reference lab
 * OpenVMS VAX V7.3 node VAX1 via SDA READ SYS$SYSTEM:SYSDEF.STB
 * (docs/oracle/vax73-privileges.md §2). So the privilege gate below
 * was checking DETACH and calling it SETPRV: a process authorized to
 * create detached processes could set any privilege, and a process
 * genuinely authorized for SETPRV could not.
 *
 * The redefinitions are gone rather than corrected in place, so this
 * cannot drift from the executive's and userspace's shared definition
 * again.
 */
#define PRV_M_CMKRNL    VMS_PRV_M_CMKRNL
#define PRV_M_CMEXEC    VMS_PRV_M_CMEXEC
#define PRV_M_SETPRV    VMS_PRV_M_SETPRV

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
 * VMS semantics, ORACLE-PINNED on the reference lab OpenVMS VAX V7.3
 * node VAX1 (docs/oracle/vax73-privileges.md §3):
 *
 *   - Disabling a privilege is ALWAYS allowed.
 *   - The CURRENT (process) mask and the AUTHORIZED (permanent) mask are
 *     distinct; SHOW PROCESS/PRIVILEGES prints them separately and
 *     dropping from the current mask does not touch the authorized one.
 *   - Enabling a privilege that is ALREADY IN THE AUTHORIZED MASK needs
 *     no SETPRV. Measured: with SETPRV removed from the current mask,
 *     SET PROCESS/PRIVILEGE=SYSPRV returned %X10000001 and SYSPRV came
 *     back. SETPRV authorizes EXCEEDING your authorization, not USING
 *     it.
 *   - A request that reaches outside the authorized mask without SETPRV
 *     enables the authorized subset and reports SS$_NOTALLPRIV (1664,
 *     %SYSTEM-W-NOTALLPRIV, "not all requested privileges authorized").
 *     This tree previously returned SS$_NOPRIV here, which says the
 *     caller had no privilege at all when in fact part of the request
 *     was granted -- a false statement about what just happened.
 *
 * NOT PINNED, FLAGGED FOR OPERATOR SIGN-OFF: the status for widening
 * the PERMANENT mask without SETPRV. SET PROCESS/PRIVILEGE cannot write
 * the authorized mask (that is AUTHORIZE's job), so DCL gave no way to
 * provoke it on the oracle. OVMX refuses outright with SS$_NOPRIV and
 * applies NO partial change. The refusal itself is not a choice -- the
 * previous code widened perm_privs unconditionally, so any process
 * could permanently authorize itself for anything it could momentarily
 * enable. Only the STATUS is the unpinned part.
 */
long vms_ioctl_setprv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_priv_args args;
    bool may_exceed;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->mode_lock);

    /* Save previous state */
    args.prev = proc->cur_privs;

    may_exceed = (proc->current_mode == PSL_C_KERNEL) ||
                 (proc->cur_privs & PRV_M_SETPRV) != 0;

    if (args.enable) {
        /*
         * Widening the AUTHORIZED mask is refused BEFORE anything is
         * applied, so a rejected request leaves the process exactly as
         * it was. Checking it first also closes the escalation the old
         * ordering left open: the old code applied the authorized
         * subset to cur_privs and only then decided, so a caller could
         * observe a partial effect from a call that failed.
         */
        if (args.permanent && !may_exceed &&
            (args.mask & ~proc->perm_privs) != 0) {
            spin_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }

        if (!may_exceed) {
            /* Enable only what this process is authorized to hold. */
            uint64_t allowed = args.mask & proc->perm_privs;

            proc->cur_privs |= allowed;
            if (allowed != args.mask) {
                spin_unlock(&proc->mode_lock);
                args.status = SS__NOTALLPRIV;
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
