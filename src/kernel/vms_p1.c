// SPDX-License-Identifier: GPL-2.0
/*
 * vms_p1.c - P1 control-region persistence (vms-68f.ii)
 *
 * Increment (ii) of the operator-approved Option A design
 * (docs/design-in-process-activation.md Part II, "true in-process image
 * activation"), building on increment (i)'s P0 bookkeeping (vms_p0.c).
 *
 * WHAT THIS FILE IS, AND IS NOT. On OpenVMS, DCL itself -- its code and
 * data, the user stack, LNM$PROCESS, the RMS process context, the image-
 * activation context -- lives in P1, the process's CONTROL region
 * (design §A.1.1). Unlike P0 (the per-image PROGRAM region, created and
 * deleted every activation), P1 is established once and lasts for the
 * process's whole lifetime: "P0 deleted on rundown, P1 survives" is the
 * key faithful property increment (ii) exists to establish. This file
 * gives the executive a place to RECORD a process's P1 extent --
 * [p1_base, p1_limit), reflected in $GETJPI (struct vms_procinfo.p1_base/
 * p1_limit, vms_ioctl.h) -- and, by construction, to keep it separate
 * from P0. It does NOT map or unmap any memory: laying DCL's actual
 * process-permanent state into a P1 window is DCL/imgact$activate's job
 * in userspace (increment iv and later), which this increment does not
 * build.
 *
 * WHY THERE IS NO VMS_IOCTL_P1_UNMAP. VMS_IOCTL_P0_UNMAP exists because
 * image rundown is a real event with a real VMS analogue (§A.1.3): an
 * image's P0 is deleted every time control returns to DCL. P1 has no
 * rundown analogue at all -- it is created once, when the process starts,
 * and released only when the process itself goes away (vms_proc_free()
 * frees the whole struct vms_proc, p1_base/p1_limit included, same as
 * every other per-process field). Adding an unmap ioctl here would invent
 * a VMS event this increment has no design basis for and no caller that
 * needs it.
 *
 * HOW THE PERSISTENCE INVARIANT IS ACTUALLY ENFORCED, not merely
 * documented: proc->p1_base/p1_limit live under their OWN spinlock
 * (proc->p1_lock, vms_internal.h), disjoint from proc->p0_lock. Nothing
 * in vms_ioctl_p0_map()/vms_ioctl_p0_unmap() (vms_p0.c) takes p1_lock or
 * names p1_base/p1_limit, so a P0 map/unmap cycle has no code path that
 * can reach the P1 fields -- "P0 deleted on rundown, P1 survives" is a
 * fact about which lock guards which field, not a convention a future
 * edit to vms_p0.c could quietly break. tests/qemu/test_syssvc_p1.c proves
 * this against a real /dev/vms: it maps a P1 extent, cycles P0 map/unmap
 * (repeatedly), and checks $GETJPI's p1_base/p1_limit are unchanged after
 * every cycle.
 *
 * WHY A SANITY CHECK ON THE EXTENT, GIVEN THIS FILE MAPS NOTHING ITSELF.
 * Same reasoning as vms_p0.c's own header: access_ok() is the boundary
 * check the rest of this module already trusts for copy_from_user()/
 * copy_to_user(), and it is the difference between "this ioctl records a
 * fact about the caller's own address space" and "this ioctl records
 * whatever numbers a caller feels like sending". It is NOT the design's
 * access-mode/memory-protection enforcement (§A.2.3, increment iii).
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/string.h>

#include "vms_internal.h"

/*
 * vms_ioctl_p1_map - VMS_IOCTL_P1_MAP: register this process's P1
 * (control-region) extent.
 *
 * SS$_BADPARAM for a degenerate extent (a null base, or limit not strictly
 * above base) or one that fails access_ok() against the caller's own
 * address space -- see the file header for what that check does and does
 * not claim. Overwrites a previously-registered extent unconditionally,
 * same OVMX-own-accounting-ioctl reasoning vms_p0.c gives for
 * VMS_IOCTL_P0_MAP: nothing here is a VMS wire format, and a caller
 * re-registering its own P1 extent is not a condition to refuse.
 */
long vms_ioctl_p1_map(struct vms_proc *proc, unsigned long arg)
{
    struct vms_p1_args args;

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

    spin_lock(&proc->p1_lock);
    proc->p1_base = args.base;
    proc->p1_limit = args.limit;
    spin_unlock(&proc->p1_lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
