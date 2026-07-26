/*
 * test_lock_constants.c - Compat-surface regression guard for the lock API.
 *
 * The public $ENQ/$DEQ constants in starlet.h ($LCKDEF) and ssdef.h ($SSDEF)
 * MUST carry authentic OpenVMS values — they are the source/binary
 * compatibility contract. They previously drifted to internal-kernel /
 * invented values (vms-b27); this test pins them so any future drift fails
 * the build instead of silently breaking real VMS programs.
 *
 * Values: authentic OpenVMS $LCKDEF/$SSDEF (single-lineage community
 * reproduction — FreeVMS lckdef.h, Nankervis/ODS2 ssdef.h; verified
 * 2026-07-26). See the provenance comments in starlet.h / ssdef.h.
 *
 * NOTE: the kernel lock manager uses a DIFFERENT internal bitmask
 * (src/kernel/vms_ioctl.h LCK_M_*); sys_lock.c translates at the /dev/vms
 * boundary. This test guards the PUBLIC values only — the translation and
 * the full $ENQ path through the kernel are exercised by the QEMU tests.
 */
#include <stdio.h>
#include <starlet.h>
#include <ssdef.h>

static int fails;

#define CHECK(expr, want) do {                                            \
    long _g = (long)(expr);                                               \
    if (_g != (long)(want)) {                                             \
        printf("  FAIL: %-18s = %ld (0x%lx), expected %ld (0x%lx)\n",     \
               #expr, _g, _g, (long)(want), (long)(want));                \
        fails++;                                                          \
    }                                                                     \
} while (0)

int main(void)
{
    /* $LCKDEF lock modes */
    CHECK(LCK$K_NLMODE, 0);
    CHECK(LCK$K_CRMODE, 1);
    CHECK(LCK$K_CWMODE, 2);
    CHECK(LCK$K_PRMODE, 3);
    CHECK(LCK$K_PWMODE, 4);
    CHECK(LCK$K_EXMODE, 5);

    /* $LCKDEF flag bits — real OpenVMS layout (NOT the kernel's LCK_M_*) */
    CHECK(LCK$M_VALBLK,  0x0001);
    CHECK(LCK$M_CONVERT, 0x0002);
    CHECK(LCK$M_NOQUEUE, 0x0004);
    CHECK(LCK$M_SYNCSTS, 0x0008);
    CHECK(LCK$M_SYSTEM,  0x0010);

    /* $SSDEF lock-manager status codes */
    CHECK(SS$_NOTQUEUED,   2488);
    CHECK(SS$_DEADLOCK,    3594);
    CHECK(SS$_VALNOTVALID, 2544);
    CHECK(SS$_CVTUNGRANT,  8508);
    CHECK(SS$_IVLOCKID,    8484);
    CHECK(SS$_SUBLOCKS,    8492);
    CHECK(SS$_EXENQLM,    10820);

    if (fails) {
        printf("%d lock-constant mismatch(es) — compat surface has drifted\n",
               fails);
        return 1;
    }
    printf("All lock constants match authentic OpenVMS values.\n");
    return 0;
}
