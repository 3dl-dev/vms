/*
 * test_syssvc_efn0.c - event flag 0 is a real event flag (vms-f811 follow-up).
 *
 * On VMS the services that set an event flag on completion set whichever flag the
 * caller names -- flag 0 included; only EFN$C_ENF (128) means "no flag". OVMX's
 * completion sites used to treat efn == 0 as "no flag" (a leftover from the old
 * EFN$C_ENF == 0), so a program that passed 0 and then waited on or tested EF 0
 * never saw it set. This suite clears EF 0, calls services with efn 0 and reads
 * EF 0 back from the executive; it also checks EFN$C_ENF still sets nothing.
 *
 * Requires /dev/vms; without it, honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "efndef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

struct lksb_caller { uint16_t status, reserved; uint32_t lkid; char valblk[16]; };

static int ef0(void)
{
    uint32_t st = 0;
    if (!(sys$readef(0, &st) & 1)) return -1;
    return (int)(st & 1);
}

int main(void)
{
    printf("=== test_syssvc_efn0 ===\n");
    if (vms_kif_open() < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    if (!(vms_kif_register(NULL) & 1)) { printf("  FAIL: register\n"); return 1; }

    /* $ENQW with efn 0 sets EF 0 */
    struct lksb_caller lksb;
    struct dsc$descriptor_s res = { 9, DSC$K_DTYPE_T, DSC$K_CLASS_S, "EFN0TEST1" };
    memset(&lksb, 0, sizeof lksb);
    sys$clref(0);
    CHECK(ef0() == 0, "EF 0 is clear before the request");
    uint32_t st = sys$enqw(0, LCK$K_EXMODE, &lksb, 0, &res, 0, NULL, 0, NULL, 0, 0, NULL);
    CHECK((st & 1) && lksb.lkid != 0, "$ENQW (efn 0) grants the lock");
    CHECK(ef0() == 1, "$ENQW (efn 0) set EF 0");
    if (lksb.lkid) sys$deq(lksb.lkid, NULL, 0, 0);

    /* EFN$C_ENF sets nothing */
    memset(&lksb, 0, sizeof lksb);
    sys$clref(0);
    st = sys$enqw(EFN$C_ENF, LCK$K_EXMODE, &lksb, 0, &res, 0, NULL, 0, NULL, 0, 0, NULL);
    CHECK((st & 1) && ef0() == 0, "$ENQW (EFN$C_ENF) leaves EF 0 clear");
    if (lksb.lkid) sys$deq(lksb.lkid, NULL, 0, 0);

    /* $SETIMR with efn 0 sets EF 0 when the timer fires */
    uint64_t delta = (uint64_t)-1000000LL;   /* 100 ms relative */
    sys$clref(0);
    st = sys$setimr(0, &delta, NULL, 1, 0);
    CHECK(st & 1, "$SETIMR (efn 0) accepted");
    int fired = 0;
    for (int i = 0; i < 40 && !fired; i++) { usleep(100000); fired = (ef0() == 1); }
    CHECK(fired, "$SETIMR (efn 0) set EF 0 on expiry");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
