/*
 * test_syssvc_dgblsc_nosuchsec.c - $DGBLSC of a global section that was never
 * created reports SS$_NOSUCHSEC, not success (vms-d91).
 *
 * THE BUG THIS GUARDS. sys$dgblsc validated gsdnam and returned SS$_NORMAL
 * although OVMX keeps no named-global-section registry ($CRMPSC discards the
 * name): a delete of a section that never existed was reported as done -- a
 * success that shares no state with what it claims.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/dgblsc-fake-success.defect.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

int main(void)
{
    printf("=== test_syssvc_dgblsc_nosuchsec ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0 || !(ovmx_test_self(NULL) & 1)) {
        /* Executive ABSENT: assert the service fails honestly (one runtime),
         * then exit with the honest-skip code 77 that the kernel-executive
         * negative-control job requires of every public-sys$ suite (rc 0 would
         * be a fabricated success; rc 1 is "77 not reached"). */
        struct dsc$descriptor_s n0 = dsc$init("NEVER_CREATED_SECTION");
        uint32_t st0 = sys$dgblsc(0, &n0, NULL);
        if (st0 != SS$_NOSUCHDEV) {
            printf("  FAIL: with no executive $DGBLSC returned %u, expected SS$_NOSUCHDEV\n", st0);
            return 1;
        }
        printf("  PASS: with no executive $DGBLSC fails honestly (SS$_NOSUCHDEV)\n");
        printf("=== test_syssvc_dgblsc_nosuchsec: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    struct dsc$descriptor_s nm = dsc$init("NEVER_CREATED_SECTION");
    uint32_t ident[2] = {0, 0};

    uint32_t st = sys$dgblsc(0, &nm, ident);
    /* negctl: dgblsc-fake-success */
    CHECK(st == SS$_NOSUCHSEC, "$DGBLSC of a never-created section returns SS$_NOSUCHSEC");
    /* negctl: dgblsc-fake-success */
    CHECK(!(st & 1), "... and that status is not a success (low bit clear)");
    CHECK(SS$_NOSUCHSEC == 2424, "SS$_NOSUCHSEC is the oracle value 2424 (%X978)");
    CHECK(sys$dgblsc(0, NULL, ident) == SS$_BADPARAM, "$DGBLSC with no name is SS$_BADPARAM");

    printf("=== test_syssvc_dgblsc_nosuchsec: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
