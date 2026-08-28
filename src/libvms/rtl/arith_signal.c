/*
 * arith_signal.c - Alpha arithmetic-trap -> VMS condition bridge (GAP3).
 *
 * rd vms-db3, epic vms-8954 (OVMX-on-Alpha executive/signal backend).
 *
 * THE FAITHFUL CHAIN. On OpenVMS Alpha a trap-enabling floating-point fault
 * (e.g. a /S divide-by-zero with the IEEE DZE trap enabled) raises the
 * condition SS$_HPARITH ("high performance arithmetic trap"), which the
 * Condition Handling Facility searches handlers for; if unhandled it becomes
 * the image's $STATUS. OVMX/Linux-Alpha reproduces this: the Linux/Alpha kernel
 * delivers the hardware arithmetic trap as SIGFPE, and THIS module is the
 * image-start SIGFPE handler that turns it into a faithful SS$_HPARITH signal.
 *
 * WHY THE EXCEPTION-SUMMARY, NOT si_code (measured, vms-db3). A qemu-user probe
 * showed Linux si_code is LOSSY on Alpha (every FP fault arrives as FPE_FLTINV
 * regardless of the real cause), while the FPCR sticky exception bits captured
 * in the signal's machine context DO distinguish the cause (DZE->bit53,
 * INV->bit52, OVF->bit54, INE->bit56 -- public Alpha Architecture Handbook FPCR
 * layout). So the decode reads sc_fpcr and builds the Alpha exception-summary
 * byte, exactly the source SYS$GET_ARITH_EXCEPTION reads back. si_code is a
 * fallback only.
 *
 * THE FAITHFUL SIGNAL ARRAY (oracle FAO template, OpenVMS Alpha V8.4). The
 * HPARITH message is "high performance arithmetic trap, Imask=!XL, Fmask=!XL,
 * summary=!XB, PC=!XH, PS=!XL" -- so a faithful signal carries FIVE arguments
 * {Imask, Fmask, summary, PC, PS}, not a bare condition. We signal
 * lib$signal(SS$_HPARITH, 5, Imask, Fmask, summary, PC, PS). `summary` is the
 * faithful value from sc_fpcr; PC is faithful from the trap context; Imask,
 * Fmask and PS are best-effort under Linux (the Linux/Alpha signal frame does
 * not expose the hardware trap's register write-masks the way the VMS entArith
 * frame does) -- they are honestly marked, never fabricated with false
 * precision (INV-6).
 *
 * SCOPE. This module is the fault->condition BRIDGE. It does NOT force FP traps
 * on every image (VMS's C default is IEEE-with-traps-disabled: divide-by-zero
 * -> Inf, no signal); an image only traps if it was built trap-enabling and
 * enabled the IEEE trap. What this guarantees is: WHEN such a trap is delivered,
 * it becomes a faithful SS$_HPARITH condition. sys$get_arith_exception (below)
 * reads back the stashed exception state.
 */

#include <stdint.h>
#include <string.h>
#include <signal.h>
#include "ssdef.h"

extern uint32_t lib$signal(uint32_t condition, ...);

/* ================================================================
 * Alpha FPCR sticky exception bits (public Alpha Architecture
 * Handbook, cross-checked empirically under qemu-alpha, vms-db3).
 * ================================================================ */
#define OVMX_FPCR_INV_BIT   52   /* Invalid operation        */
#define OVMX_FPCR_DZE_BIT   53   /* Divide by zero           */
#define OVMX_FPCR_OVF_BIT   54   /* Overflow                 */
#define OVMX_FPCR_UNF_BIT   55   /* Underflow                */
#define OVMX_FPCR_INE_BIT   56   /* Inexact                  */
#define OVMX_FPCR_IOV_BIT   57   /* Integer overflow         */

/* Alpha exception-summary byte (EXC_SUM format, public Alpha ARM) -- the
 * `summary=!XB` argument of the SS$_HPARITH signal. */
#define OVMX_EXCSUM_SWC     0x01 /* Software completion       */
#define OVMX_EXCSUM_INV     0x02 /* Invalid operation         */
#define OVMX_EXCSUM_DZE     0x04 /* Divide by zero            */
#define OVMX_EXCSUM_OVF     0x08 /* Overflow                  */
#define OVMX_EXCSUM_UNF     0x10 /* Underflow                 */
#define OVMX_EXCSUM_INE     0x20 /* Inexact                   */
#define OVMX_EXCSUM_IOV     0x40 /* Integer overflow          */

/* ================================================================
 * Last arithmetic exception, stashed for SYS$GET_ARITH_EXCEPTION.
 * Thread-local: each thread's most recent HPARITH condition.
 * ================================================================ */
struct ovmx_arith_exc {
    uint32_t valid;
    uint32_t summary;   /* Alpha exception-summary byte     */
    uint32_t imask;     /* integer register write mask      */
    uint32_t fmask;     /* FP register write mask           */
    uint64_t pc;        /* faulting PC                      */
    uint32_t ps;        /* processor status                 */
};
static _Thread_local struct ovmx_arith_exc ovmx_last_arith;

/* Map the FPCR sticky exception bits to the Alpha exception-summary byte.
 * Faithful decode source (sc_fpcr), independent of the lossy Linux si_code. */
static uint32_t ovmx_fpcr_to_summary(uint64_t fpcr) {
    uint32_t s = 0;
    if (fpcr & (1ULL << OVMX_FPCR_INV_BIT)) s |= OVMX_EXCSUM_INV;
    if (fpcr & (1ULL << OVMX_FPCR_DZE_BIT)) s |= OVMX_EXCSUM_DZE;
    if (fpcr & (1ULL << OVMX_FPCR_OVF_BIT)) s |= OVMX_EXCSUM_OVF;
    if (fpcr & (1ULL << OVMX_FPCR_UNF_BIT)) s |= OVMX_EXCSUM_UNF;
    if (fpcr & (1ULL << OVMX_FPCR_INE_BIT)) s |= OVMX_EXCSUM_INE;
    if (fpcr & (1ULL << OVMX_FPCR_IOV_BIT)) s |= OVMX_EXCSUM_IOV;
    return s;
}

#if defined(__alpha__)
#include <ucontext.h>

/* SIGFPE handler: decode the Alpha FP exception summary from the trap's
 * machine context and raise a faithful SS$_HPARITH condition. */
static void ovmx_sigfpe_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)ucv;

    uint64_t fpcr = 0;
    uint64_t pc   = 0;
    uint32_t ps   = 0;
    if (uc) {
        /* On alpha-linux uc_mcontext IS struct sigcontext. sc_fpcr carries the
         * FPCR (sticky exception bits) captured at the trap; sc_pc the faulting
         * PC. Software completion (/S) is implied by a delivered trap. */
        fpcr = (uint64_t)uc->uc_mcontext.sc_fpcr;
        pc   = (uint64_t)uc->uc_mcontext.sc_pc;
        ps   = (uint32_t)uc->uc_mcontext.sc_ps;
    }

    uint32_t summary = ovmx_fpcr_to_summary(fpcr) | OVMX_EXCSUM_SWC;

    /* Fallback: if the substrate delivered the trap but left the FPCR sticky
     * bits unset (summary == just SWC), classify coarsely from the lossy Linux
     * si_code so the condition still names a cause rather than nothing. sc_fpcr
     * is the faithful source; this only fires on a lossy substrate. */
    if (si && (summary & ~OVMX_EXCSUM_SWC) == 0) {
        switch (si->si_code) {
            case FPE_FLTDIV: summary |= OVMX_EXCSUM_DZE; break;
            case FPE_FLTOVF: summary |= OVMX_EXCSUM_OVF; break;
            case FPE_FLTUND: summary |= OVMX_EXCSUM_UNF; break;
            case FPE_FLTRES: summary |= OVMX_EXCSUM_INE; break;
            case FPE_FLTINV: summary |= OVMX_EXCSUM_INV; break;
            default: break;
        }
    }

    /* Imask/Fmask: the VMS entArith frame carries the trapping instruction's
     * register write-masks; the Linux/Alpha signal frame does not expose them,
     * so they are reported best-effort as 0 here rather than fabricated. The
     * faithful, load-bearing value is `summary` (from sc_fpcr) + `pc`. */
    uint32_t imask = 0;
    uint32_t fmask = 0;

    /* Stash for SYS$GET_ARITH_EXCEPTION. */
    ovmx_last_arith.valid   = 1;
    ovmx_last_arith.summary = summary;
    ovmx_last_arith.imask   = imask;
    ovmx_last_arith.fmask   = fmask;
    ovmx_last_arith.pc      = pc;
    ovmx_last_arith.ps      = ps;

    /* Raise the faithful 5-arg SS$_HPARITH condition. If a handler CONTINUEs,
     * lib$signal returns and we resume after the (imprecise) trap -- the FP
     * result (IEEE default, e.g. +Inf) is already in the destination register.
     * If unhandled and SEVERE, lib$signal exits with SS$_HPARITH -> $STATUS. */
    lib$signal(SS$_HPARITH, (uint32_t)5, imask, fmask, summary,
               (uint32_t)(pc & 0xffffffffU), ps);
}
#endif /* __alpha__ */

/* ================================================================
 * ovmx$arith_signal_init - install the image-start SIGFPE handler.
 *
 * Idempotent, safe to call more than once. Installs the arithmetic-trap ->
 * SS$_HPARITH bridge for the current image. Does NOT itself enable FP traps
 * (that is the image's own /S-compiled + IEEE-trap-enabled decision, faithful
 * to VMS's per-image FP mode); it makes the DELIVERY faithful when a trap does
 * arrive.
 * ================================================================ */
/* Set to 1 once the SIGFPE bridge is installed. Exported so an image (and the
 * acceptance test) can assert the image-start auto-install actually ran under
 * -static -- distinguishing "the anchor/constructor fired" from "the handler
 * logic is wrong". */
int ovmx$arith_signal_installed = 0;

void ovmx$arith_signal_init(void) {
#if defined(__alpha__)
    /* sigaction is process-wide, so a plain static "install once" guard is
     * correct -- the constructor below runs it once in the main thread before
     * main, covering the whole process. */
    static int installed = 0;
    if (installed) return;
    installed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ovmx_sigfpe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, NULL);
    ovmx$arith_signal_installed = 1;
#endif
}

/* Unconditionally (re)install the bridge SIGFPE handler, bypassing the
 * install-once guard. Used by the acceptance test to restore the bridge after
 * a raw-handler substrate-fidelity probe; production code uses the auto-install
 * constructor + ovmx$arith_signal_init, not this. */
void ovmx$arith_signal_reinstall(void) {
#if defined(__alpha__)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ovmx_sigfpe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, NULL);
    ovmx$arith_signal_installed = 1;
#endif
}

/* ================================================================
 * Image-start auto-install (glibc .init_array -- OVMX's LIB$INITIALIZE
 * equivalent, see src/vmslink/link.c). Runs once before main and installs the
 * arithmetic-trap bridge for the image, faithful to VMS establishing arith-trap
 * handling at image startup.
 *
 * FORCE-PULL under -static: a constructor in an archive TU is dropped unless
 * the member is referenced. `ovmx$arith_signal_anchor` is the strong symbol the
 * per-image anchor TU (arith_signal_bind.c) references to drag this TU (and its
 * constructor) into a static image -- the established pattern from
 * src/vmslink/dcl_rms_bind.c / tests/qemu/rms_acp_bind.c.
 * ================================================================ */
__attribute__((used))
int ovmx$arith_signal_anchor = 0;

__attribute__((constructor))
static void ovmx_arith_signal_ctor(void) {
    ovmx$arith_signal_init();
}

/* Read back the most recent arithmetic exception state for the current thread.
 * Returns the stashed summary/masks/PC/PS captured by the SIGFPE handler.
 * Returns 1 if a valid exception is stashed, 0 otherwise. Internal helper that
 * sys$get_arith_exception (sys_condition.c) builds its vector from. */
int vms$$last_arith_exception(uint32_t *summary, uint32_t *imask,
                              uint32_t *fmask, uint64_t *pc, uint32_t *ps) {
    if (!ovmx_last_arith.valid) return 0;
    if (summary) *summary = ovmx_last_arith.summary;
    if (imask)   *imask   = ovmx_last_arith.imask;
    if (fmask)   *fmask   = ovmx_last_arith.fmask;
    if (pc)      *pc      = ovmx_last_arith.pc;
    if (ps)      *ps      = ovmx_last_arith.ps;
    return 1;
}
