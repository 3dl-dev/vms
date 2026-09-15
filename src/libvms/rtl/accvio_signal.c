/*
 * accvio_signal.c - Alpha access-violation (hardware fault) -> VMS SS$_ACCVIO
 * condition bridge (CHF rung-4, gap G6).
 *
 * rd vms-cc8, epic vms-2e72 (docs/design-chf-condition-handling.md rung-4).
 *
 * THE G6 GAP. Rung-1..3 route SOFTWARE conditions (lib$signal) through the
 * authentic CHF search (dispatch_condition), and the arithmetic-trap bridge
 * (arith_signal.c, vms-db3) proved the HARDWARE->condition pattern for ONE code
 * (SS$_HPARITH via SIGFPE). Gap G6 is: ONE dispatcher must serve hardware
 * exceptions too. This module extends the arith bridge's model to the access
 * violation. On OpenVMS a reference to an inaccessible address raises the
 * condition SS$_ACCVIO, which the CHF searches handlers for exactly like any
 * other condition; if unhandled it becomes the image's $STATUS. OVMX/Linux-Alpha
 * reproduces this: the Linux/Alpha kernel delivers the access fault as SIGSEGV
 * (SIGBUS/SIGILL for the sibling faults), and THIS module is the image-start
 * handler that turns it into a faithful SS$_ACCVIO condition dispatched through
 * the SAME dispatch_condition() (reached via the public lib$signal), so a
 * SYS$SETEXV primary vector and the established frame-handler chain are searched
 * in the authentic order on a REAL trap.
 *
 * THE FAITHFUL SIGNAL ARRAY (public OpenVMS access-violation format). SS$_ACCVIO
 * is signalled with the argument pair {reason, virtual-address} followed by the
 * mandatory trailing {PC, PSL} -- lib$signal(SS$_ACCVIO, 4, reason, va, pc, psl).
 *   - reason  : the access-violation reason longword (public VMS layout below):
 *               bit<0> length violation (address in no mapped region) vs
 *               access-mode/protection violation; bit<2> the reference was a
 *               write/modify. Decoded from the Linux si_code (SEGV_MAPERR vs
 *               SEGV_ACCERR) -- faithful for the length/protection distinction.
 *   - va      : the inaccessible virtual address (si_addr) -- faithful.
 *   - pc      : the faulting PC from the trap machine context -- faithful.
 *   - psl     : processor status -- best-effort under Linux (the Linux/Alpha
 *               signal frame does not expose the VMS PSL), honestly marked, never
 *               fabricated with false precision (INV-6).
 *
 * WIDTH (honest gap, tracked separately). The OVMX chf$signal_array is a
 * LONGWORD array (VAX-width model, src/libvms/include/chfdef.h); on OpenVMS Alpha
 * the signal array is quadwords. The array here therefore carries the low 32
 * bits of a 64-bit va/pc (identical fidelity compromise to arith_signal.c's PC),
 * while the FULL 64-bit {reason, va, pc, psl} is stashed thread-local and read
 * back byte-exact via vms$$last_accvio_exception(). Widening chf$signal_array to
 * quadwords for the Alpha HW-condition path is an ABI change filed as its own
 * rung-4 fidelity child; it is NOT silently skipped.
 *
 * SCOPE. This module is the fault->condition BRIDGE for the access violation,
 * mirroring arith_signal.c. It does NOT change SYS$IMGACT's own SIGSEGV
 * rundown path (sys_imgact.c); wiring the bridge into the production activator's
 * fault handling is a separate integration (its own Alpha-rail regression proof).
 * An access violation is NOT resumable (returning from the handler would
 * re-execute the faulting instruction and re-fault), so a handler for SS$_ACCVIO
 * must transfer control via SYS$UNWIND (rung-2) rather than CONTINUE -- hence
 * SA_NODEFER (the handler longjmps out via the CHF unwind anchor; the kernel's
 * signal-return mask restore is bypassed, so SIGSEGV must not be left blocked).
 */

#include <stdint.h>
#include <string.h>
#include <signal.h>
#include "ssdef.h"

extern uint32_t lib$signal(uint32_t condition, ...);

/* ================================================================
 * VMS access-violation reason longword (public OpenVMS ACCVIO signal format,
 * OpenVMS System Services / Programming Concepts). Only the bits OVMX can
 * faithfully derive from the Linux trap are set; the rest are honestly 0.
 * ================================================================ */
#define OVMX_ACCVIO_LENGTH_VIOLATION 0x1  /* bit<0>: 1=length (unmapped), 0=protection */
#define OVMX_ACCVIO_WRITE            0x4  /* bit<2>: 1=write/modify reference, 0=read   */

/* ================================================================
 * Last access violation, stashed for a faithful 64-bit read-back (the longword
 * signal array cannot carry a >32-bit va/pc; see WIDTH above). Thread-local:
 * each thread's most recent SS$_ACCVIO condition.
 * ================================================================ */
struct ovmx_accvio_exc {
    uint32_t valid;
    uint32_t reason;    /* VMS access-violation reason longword */
    uint64_t va;        /* inaccessible virtual address (full 64-bit) */
    uint64_t pc;        /* faulting PC (full 64-bit) */
    uint32_t psl;       /* processor status (best-effort) */
};
static _Thread_local struct ovmx_accvio_exc ovmx_last_accvio;

/* Set to 1 once the SIGSEGV/SIGBUS/SIGILL bridge is installed. Exported so an
 * image (and the acceptance test) can assert the image-start auto-install
 * actually ran under -static -- distinguishing "the anchor/constructor fired"
 * from "the handler logic is wrong". */
int ovmx$accvio_signal_installed = 0;

#if defined(__alpha__)
#include <ucontext.h>

/* Re-entrancy guard: with SA_NODEFER a fault taken INSIDE the bridge (e.g. a
 * genuine executive bug while building the condition) would recurse forever.
 * Bound it: on re-entry, restore the default disposition and re-raise so the
 * process dies with a real signal rather than spinning. */
static _Thread_local int ovmx_in_accvio_handler = 0;

/* SIGSEGV/SIGBUS/SIGILL handler: decode the faulting address + reason from the
 * trap's siginfo/machine context and raise a faithful SS$_ACCVIO condition
 * through the same dispatch_condition() the software path uses (via lib$signal).
 */
static void ovmx_sigsegv_handler(int sig, siginfo_t *si, void *ucv) {
    if (ovmx_in_accvio_handler) {
        /* Nested fault inside the bridge: stop faithfully, do not loop. */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    ovmx_in_accvio_handler = 1;

    ucontext_t *uc = (ucontext_t *)ucv;

    uint64_t va = si ? (uint64_t)(uintptr_t)si->si_addr : 0;
    uint64_t pc = 0;
    uint32_t psl = 0;
    if (uc) {
        /* On alpha-linux uc_mcontext IS struct sigcontext; sc_pc is the faulting
         * PC, sc_ps the processor status. */
        pc  = (uint64_t)uc->uc_mcontext.sc_pc;
        psl = (uint32_t)uc->uc_mcontext.sc_ps;
    }

    /* Reason: length violation (address in no mapped region) vs protection is
     * the faithful si_code distinction. The write/modify bit is not reliably
     * exposed by the Linux/Alpha signal frame, so it is left 0 (best-effort,
     * INV-6) rather than fabricated. */
    uint32_t reason = 0;
    if (si && si->si_code == SEGV_MAPERR)
        reason |= OVMX_ACCVIO_LENGTH_VIOLATION;

    /* Stash the FULL 64-bit state for the faithful read-back. */
    ovmx_last_accvio.valid  = 1;
    ovmx_last_accvio.reason = reason;
    ovmx_last_accvio.va     = va;
    ovmx_last_accvio.pc     = pc;
    ovmx_last_accvio.psl    = psl;

    ovmx_in_accvio_handler = 0;

    /* Raise the faithful SS$_ACCVIO condition {reason, va, pc, psl}. A handler
     * cannot CONTINUE an access violation (the instruction would re-fault); the
     * faithful response is SYS$UNWIND (rung-2), which transfers out via the CHF
     * unwind anchor and never returns here. If lib$signal DOES return (no
     * handler unwound), the condition is SEVERE and unhandled -> it becomes
     * $STATUS / terminates, exactly as an unhandled ACCVIO does on VMS. */
    lib$signal(SS$_ACCVIO, (uint32_t)4, reason,
               (uint32_t)(va & 0xffffffffU),
               (uint32_t)(pc & 0xffffffffU), psl);
}
#endif /* __alpha__ */

/* ================================================================
 * ovmx$accvio_signal_init - install the image-start access-violation bridge.
 *
 * Idempotent, safe to call more than once. Installs SIGSEGV/SIGBUS/SIGILL ->
 * SS$_ACCVIO for the current image. SA_NODEFER because a handler for a
 * non-resumable fault unwinds out (longjmp via the CHF anchor) rather than
 * returning, so the fault signal must not be left blocked at the resume site.
 * ================================================================ */
void ovmx$accvio_signal_init(void) {
#if defined(__alpha__)
    static int installed = 0;
    if (installed) return;
    installed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ovmx_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    ovmx$accvio_signal_installed = 1;
#endif
}

/* Unconditionally (re)install the bridge handler, bypassing the install-once
 * guard. Used by the acceptance test to restore the bridge after a raw-handler
 * substrate-fidelity probe; production code uses the auto-install constructor. */
void ovmx$accvio_signal_reinstall(void) {
#if defined(__alpha__)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ovmx_sigsegv_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    ovmx$accvio_signal_installed = 1;
#endif
}

/* ================================================================
 * Image-start auto-install (glibc/musl .init_array -- OVMX's LIB$INITIALIZE
 * equivalent). FORCE-PULL under -static: a constructor in an archive TU is
 * dropped unless the member is referenced. ovmx$accvio_signal_anchor is the
 * strong symbol the per-image anchor TU (accvio_signal_bind.c) references to
 * drag this TU (and its constructor) into a static image -- the established
 * pattern from arith_signal_bind.c / dcl_rms_bind.c.
 * ================================================================ */
__attribute__((used))
int ovmx$accvio_signal_anchor = 0;

__attribute__((constructor))
static void ovmx_accvio_signal_ctor(void) {
    ovmx$accvio_signal_init();
}

/* Read back the most recent access-violation state for the current thread, at
 * FULL 64-bit width (the longword signal array cannot carry a >32-bit va/pc).
 * Returns 1 if a valid violation is stashed, 0 otherwise. */
int vms$$last_accvio_exception(uint32_t *reason, uint64_t *va,
                               uint64_t *pc, uint32_t *psl) {
    if (!ovmx_last_accvio.valid) return 0;
    if (reason) *reason = ovmx_last_accvio.reason;
    if (va)     *va     = ovmx_last_accvio.va;
    if (pc)     *pc     = ovmx_last_accvio.pc;
    if (psl)    *psl    = ovmx_last_accvio.psl;
    return 1;
}
