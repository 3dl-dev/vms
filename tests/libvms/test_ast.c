/*
 * test_ast.c - sys$dclast / sys$setast: no silent fallback (vms-as1)
 *
 * This build environment has no /dev/vms (it is not the QEMU/kernel
 * runtime -- see CLAUDE.md Rule 9). src/libvms/syssvc/sys_ast.c now
 * routes $DCLAST/$SETAST exclusively through the kernel executive via
 * vms_kif; with no /dev/vms to open, it MUST fail honestly with
 * SS$_NOSUCHDEV rather than silently simulating AST delivery locally
 * (the per-process PCB+SIGUSR1 fake this epic replaced).
 *
 * The real "AST delivered by the executive" proof lives in the QEMU
 * kernel-executive CI job (tests/qemu/test_kmod_ast.c,
 * tests/qemu/test_kmod_lock_mproc.c), which loads vms.ko and drives
 * DCLAST/SETAST/DELIVERAST against a real /dev/vms -- this test only
 * proves the ground floor: on a host with no executive, sys$dclast/
 * sys$setast do not pretend to succeed.
 */

#include <stdio.h>
#include "starlet.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static void dummy_ast(uint32_t param) { (void)param; }

int main(void)
{
    printf("=== test_ast (no silent fallback) ===\n");

    /* Parameter validation happens before the /dev/vms check and needs
     * no device -- still must reject a NULL AST routine. */
    uint32_t s0 = sys$dclast(NULL, 0, 0);
    CHECK(s0 == SS$_BADPARAM, "sys$dclast(NULL astadr) rejected without opening /dev/vms");

    /* No /dev/vms in this environment (verified by the test harness --
     * this file only runs where that holds) -- $DCLAST must fail
     * honestly, not queue an AST nobody will ever deliver. */
    uint32_t s1 = sys$dclast(dummy_ast, 42, 3);
    CHECK(s1 == SS$_NOSUCHDEV, "sys$dclast with no /dev/vms returns SS$_NOSUCHDEV");

    uint32_t s2 = sys$setast(1);
    CHECK(s2 == SS$_NOSUCHDEV, "sys$setast(1) with no /dev/vms returns SS$_NOSUCHDEV");

    uint32_t s3 = sys$setast(0);
    CHECK(s3 == SS$_NOSUCHDEV, "sys$setast(0) with no /dev/vms returns SS$_NOSUCHDEV");

    printf("=== test_ast: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
