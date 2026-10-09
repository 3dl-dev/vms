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

/*
 * Access-mode enforcement is promoted onto the kernel-backend shim (rd vms-5b2,
 * Exec-core Phase D; epic vms-8e8; design docs/design-netbsd-executive-core.md)
 * and, like event flags (vms_eflag.c, Phase B) and ASTs (vms_ast.c, this phase),
 * now LIVES in src/kernel-core/. The ONLY host primitives this file touches are
 * locking (proc->mode_lock) and user<->kernel copy; both go through exec_*
 * (exec_kbackend.h). On Linux each op expands to the EXACT primitive this file
 * used before (spin_lock/spin_unlock, copy_*_user), so the module is
 * behaviour-identical -- proven byte-for-byte by the disassembly-identical
 * vms_access.o and by the unchanged Kernel Executive QEMU suite counts. The SAME
 * source compiles against the NetBSD backend without a single `#if`; no NetBSD
 * backend for access modes is added in this phase (Phase D is the Linux-side
 * extraction only).
 *
 * This is the LEAST-coupled of the extracted facilities: no intrusive lists, no
 * alloc, no wait/wake -- just mode_lock and copy. It calls the other facilities'
 * image-rundown release helpers (vms_proc_rundown_locks/channels/asts, declared
 * in vms_internal.h), which is a plain cross-facility function call, not a host
 * primitive. This file therefore includes ONLY the shim contract and the shared
 * OVMX structs (vms_internal.h) -- no `<linux/…>` header of its own.
 */

#include "vms_internal.h"
#include "exec_kbackend.h"
#include "vms_prot.h"     /* vms_prot_require_priv: the known-file list is CMKRNL's */

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
 *   - To super mode: PRV$M_CMEXEC (or CMKRNL)
 *   - To user mode: always allowed (least privileged)
 *
 * Actually in VMS, you can only go to more privileged modes with
 * change-mode instructions. Going to less privileged is via REI.
 * We enforce: you can go to equal or more privileged if you have
 * the right privilege. Going less privileged is always OK.
 *
 * SUPER NOW REQUIRES PRIVILEGE TOO (vms-68f.iii, in-process image
 * activation increment (iii) -- docs/design-in-process-activation.md
 * Part II §A.2.3(a)). This used to say "to super mode: from user only, no
 * special priv (less privileged)" -- backwards: SUPER (2) is a LOWER,
 * i.e. MORE privileged, number than USER (3), so raising into it is an
 * escalation by this function's own stated rule, and the design is
 * explicit that raising User->Super/Exec/Kernel all require the
 * change-mode privilege VMS names (CMEXEC/CMKRNL). Leaving Super
 * unguarded meant ANY unprivileged caller already in User mode could
 * request Super and be granted it outright -- the exact "cannot raise its
 * own mode except via the controlled transition" property this increment
 * exists to enforce, previously true for Kernel/Exec only. The paired,
 * privilege-free route into User (VMS_IOCTL_ENTER_IMAGE, vms_access.c
 * below) and back (VMS_IOCTL_IMAGE_RUNDOWN) is EXECUTIVE-VERIFIED instead
 * of privilege-gated -- it can only return to the exact mode a matching
 * ENTER_IMAGE recorded, never to an arbitrary target -- which is why it
 * does not need this same guard.
 */
long vms_ioctl_setmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mode_args args;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (args.mode > PSL_C_USER) {
        args.status = SS__BADPARAM;
        goto out;
    }

    exec_lock(&proc->mode_lock);

    /* Check if we're going to a more privileged mode */
    if (args.mode < proc->current_mode) {
        /* More privileged = lower number */
        if (args.mode == PSL_C_KERNEL) {
            if (!(proc->cur_privs & PRV_M_CMKRNL)) {
                exec_unlock(&proc->mode_lock);
                args.status = SS__NOPRIV;
                goto out;
            }
        } else if (args.mode == PSL_C_EXEC || args.mode == PSL_C_SUPER) {
            if (!(proc->cur_privs & (PRV_M_CMEXEC | PRV_M_CMKRNL))) {
                exec_unlock(&proc->mode_lock);
                args.status = SS__NOPRIV;
                goto out;
            }
        }
    }

    proc->current_mode = args.mode;
    exec_unlock(&proc->mode_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_enter_image - VMS_IOCTL_ENTER_IMAGE: the controlled descent
 * DCL (Supervisor) uses to enter an activated image (User) (vms-68f.iii,
 * docs/design-in-process-activation.md Part II §A.1.3, §A.2.3).
 *
 * Refused SS$_NOPRIV unless the caller's CURRENT mode is exactly
 * PSL_C_SUPER -- this increment's ceiling only needs DCL's own descent
 * (the design's command loop, §A.1.2), and refusing every other starting
 * mode is what makes the paired return (VMS_IOCTL_IMAGE_RUNDOWN) able to
 * restore a SINGLE recorded mode rather than a caller-chosen one. No
 * privilege is required to descend -- lowering mode is always allowed,
 * the same rule VMS_IOCTL_SETMODE already applies to a drop.
 *
 * On success: pre_image_mode records PSL_C_SUPER (what to restore),
 * current_mode becomes PSL_C_USER, image_active becomes 1. A second
 * ENTER_IMAGE while image_active is already 1 is refused the same way
 * (current_mode is PSL_C_USER by then, not PSL_C_SUPER) -- there is no
 * nested-descent case in this increment's design.
 */
long vms_ioctl_enter_image(struct vms_proc *proc, unsigned long arg)
{
    struct vms_modexfer_args args;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->mode_lock);

    if (proc->current_mode != PSL_C_SUPER) {
        args.prev_mode = proc->current_mode;
        args.new_mode = proc->current_mode;
        exec_unlock(&proc->mode_lock);
        args.status = SS__NOPRIV;
        goto out;
    }

    args.prev_mode = proc->current_mode;
    proc->pre_image_mode = proc->current_mode;
    proc->current_mode = PSL_C_USER;
    proc->image_active = 1;
    args.new_mode = proc->current_mode;

    exec_unlock(&proc->mode_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_image_rundown - VMS_IOCTL_IMAGE_RUNDOWN: the controlled,
 * PAIRED return DCL uses when an activated image exits (vms-68f.iii,
 * docs/design-in-process-activation.md Part II §A.1.3, §A.2.3).
 *
 * Refused SS$_NOPRIV unless image_active is currently 1 -- i.e. unless
 * THIS process is mid a descent that VMS_IOCTL_ENTER_IMAGE actually
 * performed. THIS is the enforcement the design's negative control names:
 * a caller cannot manufacture a return to Supervisor by calling RUNDOWN
 * cold (no privilege, no prior ENTER_IMAGE, current_mode already
 * PSL_C_USER by default at registration -- see vms_module.c), and cannot
 * replay a second RUNDOWN after a first one already cleared the flag.
 *
 * On success: current_mode is restored to whatever ENTER_IMAGE recorded
 * in pre_image_mode (PSL_C_SUPER on every path this design uses), and
 * image_active is cleared. Unlike VMS_IOCTL_SETMODE's raise path, this
 * requires NO privilege check of its own -- the executive already proved
 * the caller legitimately descended, and the target is not caller-chosen,
 * it is the executive's own recorded fact.
 *
 * IMAGE-SCOPED RESOURCE RELEASE (vms-68f.v, docs/design-in-process-
 * activation.md Part II §A.2.1 step 2, §A.6.1 -- "rundown completeness is
 * the hard part"). On OpenVMS, image rundown does not just switch mode: it
 * releases the resources the image owned -- the channels it $ASSIGNed, the
 * locks it $ENQed, the ASTs it declared -- all at USER access mode, while
 * process-permanent (inner-mode) state survives so DCL resumes intact. This
 * handler now does that: the mode being run down (proc->current_mode, USER on
 * every path ENTER_IMAGE built) is captured, and after the mode is restored
 * the executive dequeues exactly this process's resources owned at that mode
 * or outer. The P1 control-region extent is under its own lock and is never
 * named here, so "P0/image state dies at rundown, P1 survives" holds at the
 * resource level too. Which classes are image-scoped vs process-permanent is
 * grounded per class in docs/design-image-rundown-resource-classes.md, not
 * guessed (Rule 8). Release runs OUTSIDE mode_lock (the resource lists have
 * their own locks; one PCB, one image at a time, so no concurrent activation
 * races it) -- the same discipline vms_proc_free_claimed() uses.
 */
long vms_ioctl_image_rundown(struct vms_proc *proc, unsigned long arg)
{
    struct vms_modexfer_args args;
    uint8_t rundown_mode;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->mode_lock);

    if (!proc->image_active) {
        args.prev_mode = proc->current_mode;
        args.new_mode = proc->current_mode;
        exec_unlock(&proc->mode_lock);
        args.status = SS__NOPRIV;
        goto out;
    }

    args.prev_mode = proc->current_mode;
    rundown_mode = proc->current_mode;   /* the image's mode (PSL_C_USER) */
    proc->current_mode = proc->pre_image_mode;
    proc->image_active = 0;
    args.new_mode = proc->current_mode;

    exec_unlock(&proc->mode_lock);

    /* Release the image's resources; process-permanent state is left alone. */
    vms_proc_rundown_locks(proc, rundown_mode);
    vms_proc_rundown_channels(proc, rundown_mode);
    vms_proc_rundown_asts(proc, rundown_mode);
    /* VMS deletes the process's user-mode logical names at image rundown
     * (rd vms-ef21); supervisor and inner-mode names outlive the image. */
    vms_lnm_rundown(proc->vms_pid, rundown_mode);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
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
    exec_lock(&proc->mode_lock);
    args.mode = proc->current_mode;
    args.cur_privs = proc->cur_privs;
    args.perm_privs = proc->perm_privs;
    exec_unlock(&proc->mode_lock);

    /* Zero padding */
    args.pad[0] = args.pad[1] = args.pad[2] = 0;

    if (exec_copyout((void *)arg, &args, sizeof(args)))
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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->mode_lock);

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
            exec_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }

        if (!may_exceed) {
            /* Enable only what this process is authorized to hold. */
            uint64_t allowed = args.mask & proc->perm_privs;

            proc->cur_privs |= allowed;
            if (allowed != args.mask) {
                exec_unlock(&proc->mode_lock);
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

    exec_unlock(&proc->mode_lock);
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->mode_lock);
    args.prev = proc->cur_privs;

    if ((proc->cur_privs & args.mask) == args.mask)
        args.status = SS__NORMAL;
    else
        args.status = SS__NOPRIV;

    exec_unlock(&proc->mode_lock);

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/* ================================================================
 * Known File Entries -- the executive's list of INSTALLed images
 * (VMS_IOCTL_KFE, rd vms-7c64 / vms-220).
 *
 * On VMS the known file list lives in the executive and INSTALL changes it
 * under CMKRNL (INSTALL ADD/REPLACE/REMOVE fail %SYSTEM-F-NOCMKRNL without it,
 * observed on the lab Alpha V8.4: tests/lab/captures/install-priv-20261009/).
 * Here too: the list is executive memory, changed only by a CMKRNL caller
 * (vms_prot_require_priv on the caller's own PCB), and an entry names its file
 * by the identity the SUBSTRATE reports for the descriptor the caller holds
 * (exec_file_identity), so the activation-time grant can recognise that very
 * file. LIST is open to every process, as INSTALL LIST is.
 * ================================================================ */
struct vms_kfe {
    int      used;
    uint64_t dev, ino;
    uint64_t privs;
    uint32_t flags;
    uint32_t access;
    char     name[VMS_KFE_NAMELEN];
};
static struct vms_kfe vms_kfe_tab[VMS_KFE_MAX];
static EXEC_DEFINE_MUTEX(vms_kfe_mutex);

static int kfe_find(uint64_t dev, uint64_t ino)
{
    for (int i = 0; i < VMS_KFE_MAX; i++)
        if (vms_kfe_tab[i].used && vms_kfe_tab[i].dev == dev && vms_kfe_tab[i].ino == ino)
            return i;
    return -1;
}

/* For the activation-time grant (vms-bdc1): the privileges an installed image
 * file carries, 0 when it is not installed /PRIVILEGED. */
uint64_t vms_kfe_image_privs(uint64_t dev, uint64_t ino)
{
    uint64_t p = 0;
    exec_mutex_lock(&vms_kfe_mutex);
    int i = kfe_find(dev, ino);
    if (i >= 0 && (vms_kfe_tab[i].flags & VMS_KFE_F_PRIV)) {
        p = vms_kfe_tab[i].privs;
        vms_kfe_tab[i].access++;
    }
    exec_mutex_unlock(&vms_kfe_mutex);
    return p;
}

long vms_ioctl_kfe(struct vms_proc *proc, unsigned long arg)
{
    struct vms_kfe_args args;
    uint64_t dev = 0, ino = 0, privs;
    uint32_t st = SS__NORMAL;
    int i;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.name[VMS_KFE_NAMELEN - 1] = '\0';

    if (args.op == VMS_KFE_OP_ADD || args.op == VMS_KFE_OP_REPLACE ||
        args.op == VMS_KFE_OP_REMOVE) {
        exec_lock(&proc->mode_lock);
        privs = proc->cur_privs;
        exec_unlock(&proc->mode_lock);
        st = vms_prot_require_priv(privs, VMS_PRV_M_CMKRNL);
        if (!(st & 1))
            goto out;
    }
    if (args.op != VMS_KFE_OP_LIST && exec_file_identity(args.fd, &dev, &ino) != 0) {
        st = SS__BADPARAM;
        goto out;
    }

    exec_mutex_lock(&vms_kfe_mutex);
    switch (args.op) {
    case VMS_KFE_OP_ADD:
        if (kfe_find(dev, ino) >= 0) {
            st = SS__DUPLNAM;
            break;
        }
        for (i = 0; i < VMS_KFE_MAX && vms_kfe_tab[i].used; i++)
            ;
        if (i == VMS_KFE_MAX) {
            st = SS__INSFMEM;
            break;
        }
        memset(&vms_kfe_tab[i], 0, sizeof(vms_kfe_tab[i]));
        vms_kfe_tab[i].used = 1;
        vms_kfe_tab[i].dev = dev;
        vms_kfe_tab[i].ino = ino;
        vms_kfe_tab[i].privs = (args.flags & VMS_KFE_F_PRIV) ? args.privs : 0;
        vms_kfe_tab[i].flags = args.flags;
        memcpy(vms_kfe_tab[i].name, args.name, VMS_KFE_NAMELEN);
        break;
    case VMS_KFE_OP_REPLACE:
        i = kfe_find(dev, ino);
        if (i < 0) {
            st = SS__NOSUCHFILE;
            break;
        }
        vms_kfe_tab[i].privs = (args.flags & VMS_KFE_F_PRIV) ? args.privs : 0;
        vms_kfe_tab[i].flags = args.flags;
        memcpy(vms_kfe_tab[i].name, args.name, VMS_KFE_NAMELEN);
        break;
    case VMS_KFE_OP_REMOVE:
        i = kfe_find(dev, ino);
        if (i < 0)
            st = SS__NOSUCHFILE;
        else
            vms_kfe_tab[i].used = 0;
        break;
    case VMS_KFE_OP_FIND:
    case VMS_KFE_OP_LIST:
        if (args.op == VMS_KFE_OP_FIND) {
            i = kfe_find(dev, ino);
        } else {
            for (i = (int)args.index; i < VMS_KFE_MAX && !vms_kfe_tab[i].used; i++)
                ;
            if (i >= VMS_KFE_MAX)
                i = -1;
        }
        if (i < 0) {
            st = SS__NOSUCHFILE;
            break;
        }
        args.index = (uint32_t)i + 1;
        args.privs = vms_kfe_tab[i].privs;
        args.flags = vms_kfe_tab[i].flags;
        args.access = vms_kfe_tab[i].access;
        memcpy(args.name, vms_kfe_tab[i].name, VMS_KFE_NAMELEN);
        break;
    default:
        st = SS__BADPARAM;
        break;
    }
    exec_mutex_unlock(&vms_kfe_mutex);

out:
    args.status = st;
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
