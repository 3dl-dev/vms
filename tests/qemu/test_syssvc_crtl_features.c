/*
 * test_syssvc_crtl_features.c - the DEC C RTL feature switches (vms-db7).
 *
 *   - the table holds only what OVMX honours: DECC$FILE_SHARING is index 1; a name the
 *     table lacks (a made-up one, and DEC C's real DECC$UNIX_LEVEL, which OVMX does not
 *     implement) is -1 with errno EINVAL, never accepted and ignored;
 *   - index 0, a bad mode and an out-of-range value are -1/EINVAL;
 *   - a value set is the value read back, current and default kept apart;
 *   - (that DECC$FILE_SHARING changes how the C RTL opens a file is proven where the
 *     C RTL file layer runs: the alpha crtl-fd gate, crtl_fd_test.c check 41);
 *   - decc$set_reentrancy accepts the two defined levels and refuses any other.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#include "vms_kif.h"
#include "rms/rms.h"
#include "rms/crtl_features.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

int main(void)
{
    int idx;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_crtl_features: DEC C feature switches ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_crtl_features: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    check($VMS_STATUS_SUCCESS(vms_kif_acp_mount(ODS2_UNIT)), "VDA0: mounted");

    /* --- the table ------------------------------------------------------------ */
    idx = ovmx_crtl_feature_get_index("DECC$FILE_SHARING");
    check(idx == 1, "DECC$FILE_SHARING is feature index 1 (index 0 is never valid)");
    check(ovmx_crtl_feature_get_name(idx) != NULL &&
          strcmp(ovmx_crtl_feature_get_name(idx), "DECC$FILE_SHARING") == 0,
          "the index maps back to its name");

    errno = 0;
    /* negctl: crtl-feature-unknown-accepted */
    check(ovmx_crtl_feature_get_index("DECC$NO_SUCH_FEATURE") == -1 && errno == EINVAL,
          "an unknown feature name is -1/EINVAL");
    errno = 0;
    check(ovmx_crtl_feature_get_index("DECC$UNIX_LEVEL") == -1 && errno == EINVAL,
          "a real DEC C feature OVMX does not implement is -1/EINVAL (never accepted and ignored)");
    errno = 0;
    check(ovmx_crtl_feature_get_value(0, 1) == -1 && errno == EINVAL, "index 0 is -1/EINVAL");
    errno = 0;
    check(ovmx_crtl_feature_get_value(idx, 2) == -1 && errno == EINVAL, "a mode other than 0/1 is -1/EINVAL");
    errno = 0;
    check(ovmx_crtl_feature_set_value(idx, 1, 2) == -1 && errno == EINVAL,
          "a value outside the feature's range is -1/EINVAL");

    check(ovmx_crtl_feature_get_value(idx, 0) == 0 && ovmx_crtl_feature_get_value(idx, 1) == 0,
          "default and current are both 0 to begin with");
    check(ovmx_crtl_feature_set_value(idx, 1, 1) == 0, "set the current value to 1");
    /* negctl: crtl-feature-set-ignored */
    check(ovmx_crtl_feature_get_value(idx, 1) == 1 && ovmx_crtl_feature_get_value(idx, 0) == 0,
          "get_value reads back what set_value stored, default untouched");
    check(ovmx_crtl_feature_set_value(idx, 1, 0) == 0, "set it back to 0");

    /* --- reentrancy ------------------------------------------------------------- */
    check(ovmx_crtl_get_reentrancy() == OVMX_C_MULTITHREAD, "the C RTL starts multithread-safe");
    check(ovmx_crtl_set_reentrancy(OVMX_C_MULTITHREAD) == 0, "set_reentrancy(C$C_MULTITHREAD) is 0");
    errno = 0;
    check(ovmx_crtl_set_reentrancy(7) == -1 && errno == EINVAL, "an undefined level is -1/EINVAL");
    check(ovmx_crtl_get_reentrancy() == OVMX_C_MULTITHREAD, "a refused level leaves the recorded one");

    printf("=== test_syssvc_crtl_features: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
