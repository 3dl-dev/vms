/*
 * sys_ast.c - AST (Asynchronous System Trap) System Services
 *
 * Routes exclusively through the kernel executive (the vms.ko module's
 * 4-level AST queues, reached via /dev/vms ioctl through the
 * vms_kif_dclast/setast/deliverast wrappers in libvmssys). There is no
 * userspace AST queue and no SIGUSR1-based simulation -- the kernel
 * module is the single authoritative holder of pending ASTs, matching
 * the sys_lock.c wiring pattern ($ENQ/$DEQ, docs/design-executive-retrofit.md).
 * Docker containers have no /dev/vms, so $DCLAST/$SETAST return
 * SS$_NOSUCHDEV there -- that is accepted, by design, not a bug.
 *
 * Previously (pre executive-retrofit vms-as1) this file simulated AST
 * delivery per-process using a PCB queue + SIGUSR1 signal handler. That
 * was exactly the per-process fake this epic exists to kill: an AST
 * declared here was only ever visible to this process, and a blocking/
 * completion AST queued by the kernel lock manager for a *different*
 * process (src/kernel/vms_lock.c's queue_completion_ast/notify_blocking_asts)
 * could never reach this delivery path at all. Routing through the same
 * kernel AST queues that the lock manager already feeds is what makes
 * cross-process AST delivery real instead of coincidental.
 *
 * ASTs are queued in kernel memory per access mode (vms.ko, VMS_IOCTL_DCLAST).
 * Delivery is realized by draining the kernel queue via VMS_IOCTL_DELIVERAST
 * and invoking the returned function pointer in this process's own address
 * space -- the astadr is always valid in the address space of whichever
 * process registered it (self for $DCLAST, the lock holder for a blocking/
 * completion AST), so no cross-process pointer hazard exists.
 */

#include <stdint.h>
#include <stddef.h>
#include "starlet.h"
#include "vms_kif.h"

/*
 * Lazily open /dev/vms for this thread. vms_kif_open() is idempotent, so
 * it is safe to call on every $DCLAST/$SETAST. Returns 0 if the kernel
 * device is available, -1 otherwise (e.g. Docker mode, which has no
 * /dev/vms -- see CLAUDE.md Rule 9 / docs/design-executive-retrofit.md).
 */
static int ensure_kif_open(void)
{
    return vms_kif_open() >= 0 ? 0 : -1;
}

/*
 * ast_kstat_to_ss - Translate a kernel AST-facility status code into its
 * public ssdef.h SS$_xxx constant, at the boundary where a raw kernel
 * status crosses into the public sys$dclast/sys$setast contract.
 *
 * The kernel module (src/kernel/vms_internal.h, SS__xxx) and the public
 * headers (src/libvms/include/ssdef.h) do not agree on the numeric value
 * for every code -- the same cross-scheme mismatch sys_lock.c's
 * kstat_to_ss() already documents and translates for the lock manager.
 * The magic numbers on the left are the raw kernel SS__xxx values from
 * src/kernel/vms_internal.h (kept as literals rather than an #include,
 * since that header pulls in kernel-only headers and cannot be built
 * into glibc userspace code):
 *
 *   SS__NORMAL   0x01 (1)   matches SS$_NORMAL   (1)   -- passthrough
 *   SS__BADPARAM 0x14 (20)  matches SS$_BADPARAM (20)  -- passthrough
 *   SS__NOPRIV   0x24 (36)  matches SS$_NOPRIV   (36)  -- passthrough
 *   SS__WASSET   9          matches SS$_WASSET   (9)   -- passthrough
 *   SS__WASCLR   5          !=     SS$_WASCLR    (1)   -- translated
 *   SS__INSFMEM  0x2C (44)  !=     SS$_INSFMEM   (292) -- translated
 *   SS__EXASTLM  0x38 (56)  !=     SS$_EXASTLM   (2756)-- translated
 *
 * NOTE (flag for operator sign-off, not self-certified): SS$_WASCLR is
 * defined in ssdef.h as 1, identical to SS$_NORMAL, with an in-file
 * comment marking it an "alternate" value -- this looks like a repo
 * placeholder rather than a pinned real-VMS value (see the open
 * placeholder-constants item, vms-c90). This function preserves
 * whatever ssdef.h already promises callers today (no behavior change
 * for existing/future callers of the public sys$setast contract); it
 * does not assert that 1 is the VMS-authentic SS$_WASCLR value.
 */
static uint32_t ast_kstat_to_ss(uint32_t k)
{
    switch (k) {
    case 5:  return SS$_WASCLR;   /* kernel SS__WASCLR */
    case 44: return SS$_INSFMEM;  /* kernel SS__INSFMEM */
    case 56: return SS$_EXASTLM;  /* kernel SS__EXASTLM */
    default: return k;
    }
}

/*
 * sys$dclast - Declare AST (Asynchronous System Trap).
 *
 * Queues the AST in the kernel's per-access-mode queue for the calling
 * process (VMS_IOCTL_DCLAST). If AST delivery is enabled for that mode,
 * the kernel already holds it ready for the next sys$setast(1)/DELIVERAST
 * drain; nothing else fires it early.
 *
 * Parameters:
 *   astadr - AST routine to call
 *   astprm - Parameter passed to the AST routine
 *   acmode - Access mode for the AST queue
 */
uint32_t sys$dclast(void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t acmode) {
    if (!astadr) return SS$_BADPARAM;

    if (ensure_kif_open() < 0)
        return SS$_NOSUCHDEV;

    uint8_t mode = (uint8_t)(acmode > PSL_C_USER ? PSL_C_USER : acmode);

    uint32_t kstatus = vms_kif_dclast((uint64_t)(uintptr_t)astadr,
                                       (uint64_t)astprm, mode);
    return ast_kstat_to_ss(kstatus);
}

/*
 * deliver_pending_asts - Drain the kernel's AST queues for this process,
 * invoking each returned handler in order (kernel/exec/super/user
 * priority, matching vms_ioctl_deliverast's scan order in vms.ko).
 *
 * Called when sys$setast(1) re-enables delivery, so ASTs queued while
 * delivery was disabled (by this process's own $DCLAST, or by the
 * kernel lock manager on behalf of another process's ENQ/CONVERT) fire
 * immediately -- the same "enable delivers what's pending" contract the
 * old PCB-based implementation offered, now backed by the kernel queue.
 */
static void deliver_pending_asts(void)
{
    uint64_t astadr;
    uint64_t astprm;
    uint8_t  acmode;

    while (vms_kif_deliverast(&astadr, &astprm, &acmode) == 0) {
        (void)acmode;
        void (*handler)(uint32_t) = (void (*)(uint32_t))(uintptr_t)astadr;
        if (handler)
            handler((uint32_t)astprm);
    }
}

/*
 * sys$setast - Enable or disable AST delivery.
 *
 * Operates on the kernel's queue for the calling process's current
 * access mode (VMS_IOCTL_SETAST). When enabling, any queued ASTs
 * (including ones queued by the kernel lock manager as a completion or
 * blocking AST on behalf of another process) are drained and delivered
 * immediately.
 *
 * Returns:
 *   SS$_WASSET     - AST delivery was previously enabled
 *   SS$_WASCLR     - AST delivery was previously disabled
 *   SS$_NOSUCHDEV  - /dev/vms is not available (no executive; Rule 9)
 */
uint32_t sys$setast(uint32_t enbflg) {
    if (ensure_kif_open() < 0)
        return SS$_NOSUCHDEV;

    uint32_t kstatus = vms_kif_setast(enbflg ? 1 : 0);
    uint32_t pub_status = ast_kstat_to_ss(kstatus);

    if (enbflg)
        deliver_pending_asts();

    return pub_status;
}
