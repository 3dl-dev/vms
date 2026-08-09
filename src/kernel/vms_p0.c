// SPDX-License-Identifier: GPL-2.0
/*
 * vms_p0.c - P0 program-region bookkeeping (vms-68f.i)
 *
 * Increment (i) of the operator-approved Option A design
 * (docs/design-in-process-activation.md Part II, "true in-process image
 * activation"): the executive foundation that later increments build on to
 * run an activated image in DCL's own process instead of forking one.
 *
 * WHAT THIS FILE IS, AND IS NOT. On OpenVMS, RUN maps an image into the
 * process's P0 (program) region and image rundown deletes that region,
 * leaving P1 (DCL's own process-permanent state) untouched -- one VMS
 * process throughout (design §A.1.1, §A.1.3). This file gives the executive
 * a place to RECORD that fact -- [p0_base, p0_limit) per process, reflected
 * in $GETJPI (struct vms_procinfo.p0_base/p0_limit, vms_ioctl.h) -- so it is
 * observable by the process itself and by any other process authorized to
 * read its row. It does NOT map or unmap any memory: the P0 window
 * reservation and the per-image PT_LOAD mmaps into it are DCL's and
 * imgact$activate's job in userspace (design §A.2.1 steps 1-4), which this
 * increment does not build. VMS_IOCTL_P0_MAP is that step 4's executive
 * half ("register the P0 extent ... so it knows this process has an image
 * mapped"); VMS_IOCTL_P0_UNMAP is rundown step 4's ("executive marks the
 * process image-less"). Activating an image into P0 (increment iv),
 * releasing image-scoped executive state at rundown (increment v) and the
 * access-mode transitions around both (increment iii) are separate items --
 * see vms-6ba/vms-6f1 and vms-68f's decomposition.
 *
 * WHY A SANITY CHECK ON THE EXTENT, GIVEN THIS FILE MAPS NOTHING ITSELF.
 * access_ok() is the same userspace-address boundary check the rest of this
 * module already trusts for every copy_from_user()/copy_to_user() -- it
 * costs nothing extra to ask it here too, and it is the difference between
 * "this ioctl records a fact about the caller's own address space" and
 * "this ioctl records whatever numbers a caller feels like sending", which
 * would make $GETJPI's new fields decorative rather than accounting. It is
 * NOT the design's access-mode/memory-protection enforcement (§A.2.3,
 * increment iii) -- no page table is walked, no mapping is verified to
 * exist, and nothing here stops a process recording an extent inside its
 * own address space that has nothing mapped in it. That is out of scope for
 * a bookkeeping ioctl over state userspace already created; the design
 * labels the ceiling explicitly (service-boundary + critical-P1 pages) and
 * this increment does not reach it.
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "vms_internal.h"

/*
 * vms_ioctl_p0_map - VMS_IOCTL_P0_MAP: register this process's current P0
 * extent.
 *
 * SS$_BADPARAM for a degenerate extent (a null base, or limit not strictly
 * above base) or one that fails access_ok() against the caller's own
 * address space -- see the file header for what that check does and does
 * not claim. Overwrites a previously-registered extent unconditionally:
 * this is OVMX's own accounting ioctl (not a VMS wire format, vms_ioctl.h),
 * and a second RUN replacing the first RUN's now-rundown extent is exactly
 * the sequence increment (iv)/(v) will drive through this same call.
 */
long vms_ioctl_p0_map(struct vms_proc *proc, unsigned long arg)
{
    struct vms_p0_args args;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.base == 0 || args.limit <= args.base) {
        args.status = SS__BADPARAM;
        goto out;
    }

    if (!access_ok((void __user *)(unsigned long)args.base,
                    args.limit - args.base)) {
        args.status = SS__BADPARAM;
        goto out;
    }

    spin_lock(&proc->p0_lock);
    proc->p0_base = args.base;
    proc->p0_limit = args.limit;
    spin_unlock(&proc->p0_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_p0_unmap - VMS_IOCTL_P0_UNMAP: clear this process's P0 extent.
 *
 * Unconditional success: clearing an already-clear extent (no image ever
 * mapped, or a second UNMAP after the first) is a no-op, not an error --
 * there is no VMS condition being modeled here to refuse it against (this
 * ioctl has no VMS wire-format counterpart, vms_ioctl.h). base/limit come
 * back holding whatever WAS registered (zero if nothing was), so a caller
 * -- or this suite's QEMU proof -- can observe what rundown just freed
 * without a separate GETJPI round trip.
 */
long vms_ioctl_p0_unmap(struct vms_proc *proc, unsigned long arg)
{
    struct vms_p0_args args;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->p0_lock);
    args.base = proc->p0_base;
    args.limit = proc->p0_limit;
    proc->p0_base = 0;
    proc->p0_limit = 0;
    spin_unlock(&proc->p0_lock);

    args.status = SS__NORMAL;

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
