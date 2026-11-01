/*
 * lib_lun.c - LIB$GET_LUN / LIB$FREE_LUN, the processwide logical-unit pool.
 *
 * The OpenVMS RTL help (RTL_Routines LIB$ LIB$GET_LUN, read on VAX1) says the
 * routine "allocates one logical unit number from a processwide pool", returns
 * -1 in the argument when none is available, and "can allocate logical unit
 * numbers 100 through 119 on VAX, and 100 through 299 on Alpha".
 *
 * Observed on VAX1 (OpenVMS VAX V7.3, 2026-10-21, a MACRO-32 probe of our own,
 * tests/lab/captures/lib-lun-20261021/vax1-lib-lun.txt):
 *   - successive calls hand out 119, 118, ... 100 (highest first), SS$_NORMAL;
 *   - the 21st call returns -1 and LIB$_INSLUN (%X001582B4);
 *   - LIB$FREE_LUN of an allocated unit is SS$_NORMAL, of the same unit again
 *     LIB$_LUNALRFRE (%X001582BC), and of 5 or of 150 (outside 100-119)
 *     LIB$_LUNRESSYS (%X001582C4);
 *   - a freed unit is handed out again by the next LIB$GET_LUN.
 *
 * OVMX's pool is the VAX one when built for VAX, and 100 through 299 elsewhere
 * (the help's Alpha range). Above 119 the order is not one we observed; the
 * 120-299 block is handed out highest first, after 119-100, the same rule the
 * VAX pool follows.
 */

#include <stdint.h>
#include <pthread.h>
#include "ssdef.h"
#include "libdef.h"
#include "lib$routines.h"

#define LUN_FIRST  100
#if defined(__vax__) || defined(__vax)
#define LUN_LAST   119
#else
#define LUN_LAST   299
#endif
#define LUN_VAXTOP 119     /* the block handed out first, highest first */
#define LUN_COUNT  (LUN_LAST - LUN_FIRST + 1)

static uint8_t lun_used[LUN_COUNT];
static pthread_mutex_t lun_lock = PTHREAD_MUTEX_INITIALIZER;

static int lun_take_locked(int lo, int hi)
{
    for (int n = hi; n >= lo; n--) {
        if (!lun_used[n - LUN_FIRST]) {
            lun_used[n - LUN_FIRST] = 1;
            return n;
        }
    }
    return -1;
}

uint32_t lib$get_lun(int32_t *lun)
{
    if (!lun)
        return SS$_ACCVIO;

    pthread_mutex_lock(&lun_lock);
    int n = lun_take_locked(LUN_FIRST, LUN_VAXTOP);
    if (n < 0 && LUN_LAST > LUN_VAXTOP)
        n = lun_take_locked(LUN_VAXTOP + 1, LUN_LAST);
    pthread_mutex_unlock(&lun_lock);

    *lun = n;
    return n < 0 ? LIB$_INSLUN : SS$_NORMAL;
}

uint32_t lib$free_lun(const int32_t *lun)
{
    if (!lun)
        return SS$_ACCVIO;

    int32_t n = *lun;
    if (n < LUN_FIRST || n > LUN_LAST)
        return LIB$_LUNRESSYS;

    uint32_t st = SS$_NORMAL;
    pthread_mutex_lock(&lun_lock);
    if (!lun_used[n - LUN_FIRST])
        st = LIB$_LUNALRFRE;
    else
        lun_used[n - LUN_FIRST] = 0;
    pthread_mutex_unlock(&lun_lock);
    return st;
}
