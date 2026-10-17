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

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

int main(void)
{
    printf("=== test_syssvc_dgblsc_nosuchsec ===\n");
    struct dsc$descriptor_s nm = dsc$init("NEVER_CREATED_SECTION");
    uint32_t ident[2] = {0, 0};

    uint32_t st = sys$dgblsc(0, &nm, ident);
    CHECK(st == SS$_NOSUCHSEC, "$DGBLSC of a never-created section returns SS$_NOSUCHSEC");
    CHECK(!(st & 1), "... and that status is not a success (low bit clear)");
    CHECK(SS$_NOSUCHSEC == 2424, "SS$_NOSUCHSEC is the oracle value 2424 (%X978)");
    CHECK(sys$dgblsc(0, NULL, ident) == SS$_BADPARAM, "$DGBLSC with no name is SS$_BADPARAM");

    printf("=== test_syssvc_dgblsc_nosuchsec: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
