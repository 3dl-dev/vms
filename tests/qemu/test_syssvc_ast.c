/*
 * test_syssvc_ast.c - ASTs through the PUBLIC sys$ API, against a real
 * /dev/vms (vms-as1).
 *
 * ============================================================
 * WRITTEN AS THE PROOF THAT src/libvms/syssvc/sys_ast.c REACHES THE EXECUTIVE.
 *
 * Before vms-as1, sys_ast.c implemented the AST facility in per-process memory
 * (struct vms_pcb's ast[mode] queues + a SIGUSR1 handler) and never called the
 * executive. Every AST entry point of the kernel-interface client --
 * vms_kif_dclast, vms_kif_setast, vms_kif_deliverast -- had ZERO callers
 * product-wide, while src/kernel/vms_ast.c implemented a real 4-level queue on
 * the executive PCB that nothing reached. A VMS program calling $DCLAST got a
 * per-process answer and never learned the executive was not consulted -- the
 * INV-6 facade the executive-retrofit epic (vms-6b8) exists to kill.
 * ============================================================
 *
 * WHY THIS SUITE CAN TELL THE EXECUTIVE FROM A PER-PROCESS FAKE.
 *
 * $DCLAST is self-directed -- an AST is declared to the CALLING process -- so
 * the cross-process "A writes, B reads" discriminator the event-flag suite
 * uses does not apply. The discriminator here is LAYER, not process: PART 1
 * declares an AST through the PUBLIC sys$dclast and then reads it back through
 * the RAW kernel interface (vms_kif_deliverast). A sys_ast.c that still queued
 * ASTs in this image's own heap would leave the executive queue EMPTY, so the
 * raw kernel reader would return "nothing pending" and PART 1's astprm/astadr
 * assertions would fail. They can only pass if the public service put the AST
 * where the executive -- and therefore the kernel reader -- can see it.
 *
 * PART 2 then proves declare+deliver end to end through the public API: an AST
 * declared while delivery is disabled is NOT run until sys$setast(1) drains the
 * executive's queue, and it runs with the astprm the caller declared. PART 3
 * exercises the per-mode enable flag's previous-state return, which is the
 * property tests/qemu/facility_defects.sh's ast-setast-disable control mutates
 * in the kernel; both this suite and test_kmod_ast go red on it, one layer
 * apart, which is what proves this suite is not blind to the executive.
 *
 * Do not weaken it, and do not "fix" a future failure by asserting whatever
 * OVMX happens to do -- fix the product.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/*
 * ORACLE-PINNED (vms-68c, docs/oracle/vax73-event-flags.md): $SSDEF on the
 * reference lab VAX V7.3 gives SS$_WASCLR 1 (== SS$_NORMAL, which is VMS) and
 * SS$_WASSET 9. These come from ssdef.h; named here only for the assertion
 * text below.
 */

/* AST parameter magics -- distinct so a dropped/confused astprm is visible. */
#define MAGIC1  0x0A51u   /* PART 1: public-writer / kernel-reader round trip */
#define MAGIC2  0x1234u   /* PART 2: end-to-end delivery through the public API */

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* The delivered-AST recorder. volatile: written from the AST routine the
 * executive hands back and read by the assertions. */
static volatile uint32_t g_fired = 0;
static volatile uint32_t g_param = 0;

static void record_ast(uint32_t prm) {
    g_fired++;
    g_param = prm;
}

/*
 * Same rule as test_syssvc_ef_mproc.c: kif_bind() completes open->register
 * before every ioctl, so a public sys$ entry point needs no help from its
 * caller. The only /dev/vms call this file makes by hand is the open below,
 * and it makes it to decide skip-vs-run, nothing else.
 */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_ast (ASTs via the PUBLIC sys$ API) ===\n");

    if (!executive_present()) {
        /*
         * CI negative control (a rig booted without insmod'ing vms.ko).
         * vms-0ff removed OVMX's executive-absent state; even here the
         * no-fabricated-success property still holds and is still asserted: a
         * public sys$ entry point must never report success for an executive
         * operation it could not have performed.
         */
        printf("  FAIL: cannot open /dev/vms\n");
        st = sys$dclast(record_ast, MAGIC1, PSL_C_USER);
        printf("  INFO: sys$dclast with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "sys$dclast does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ast: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ==================================================================
     * PART 1 -- EXECUTIVE RESIDENCY. Public writer, kernel reader.
     *
     * Declare an AST through the PUBLIC sys$dclast, then read it back through
     * the RAW kernel interface. A per-process userspace fake in sys_ast.c
     * would keep the AST in this image's heap, where the kernel reader cannot
     * see it -- so these assertions are exactly what a facade fails.
     * ================================================================== */
    st = sys$dclast(record_ast, MAGIC1, PSL_C_USER);
    printf("  INFO: sys$dclast(record_ast, 0x%x, USER) returned status %u\n", MAGIC1, st);
    CHECK(st & 1, "sys$dclast reported success declaring an AST");

    /* Enable the current mode's queue and read the head back through the
     * kernel. vms_kif_* uses this thread's own /dev/vms descriptor, but the
     * executive keys the AST queue by PROCESS (tgid), so it is the SAME queue
     * the public sys$dclast just wrote to. */
    (void)vms_kif_setast(1);
    {
        uint64_t dlv_astadr = 0, dlv_astprm = 0;
        uint8_t  dlv_acmode = 0xFF;
        int rc = vms_kif_deliverast(&dlv_astadr, &dlv_astprm, &dlv_acmode);
        printf("  INFO: kernel vms_kif_deliverast rc=%d astprm=0x%llx acmode=%u\n",
               rc, (unsigned long long)dlv_astprm, dlv_acmode);
        CHECK(rc == 0,
              "the AST declared through the PUBLIC sys$dclast is in the EXECUTIVE queue (a per-process fake would be invisible to the kernel reader)");
        CHECK(dlv_astprm == MAGIC1,
              "the executive round-tripped the astprm the public sys$dclast declared");
        CHECK(dlv_astadr == (uint64_t)(uintptr_t)record_ast,
              "the executive round-tripped the astadr the public sys$dclast declared");
        CHECK(dlv_acmode == PSL_C_USER,
              "the executive round-tripped the access mode the public sys$dclast declared");
    }

    /* ==================================================================
     * PART 2 -- DECLARE + DELIVER, end to end through the public API.
     *
     * An AST declared while delivery is disabled must not run until
     * sys$setast(1) drains the executive's queue, and it must run with the
     * astprm the caller declared.
     * ================================================================== */
    g_fired = 0;
    g_param = 0;

    (void)sys$setast(0);   /* disable delivery for this mode */

    st = sys$dclast(record_ast, MAGIC2, PSL_C_USER);
    printf("  INFO: sys$dclast(record_ast, 0x%x, USER) [delivery disabled] returned status %u\n",
           MAGIC2, st);
    CHECK(st & 1, "sys$dclast queued an AST while delivery was disabled");
    CHECK(g_fired == 0,
          "the AST is NOT delivered while AST delivery is disabled");

    st = sys$setast(1);    /* enable -> drain -> record_ast runs */
    printf("  INFO: sys$setast(1) returned status %u; g_fired=%u g_param=0x%x\n",
           st, g_fired, g_param);
    CHECK(g_fired == 1,
          "sys$setast(1) delivered the queued AST through the public API");
    CHECK(g_param == MAGIC2,
          "the delivered AST ran with the astprm sys$dclast declared");

    /* ==================================================================
     * PART 3 -- the per-mode enable flag's PREVIOUS-STATE return. This is the
     * property tests/qemu/facility_defects.sh's ast-setast-disable control
     * mutates in kernel/vms_ast.c; the assertion texts below are shared,
     * deliberately, with test_kmod_ast.c one layer down, so the same injected
     * defect reddens both. Delivery is drained above, so no AST is pending
     * here and sys$setast(1) delivers nothing.
     * ================================================================== */
    (void)sys$setast(0);   /* prev was enabled -> WASSET (not asserted) */

    st = sys$setast(0);    /* prev was disabled -> WASCLR */
    /* negctl: ast-setast-disable */
    CHECK(st == SS$_WASCLR, "disable again: prev state was disabled");

    st = sys$setast(1);    /* prev was disabled -> WASCLR */
    /* negctl: ast-setast-disable */
    CHECK(st == SS$_WASCLR,
          "SETAST(enable) returns WASCLR (== prev state was disabled)");

    printf("=== test_syssvc_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
