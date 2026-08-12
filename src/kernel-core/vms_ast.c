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

/*
 * AST delivery is promoted onto the kernel-backend shim (rd vms-5b2, Exec-core
 * Phase D; epic vms-8e8; design docs/design-netbsd-executive-core.md) and, like
 * event flags before it (vms_eflag.c, Phase B), now LIVES in src/kernel-core/.
 * Every host primitive this file touches -- locking (proc->mode_lock,
 * ast_state->lock), user<->kernel copy, and kernel alloc/free -- goes through
 * exec_* (exec_kbackend.h), and every intrusive-list operation goes through
 * exec_list_* (exec_list.h). On Linux each op expands to the EXACT primitive
 * this file used before (spin_lock/spin_unlock, copy_*_user, kmalloc/kfree, the
 * <linux/list.h> list_* macros), so the module is behaviour-identical -- proven
 * byte-for-byte by the disassembly-identical vms_ast.o and by the unchanged
 * Kernel Executive QEMU suite counts. The SAME source compiles against the
 * NetBSD backend without a single `#if` in this file; no NetBSD backend for AST
 * is added in this phase (Phase D is the Linux-side extraction only).
 *
 * AST is a "low-coupling" facility: it uses locking + intrusive lists + copy +
 * alloc and NOTHING ELSE -- no wait/wake (so no cv contract to reason about,
 * unlike event flags), no memory-mapping, no barriers. This file therefore
 * includes ONLY the shim contracts and the shared OVMX structs (vms_internal.h)
 * -- no `<linux/…>` header of its own.
 */

#include "vms_internal.h"
#include "exec_kbackend.h"
#include "exec_list.h"

/* Privilege bits */
/* Bits come from vms_ioctl.h's single oracle-pinned table (vms-2b8);
 * these two happened to be right, but a local copy is how the wrong
 * ones in vms_access.c survived. */
#define PRV_M_CMKRNL    VMS_PRV_M_CMKRNL
#define PRV_M_CMEXEC    VMS_PRV_M_CMEXEC

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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
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
    exec_lock(&proc->mode_lock);
    if (args.acmode < proc->current_mode) {
        /* Declaring at more privileged mode */
        if (args.acmode == PSL_C_KERNEL && !(proc->cur_privs & PRV_M_CMKRNL)) {
            exec_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
        if (args.acmode == PSL_C_EXEC && !(proc->cur_privs & (PRV_M_CMEXEC | PRV_M_CMKRNL))) {
            exec_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
        /*
         * SUPER was left behind (vms-95a): the KERNEL and EXEC checks above
         * gate a declaration at a more privileged mode, but SUPER (PSL_C_SUPER
         * = 2, MORE privileged than USER = 3 by this function's own numbering)
         * fell through with NO check, so a USER-mode caller could declare an
         * AST at SUPER without CMEXEC/CMKRNL. Combined with image rundown
         * flushing only USER-mode ASTs, such a SUPER AST survives the image and
         * runs when DCL next drains SUPER -- an escalation. This mirrors the
         * SUPER gate vms_ioctl_setmode() already carries (vms_access.c); $DCLAST
         * is the same access-mode asymmetry, closed here.
         */
        if (args.acmode == PSL_C_SUPER &&
            !(proc->cur_privs & (PRV_M_CMEXEC | PRV_M_CMKRNL))) {
            exec_unlock(&proc->mode_lock);
            args.status = SS__NOPRIV;
            goto out;
        }
    }
    exec_unlock(&proc->mode_lock);

    ast_state = &proc->ast[args.acmode];

    /* exec_alloc() may sleep (Linux GFP_KERNEL), so allocate BEFORE taking the
     * lock -- never allocate in atomic context under a held exec_lock. */
    entry = exec_alloc(sizeof(*entry));
    if (!entry) {
        args.status = SS__INSFMEM;
        goto out;
    }

    /* Check quota */
    exec_lock(&ast_state->lock);
    if (ast_state->count >= VMS_AST_MAX_PER_MODE) {
        exec_unlock(&ast_state->lock);
        exec_free(entry);
        args.status = SS__EXASTLM;
        goto out;
    }

    entry->astadr = args.astadr;
    entry->astprm = args.astprm;
    entry->acmode = args.acmode;
    exec_list_add_tail(&entry->list, &ast_state->pending);
    ast_state->count++;
    exec_unlock(&ast_state->lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->mode_lock);
    mode = proc->current_mode;
    exec_unlock(&proc->mode_lock);

    ast_state = &proc->ast[mode];

    exec_lock(&ast_state->lock);
    args.prev_state = ast_state->enabled;
    ast_state->enabled = args.enable ? 1 : 0;
    exec_unlock(&ast_state->lock);

    args.status = args.prev_state ? SS__WASSET : SS__WASCLR;

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * vms_ioctl_deliverast - Deliver next pending AST
 *
 * Called by the userspace runtime to fetch the next deliverable AST.
 * Returns the highest-priority (most privileged) AST from an ENABLED queue
 * at the caller's CURRENT access mode or OUTER (less privileged), removing
 * it from the queue.
 *
 * SECURITY: DELIVERY IS BOUNDED BELOW BY THE CALLER'S CURRENT MODE (vms-as1).
 *
 * This scan used to start at PSL_C_KERNEL unconditionally, so it delivered
 * an AST from ANY enabled queue regardless of the caller's mode. Combined
 * with vms_module.c registering every mode's queue enabled=1 by default,
 * that was an ACCESS-MODE ESCALATION: a user-mode process calling
 * sys$setast(1) drains through this ioctl (deliver_pending_asts() in
 * src/libvms/syssvc/sys_ast.c loops it to exhaustion), so a user-mode
 * $SETAST executed the AST routines queued for KERNEL/EXEC/SUPER mode --
 * privileged routines the executive holds for delivery only at those modes.
 *
 * The bound closes exactly that: an AST declared for a MORE privileged mode
 * (numerically lower) than the caller's current mode is never handed back.
 * A user-mode caller (current_mode == PSL_C_USER) drains ONLY the user-mode
 * queue; an exec-mode caller drains exec/super/user; a kernel-mode caller
 * drains all four. The inner-mode ASTs are not lost -- they stay queued
 * until the process is genuinely executing at (or inside) that mode.
 *
 * THIS IS AN OVMX CONTAINMENT CHOICE, NOT VMS-AUTHENTIC AST PREEMPTION
 * (clean-room, Rule 8). On real VMS a kernel-mode AST preempts a process
 * running in user mode by TRAPPING to kernel mode to run the routine, so
 * inner ASTs are delivered "over" outer execution. OVMX has no in-process
 * mode-switch trap: an AST routine handed back here is dispatched as an
 * ordinary userspace call at the process's real OS privilege. Delivering an
 * inner-mode routine to an outer-mode drain would therefore run a privileged
 * routine WITHOUT privilege -- so OVMX scopes delivery to the caller's mode
 * and outer instead of reproducing VMS's upward-preemption. Labelled here as
 * an OVMX design decision; not presented as VMS behaviour.
 *
 * This is the polling interface. The kernel can also proactively
 * notify via signals when ASTs are queued (future enhancement).
 */
long vms_ioctl_deliverast(struct vms_proc *proc, unsigned long arg)
{
    struct vms_ast_args args;
    struct vms_ast_entry *entry;
    int mode;
    uint8_t cur_mode;

    memset(&args, 0, sizeof(args));

    exec_lock(&proc->mode_lock);
    cur_mode = proc->current_mode;
    exec_unlock(&proc->mode_lock);

    /* Scan from the caller's current mode toward least privileged. Never
     * below cur_mode: an inner (more privileged) AST is not the caller's to
     * run at its current mode (see the header comment -- vms-as1). */
    for (mode = cur_mode; mode <= PSL_C_USER; mode++) {
        struct vms_ast_state *ast_state = &proc->ast[mode];

        exec_lock(&ast_state->lock);
        if (!ast_state->enabled || exec_list_empty(&ast_state->pending)) {
            exec_unlock(&ast_state->lock);
            continue;
        }

        /* Dequeue first entry */
        entry = exec_list_first_entry(&ast_state->pending,
                                 struct vms_ast_entry, list);
        exec_list_del(&entry->list);
        ast_state->count--;
        exec_unlock(&ast_state->lock);

        /* Return to userspace */
        args.astadr = entry->astadr;
        args.astprm = entry->astprm;
        args.acmode = entry->acmode;
        args.status = SS__NORMAL;
        exec_free(entry);

        /* VMS_IOCTL_DELIVERAST is _IOR: `arg` is a userspace pointer to a
         * struct vms_ast_args that receives the delivered AST entry. Return
         * 0 to signal "an AST was delivered"; the caller reads astadr/astprm/
         * acmode from the buffer. */
        if (exec_copyout((void *)arg, &args, sizeof(args)))
            return -EFAULT;
        return 0;
    }

    /* No ASTs to deliver */
    return -EAGAIN;
}

/*
 * vms_proc_rundown_asts - image rundown's AST flush (vms-68f.v).
 *
 * Discard the pending ASTs queued at access mode >= min_acmode (image rundown
 * passes PSL_C_USER, so the image's USER-mode ASTs), leaving inner-mode
 * (process-permanent) AST queues untouched. ASTs are the one class already
 * segregated per access mode -- proc->ast[m] is the queue for mode m (see
 * struct vms_proc) -- so image-scoped selection is exact and needs no added
 * field. This mirrors the per-mode drain vms_proc_free_claimed() does at
 * process death, restricted to the outer modes rundown owns. Grounding:
 * docs/design-image-rundown-resource-classes.md (user-mode ASTs are flushed
 * at image rundown; the enable flag is left as-is -- rundown flushes queued
 * entries, it does not re-arm delivery).
 */
void vms_proc_rundown_asts(struct vms_proc *proc, uint8_t min_acmode)
{
    struct vms_ast_entry *ast, *tmp;
    int m;

    for (m = min_acmode; m <= PSL_C_USER; m++) {
        struct vms_ast_state *st = &proc->ast[m];

        exec_lock(&st->lock);
        exec_list_for_each_entry_safe(ast, tmp, &st->pending, list) {
            exec_list_del(&ast->list);
            exec_free(ast);
        }
        st->count = 0;
        exec_unlock(&st->lock);
    }
}
