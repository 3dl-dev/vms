/*
 * test_accvio_dispatch.c - the Alpha access-violation -> SS$_ACCVIO condition
 * bridge, end to end, under -static (rd vms-cc8, CHF rung-4, gap G6).
 *
 * Proves the G6 executive/signal primitive: a real hardware access violation on
 * OVMX/Linux-Alpha is delivered as SIGSEGV and turned into a FAITHFUL SS$_ACCVIO
 * condition dispatched through the SAME dispatch_condition() the software path
 * uses -- so a SYS$SETEXV primary vector and the established frame-handler chain
 * are searched in the authentic order on a REAL trap, and (since an access
 * violation is not resumable) a handler transfers control out via SYS$UNWIND
 * (rung-2). This is the hardware half of "one dispatcher serves software signals
 * AND hardware exceptions": the arithmetic bridge (test_arith_hparith) proved it
 * for SS$_HPARITH; this proves it for SS$_ACCVIO.
 *
 * Structured (per the design review, mirroring test_arith_hparith) so a SUBSTRATE
 * fidelity failure is DISTINGUISHABLE from a bridge bug: CHECK 2 catches a real
 * SIGSEGV with a RAW handler and asserts the substrate reports the faulting
 * address faithfully BEFORE the full chain runs. If CHECK 2 fails under
 * qemu-system TCG, that is "blocked-on-substrate" (INV-6, honest stop), never a
 * faked trap.
 *
 * Alpha-only (the fault-delivery + sc_pc machine context are Alpha; the bridge
 * itself is #if __alpha__). On other arches it honest-skips (exit 77).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Reason bits (must match src/libvms/rtl/accvio_signal.c). */
#define ACCVIO_LENGTH_VIOLATION 0x1

#if defined(__alpha__)
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>
#include "ssdef.h"
#include "chfdef.h"
#include "starlet.h"
#include "lib$routines.h"

/* Bridge surface (src/libvms/rtl/accvio_signal.c). */
extern int  ovmx$accvio_signal_installed;
extern void ovmx$accvio_signal_reinstall(void);
extern int  vms$$last_accvio_exception(uint32_t *reason, uint64_t *va,
                                       uint64_t *pc, uint32_t *psl);

/* A recognizable, reliably-unmapped user address (page-aligned, well away from
 * any mapping). Dereferencing it is a genuine length-violation access fault. */
#define BAD_ADDR ((volatile int *)(uintptr_t)0xdead0000UL)

/* ---- CHECK 2: raw substrate-fidelity probe ---- */
static sigjmp_buf raw_jb;
static volatile uintptr_t raw_addr;
static volatile int raw_code;
static void raw_segv(int s, siginfo_t *si, void *ucv) {
    (void)s; (void)ucv;
    raw_addr = si ? (uintptr_t)si->si_addr : 0;
    raw_code = si ? si->si_code : 0;
    siglongjmp(raw_jb, 1);
}

/* ---- CHECK 3: dispatch order + unwind recovery ---- */
static volatile int seq;
static volatile int order_primary;
static volatile int order_frame;
static volatile int cond_caught;
static volatile uint32_t cond_name;
static volatile uint64_t caught_va;
static volatile uint32_t caught_reason;

static volatile int after_fault_ran;
static volatile int resumed;

/* PRIMARY software-exception vector: records that it ran first, then resignals
 * so the search proceeds to the frame-handler chain. */
static uint32_t prim_resignal(struct chf$signal_array *sig,
                              struct chf$mech_array *mech) {
    (void)sig; (void)mech;
    order_primary = ++seq;
    return SS$_RESIGNAL;
}

/* Frame handler: catches SS$_ACCVIO, captures the faithful full-width fault
 * state, and (an access violation is not resumable) unwinds to the establisher
 * frame's anchor via SYS$UNWIND. */
static uint32_t accvio_handler(struct chf$signal_array *sig,
                               struct chf$mech_array *mech) {
    cond_name = sig->chf$is_sig_name;
    if (sig->chf$is_sig_name == (uint32_t)SS$_ACCVIO) {
        cond_caught = 1;
        order_frame = ++seq;
        uint32_t reason = 0, psl = 0; uint64_t va = 0, pc = 0;
        if (vms$$last_accvio_exception(&reason, &va, &pc, &psl)) {
            caught_va = va;
            caught_reason = reason;
        }
        /* Deferred unwind to the establisher's frame (rung-2). */
        (void)sys$unwind(&mech->chf$is_mch_depth, 0);
        return SS$_CONTINUE;
    }
    return SS$_RESIGNAL;
}

static void guarded(void) {
    (void)lib$establish((void *)accvio_handler);
    sys$setexv(CHF$K_PRIMARY_VECTOR, (void *)prim_resignal, 0, NULL);

    if (VMS$UNWIND_ANCHOR()) {
        /* Transferred back here by SYS$UNWIND after the handler ran. */
        resumed = 1;
        return;
    }

    /* A REAL hardware access violation (write to an unmapped page). */
    *BAD_ADDR = 0x1;

    /* Must NOT execute: control was transferred to the anchor above. */
    after_fault_ran = 1;
}

int main(void) {
    printf("=== test_accvio_dispatch (Alpha access-violation -> SS$_ACCVIO bridge, -static) ===\n");

    /* CHECK 1: the image-start auto-install fired under -static (anchor + ctor). */
    CHECK(ovmx$accvio_signal_installed == 1,
          "image-start SIGSEGV bridge auto-installed under -static (anchor + constructor)");

    /* CHECK 2: SUBSTRATE FIDELITY (raw, independent of the bridge). A real wild
     * write must be delivered as SIGSEGV with a faithful faulting address. A
     * failure here = blocked-on-substrate, NOT a bridge bug. */
    struct sigaction raw = {0}, saved;
    raw.sa_sigaction = raw_segv;
    raw.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&raw.sa_mask);
    sigaction(SIGSEGV, &raw, &saved);
    raw_addr = 0; raw_code = 0;
    if (!sigsetjmp(raw_jb, 1)) {
        *BAD_ADDR = 0x2;
        printf("  INFO: raw wild write did NOT trap (substrate did not deliver SIGSEGV)\n");
    }
    CHECK(raw_addr == (uintptr_t)BAD_ADDR,
          "substrate delivers SIGSEGV with the faithful faulting address (si_addr)");

    /* Restore the bridge handler that CHECK 2 overrode. */
    ovmx$accvio_signal_reinstall();

    /* CHECK 3: FULL CHAIN on a REAL trap. Establish a frame handler + a PRIMARY
     * vector, fault, and assert the authentic search order + unwind recovery. */
    seq = 0; order_primary = 0; order_frame = 0;
    cond_caught = 0; cond_name = 0; caught_va = 0; caught_reason = 0;
    after_fault_ran = 0; resumed = 0;

    guarded();

    /* Clean up the process-global primary vector + frame handler. */
    sys$setexv(CHF$K_PRIMARY_VECTOR, NULL, 0, NULL);
    (void)lib$revert();

    CHECK(cond_caught == 1,
          "bridge raised a condition to the established handler on a real access violation");
    CHECK(cond_name == (uint32_t)SS$_ACCVIO,
          "condition raised is SS$_ACCVIO (hardware fault -> same dispatch_condition)");
    CHECK(order_primary != 0 && order_frame != 0,
          "both the PRIMARY exception vector and the frame handler ran");
    CHECK(order_primary < order_frame,
          "PRIMARY vector is searched BEFORE the frame chain on a real hardware trap");
    CHECK(resumed == 1 && after_fault_ran == 0,
          "handler transferred control via SYS$UNWIND (code after the fault did NOT run)");
    CHECK(caught_va == (uint64_t)(uintptr_t)BAD_ADDR,
          "faithful 64-bit faulting VA available via vms$$last_accvio_exception");
    CHECK((caught_reason & ACCVIO_LENGTH_VIOLATION) != 0,
          "reason carries the length-violation bit (unmapped address, from si_code)");

    printf("=== test_accvio_dispatch: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

#else  /* !__alpha__ */
int main(void) {
    printf("=== test_accvio_dispatch: SKIPPED (Alpha-only: fault-delivery + sc_pc machine context) ===\n");
    return EXIT_SKIP;
}
#endif
