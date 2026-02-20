/*
 * lib_eventflags.c - LIB$ Event Flag and Terminal Routines
 *
 * Implements:
 *
 *   LIB$GET_EF    - Allocate a free local event flag number
 *   LIB$FREE_EF   - Release a previously allocated event flag
 *   LIB$LP_LINES  - Return lines per page for listing output
 *
 * On VMS, event flags are shared 32-bit registers in clusters of 32.
 * Local event flags 0-31 are cluster 0, flags 32-63 are cluster 1,
 * etc.  Flags 0-23 are reserved for system use; lib$get_ef allocates
 * from flags 24-63.
 *
 * This implementation maintains a per-process allocation bitmap using
 * a mutex for thread safety.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include "ssdef.h"
#include "lib$routines.h"

/* ================================================================
 * Event flag allocator
 * ================================================================ */

/*
 * VMS event flag ranges:
 *   0-23   Reserved for system use (we do not allocate these)
 *   24-63  Available to user images via lib$get_ef
 *   64-95  Available via sys$ascefc (associate common event flags)
 *   96-127 Available via sys$ascefc
 *
 * We manage flags 24-63 (40 flags) as a simple bitmap.
 */

#define EF_FIRST    24      /* First allocatable flag */
#define EF_LAST     63      /* Last allocatable flag */
#define EF_COUNT    (EF_LAST - EF_FIRST + 1)

/* Bit N in the bitmap represents flag (EF_FIRST + N).
 * Bit set = flag is in use. */
static uint64_t ef_bitmap = 0;
static pthread_mutex_t ef_lock = PTHREAD_MUTEX_INITIALIZER;

/* Error codes for lib$get_ef */
#define LIB$_EF_ALRFRE  0x00801814  /* Event flag already free */
#define LIB$_EF_RESSYS  0x00801824  /* Event flag reserved for system */
#define LIB$_INSEF      0x00801834  /* Insufficient event flags */

/*
 * lib$get_ef - Allocate a free local event flag.
 *
 * Scans the free-flag bitmap and allocates the first available flag
 * in the range 24-63. The allocated flag number is stored in *efn.
 *
 * Parameters:
 *   efn - Pointer to receive allocated event flag number
 *
 * Returns: SS$_NORMAL on success, LIB$_INSEF if none available
 */
uint32_t lib$get_ef(uint32_t *efn)
{
    if (!efn) return SS$_BADPARAM;

    pthread_mutex_lock(&ef_lock);

    for (int i = 0; i < EF_COUNT; i++) {
        uint64_t bit = (uint64_t)1 << (unsigned)i;
        if (!(ef_bitmap & bit)) {
            ef_bitmap |= bit;
            pthread_mutex_unlock(&ef_lock);
            *efn = (uint32_t)(EF_FIRST + i);
            return SS$_NORMAL;
        }
    }

    pthread_mutex_unlock(&ef_lock);
    return LIB$_INSEF;
}

/*
 * lib$free_ef - Release a previously allocated event flag.
 *
 * Marks the event flag *efn as available for future allocation.
 *
 * Parameters:
 *   efn - Pointer to event flag number to release
 *
 * Returns: SS$_NORMAL on success
 *          LIB$_EF_RESSYS if the flag is in the system-reserved range
 *          LIB$_EF_ALRFRE if the flag was not allocated
 */
uint32_t lib$free_ef(const uint32_t *efn)
{
    if (!efn) return SS$_BADPARAM;

    uint32_t flag = *efn;

    if (flag < EF_FIRST) return LIB$_EF_RESSYS;
    if (flag > EF_LAST)  return SS$_BADPARAM;

    int idx = (int)(flag - EF_FIRST);
    uint64_t bit = (uint64_t)1 << (unsigned)idx;

    pthread_mutex_lock(&ef_lock);

    if (!(ef_bitmap & bit)) {
        pthread_mutex_unlock(&ef_lock);
        return LIB$_EF_ALRFRE;
    }

    ef_bitmap &= ~bit;
    pthread_mutex_unlock(&ef_lock);

    return SS$_NORMAL;
}

/* ================================================================
 * LIB$LP_LINES
 * ================================================================ */

/*
 * lib$lp_lines - Lines per page for listing output.
 *
 * On VMS this returns the default number of lines per page for
 * listing-style output (typically 66 for a standard printer page).
 * The actual value may be affected by the LP$LINES logical name;
 * in our implementation we return the VMS default of 66.
 *
 * This function takes no arguments and returns the line count
 * directly (not a VMS status code).
 *
 * Returns: lines per page (66)
 */
uint32_t lib$lp_lines(void)
{
    return 66;
}
