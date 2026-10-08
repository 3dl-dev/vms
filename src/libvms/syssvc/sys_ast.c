/*
 * sys_ast.c - AST (Asynchronous System Trap) System Services
 *
 * ASTs are VMS's mechanism for asynchronous event notification. They are more
 * structured than Unix signals: each AST carries a routine address and a
 * parameter, and delivery can be enabled/disabled without losing queued ASTs.
 *
 * EVERY AST LIVES IN THE EXECUTIVE (src/kernel/vms_ast.c, reached through
 * /dev/vms by the vms_kif_* wrappers in libvmssys). This file holds no AST
 * state of its own. It is a translation layer and nothing else -- VMS
 * arguments in, /dev/vms ioctls out, executive statuses back -- and the queue,
 * the per-mode enable flag and the quota are all the executive's.
 *
 * WHAT THIS FILE USED TO BE, AND WHY THAT MATTERED (vms-as1)
 *
 * It implemented the AST facility in per-process memory: struct vms_pcb's
 * ast[mode] queues guarded by a pthread mutex, with delivery flagged by a
 * SIGUSR1 handler. Every AST entry point of the kernel-interface client --
 * vms_kif_dclast, vms_kif_setast, vms_kif_deliverast -- had ZERO callers
 * product-wide, while src/kernel/vms_ast.c implemented a real 4-level queue on
 * a per-process executive PCB that nothing ever asked for. A VMS program
 * calling $DCLAST got a per-process answer and never learned the executive was
 * not consulted -- the exact INV-6 facade the executive-retrofit epic exists
 * to kill: a system facility living in per-process memory, reporting success
 * while the executive that is supposed to own the queue is bypassed.
 *
 * Why the executive must own the queue, even though $DCLAST is self-directed:
 * on VMS an AST is queued to a process's PCB not only by $DCLAST but by the
 * executive itself -- $QIO completion, timers ($SETIMR), and the lock manager's
 * blocking AST all queue an AST into a process from kernel context. A queue
 * that lives in one image's userspace heap cannot receive any of those. The
 * queue is the executive's so the executive can put ASTs into it.
 *
 * TWO RULES FOR ANYONE EDITING THIS FILE
 *
 * 1. NO PER-PROCESS FALLBACK, EVER. Not when /dev/vms cannot be opened, not
 *    "to keep a unit test running". The executive is INTEGRAL: PID 1 refuses
 *    to bring the system up without it (src/ovmx_init/ovmx_init.c), so its
 *    absence is unreachable, and vms_kif's kif_bind() completes open->register
 *    itself before every ioctl. A caller that reaches a sys$ entry point here
 *    has an executive. If it is somehow absent the ioctl fails and the honest
 *    failure status is returned -- never a fabricated success.
 * 2. DELIVERY IS A DRAIN OF THE EXECUTIVE QUEUE, NOT A LOCAL DISPATCH. The AST
 *    routine address travels to the executive as an opaque value and comes back
 *    out through VMS_IOCTL_DELIVERAST; it is a userspace address in THIS
 *    process (ASTs declared by $DCLAST are self-directed), so it is safe to
 *    call after the executive hands it back. Do not reintroduce a userspace
 *    queue to "remember" ASTs -- the executive remembers them, across execve()
 *    and independent of this image's heap.
 */

/*
 * OVMX userspace service register (rd vms-5b4). The gate is
 * tests/integration/test_userspace_service_register.sh. Each line records
 * WHERE THE ANSWER COMES FROM.
 *
 * WHY PARTIAL AND NOT EXECUTIVE. The full OVMX-EXECUTIVE exemption is priced in
 * an A-writes/B-reads proof: one process performs the operation, ANOTHER
 * observes it, so a per-process fake that passes every single-process test is
 * ruled out. $DCLAST is SELF-DIRECTED -- an AST is declared to the CALLING
 * process, into that process's own executive PCB -- so there is no cross-process
 * A-writes/B-reads test that can name these services, and never will be. That
 * is the exact shape of sys$wflor/sys$wfland in sys_event.c, and it takes the
 * same honest PARTIAL+LOCAL pair: the exec half is true (the queue, the enable
 * flag and the quota are the executive's, one-line pass-throughs to vms_kif_*),
 * and the local half records that full residency is UNPROVEN BY THAT METHOD.
 *
 * The proof that DOES stand behind the wiring is a cross-LAYER one rather than
 * a cross-process one: tests/qemu/test_syssvc_ast.c declares an AST through the
 * PUBLIC sys$dclast and reads it back through the RAW kernel interface, which a
 * per-process fake would fail; and tests/qemu/facility_defects.sh's
 * ast-setast-disable control mutates the executive's SETAST path and reddens
 * this file's public-API assertions one layer up from test_kmod_ast.
 *
 * OVMX-PARTIAL: sys$dclast (vms-as1) -- exec: the AST queue, the per-mode enable
 *     flag and the quota are the executive's (src/kernel/vms_ast.c); this is a
 *     one-line pass-through to vms_kif_dclast.
 * OVMX-LOCAL: sys$dclast -- $DCLAST is self-directed, so no A-writes/B-reads
 *     test can name it; full residency is UNPROVEN by that method (the
 *     cross-layer proof tests/qemu/test_syssvc_ast.c stands in its place).
 * OVMX-PARTIAL: sys$setast (vms-as1) -- exec: the per-mode enable flag is the
 *     executive's; delivery drains the executive's queue through vms_kif_setast
 *     and vms_kif_deliverast, dispatching each AST the executive returns.
 * OVMX-PARTIAL: sys$clrast (vms-44a) -- exec: the AST queue it drains and whose delivery
 *     order it changes is the executive's (vms_kif_deliverast).
 * OVMX-LOCAL: sys$clrast -- the "AST in progress" marker it clears is this layer's own
 *     (the same one lib$ast_in_prog reads); self-directed, so full residency is UNPROVEN
 *     by A-writes/B-reads (tests/qemu/test_syssvc_clrast.c is the cross-layer proof).
 * OVMX-LOCAL: sys$setast -- same self-directed limit: no A-writes/B-reads test
 *     names it, so full residency is UNPROVEN by that method.
 */

#include <stdint.h>
#include <stddef.h>
#include "iosbdef.h"      /* extended struct _iosb (iosb$l_getxxi_status) -- must precede starlet.h */
#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"

/*
 * deliver_pending_asts - drain the executive's deliverable AST queue and
 * dispatch each routine in this process.
 *
 * VMS_IOCTL_DELIVERAST returns the highest-priority AST from an ENABLED queue
 * and removes it; it reports "nothing deliverable" in band (vms_kif_deliverast
 * returns -1). The executive is the sole authority on order (kernel-mode ASTs
 * before user-mode) and on which queues are enabled -- this loop only calls
 * back what the executive chooses to hand it.
 *
 * The routine address is the value $DCLAST carried into the executive, handed
 * straight back: a code address in this process. Casting it back to a function
 * pointer and calling it is the delivery.
 *
 * Non-static (vms$$ internal-helper convention, like vms$$chan_to_fd): sys$hiber
 * (sys_process.c) drains the same queue when the executive releases its $HIBER
 * because an AST became deliverable (vms-feb). Intra-image call within
 * LIBVMS$SHR -- no symbol-vector universal.
 */
/* Nonzero while an AST routine this layer dispatched is running (LIB$AST_IN_PROG). */
static __thread int ast_in_progress;

void vms$$deliver_pending_asts(void) {
    uint64_t astadr;
    uint64_t astprm;
    uint8_t  acmode;

    while (vms_kif_deliverast(&astadr, &astprm, &acmode) == 0) {
        if (astadr) {
            void (*fn)(uint32_t) = (void (*)(uint32_t))(uintptr_t)astadr;
            ast_in_progress++;
            fn((uint32_t)astprm);
            /* $CLRAST inside the routine already cleared the in-progress state. */
            if (ast_in_progress > 0)
                ast_in_progress--;
        }
    }
}

/*
 * lib$ast_in_prog - Is the caller running as an AST routine?  Returns 1 inside
 * an AST dispatched by this layer, else 0 (the VMS RTL's boolean result).
 */
uint32_t lib$ast_in_prog(void)
{
    return ast_in_progress > 0 ? 1 : 0;
}

/*
 * sys$dclast - Declare AST (Asynchronous System Trap).
 *
 * Queues the AST in the EXECUTIVE for the specified access mode. It is not
 * delivered here: the executive holds it until delivery is enabled for that
 * mode and the queue is drained (sys$setast(1)). The access-mode privilege
 * check, the quota (SS$_EXASTLM) and the null-routine rejection (SS$_BADPARAM)
 * are all the executive's -- this layer fabricates none of them.
 *
 * Parameters:
 *   astadr - AST routine to call, carried to the executive as an opaque value
 *   astprm - Parameter passed to the AST routine
 *   acmode - Access mode for the AST queue (0=kernel .. 3=user)
 *
 * Returns SS$_NORMAL on success, or the executive's status otherwise.
 */
uint32_t sys$dclast(void (*astadr)(uint32_t), uint32_t astprm,
                    uint32_t acmode) {
    return vms_kif_dclast((uint64_t)(uintptr_t)astadr, astprm,
                          (uint8_t)acmode);
}

/*
 * sys$setast - Enable or disable AST delivery for the current access mode.
 *
 * The enable flag is the executive's, per access mode. When enabling, the
 * executive's newly-deliverable ASTs are drained and dispatched immediately,
 * which is the point at which a queued self-directed AST runs.
 *
 * Returns:
 *   SS$_WASSET - AST delivery was previously enabled
 *   SS$_WASCLR - AST delivery was previously disabled (== SS$_NORMAL on VMS)
 */
uint32_t sys$setast(uint32_t enbflg) {
    uint32_t prev = vms_kif_setast(enbflg ? 1 : 0);

    if (enbflg)
        vms$$deliver_pending_asts();

    return prev;
}

/*
 * sys$clrast - Cancel the "AST in progress" state of the running AST routine.
 *
 * On VMS an AST of one access mode is not delivered while another of that mode is
 * still running; $CLRAST removes that block, so an AST queued from inside the routine
 * is delivered at once instead of after the routine returns. Here delivery is the
 * drain of the executive's queue (vms$$deliver_pending_asts): clearing the in-progress
 * marker and draining immediately gives the same ordering -- the newly queued AST runs
 * before $CLRAST returns -- and lib$ast_in_prog reports 0 for the rest of the routine.
 * Outside an AST routine it does nothing but succeed.
 */
uint32_t sys$clrast(void)
{
    if (ast_in_progress > 0) {
        ast_in_progress = 0;
        vms$$deliver_pending_asts();
    }
    return SS$_NORMAL;
}

/* sys$dclexh is implemented in sys_process.c */

/*
 * vms$$async_finish - complete an asynchronous system service that has already
 * done its work (OVMX services run synchronously).
 *
 * The caller's contract on a SUCCESSFUL queue-and-complete: the I/O status block
 * carries the final status, the event flag is set, and the AST routine is queued
 * with its parameter.  When the service itself returned an error (an argument
 * problem, a process that does not exist) VMS leaves all three untouched and
 * returns the condition -- so does this.  The same status is returned.
 *
 * efn values at or above 128 (EFN$C_ENF and friends) mean "no event flag".
 */
/*
 * vms$$async_begin - the request half of an asynchronous system service
 * ($GETJPI, $GETSYI, $GETDVI, ...): the event flag is CLEARED when the request
 * is made, and an efn that is not one of this process's flags fails the
 * request at once (SS$_ILLEFC / SS$_UNASEFC) -- real VAX V7.3 and Alpha V8.4,
 * semantic oracle docs/oracle/semantics/info/ (JPI.EFN255, JPI.EFN64.UNASSOC;
 * rd vms-4ace). With no executive there are no flags to validate.
 */
uint32_t vms$$async_begin(uint32_t efn)
{
    if (efn == 128 /* EFN$C_ENF */)
        return SS$_NORMAL;
    uint32_t c = sys$clref(efn);
    if (c == SS$_ILLEFC || c == SS$_UNASEFC)
        return c;
    return SS$_NORMAL;
}

uint32_t vms$$async_finish(uint32_t efn, void *iosb, uint32_t status,
                           void (*astadr)(uint32_t), uint32_t astprm)
{
    /* The IOSB carries the completion status whether or not the request
     * succeeded (a $GETJPI of a process that does not exist: SS$_NONEXPR in
     * both the return and the IOSB -- docs/oracle/semantics/info/, rd vms-4ace). */
    if (iosb) {
        struct _iosb *b = (struct _iosb *)iosb;
        b->iosb$w_status = (uint16_t)status;
        b->iosb$w_bcnt = 0;
        b->iosb$l_getxxi_status = status;
    }
    if (!(status & 1))
        return status;
    if (efn < 128)
        (void)sys$setef(efn);
    if (astadr) {
        uint32_t st = sys$dclast(astadr, astprm, 3 /* user mode: the access mode of the caller */);
        if (!(st & 1))
            return st;
        /* A running process at user mode with ASTs enabled takes a queued AST
         * on its way back from the service; dispatch what is deliverable now
         * (an AST the caller has disabled stays queued in the executive). */
        vms$$deliver_pending_asts();
    }
    return status;
}
