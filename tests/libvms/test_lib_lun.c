/*
 * test_lib_lun.c - LIB$GET_LUN / LIB$FREE_LUN against what OpenVMS VAX V7.3
 * did on VAX1 (2026-10-21, tests/lab/captures/lib-lun-20261021/):
 *   119, 118, ... 100 (highest first, SS$_NORMAL); then -1 + LIB$_INSLUN
 *   (%X001582B4) once the pool is empty; FREE of an allocated unit SS$_NORMAL,
 *   of it again LIB$_LUNALRFRE (%X001582BC), of 5 or a unit outside the pool
 *   LIB$_LUNRESSYS (%X001582C4); the freed unit is the next one handed out.
 * This host build carries the non-VAX pool, 100-299 (RTL help: "100 through
 * 299 on Alpha"), so 120-299 follow 119-100.
 */
#include <stdio.h>
#include <stdint.h>
#include "lib$routines.h"
#include "libdef.h"
#include "ssdef.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

int main(void)
{
    int32_t lun = 0, n;
    uint32_t st;
    int order_ok = 1;

    printf("=== test_lib_lun (LIB$GET_LUN / LIB$FREE_LUN) ===\n");

    check(LIB$_INSLUN == 0x001582B4u && LIB$_LUNALRFRE == 0x001582BCu &&
          LIB$_LUNRESSYS == 0x001582C4u, "status values are the $LIBDEF / observed ones");

    for (int want = 119; want >= 100; want--) {
        st = lib$get_lun(&lun);
        if (st != SS$_NORMAL || lun != want) order_ok = 0;
    }
    check(order_ok, "the first 20 units are 119 down to 100, each SS$_NORMAL (VAX1 order)");

    order_ok = 1;
    for (int want = 299; want >= 120; want--) {
        st = lib$get_lun(&lun);
        if (st != SS$_NORMAL || lun != want) order_ok = 0;
    }
    check(order_ok, "then 299 down to 120 (the rest of the 100-299 pool)");

    st = lib$get_lun(&lun);
    check(st == LIB$_INSLUN && lun == -1, "an empty pool is LIB$_INSLUN and -1");

    n = 100;
    check(lib$free_lun(&n) == SS$_NORMAL, "FREE_LUN of an allocated unit is SS$_NORMAL");
    check(lib$free_lun(&n) == LIB$_LUNALRFRE, "FREE_LUN of it again is LIB$_LUNALRFRE");
    n = 5;
    check(lib$free_lun(&n) == LIB$_LUNRESSYS, "FREE_LUN of 5 is LIB$_LUNRESSYS");
    n = 300;
    check(lib$free_lun(&n) == LIB$_LUNRESSYS, "FREE_LUN beyond the pool is LIB$_LUNRESSYS");
    n = -1;
    check(lib$free_lun(&n) == LIB$_LUNRESSYS, "FREE_LUN of -1 is LIB$_LUNRESSYS");

    st = lib$get_lun(&lun);
    check(st == SS$_NORMAL && lun == 100, "the freed unit is the next one handed out");

    printf("=== test_lib_lun: %s ===\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
