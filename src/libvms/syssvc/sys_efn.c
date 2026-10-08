/*
 * sys_efn.c - the event-flag completion step of the lock services.
 *
 * On VMS the services that set an event flag on completion set whichever flag the
 * caller names, flag 0 included; only EFN$C_ENF (128) and above mean "no flag".
 * Kept in its own translation unit so the one userspace act of $ENQ/$ENQW -- naming
 * the caller's flag -- is separable from the executive-supplied lock answer.
 */
#include <stdint.h>
#include "starlet.h"

void vms$$lock_complete_efn(uint32_t efn)
{
    if (efn < 128)
        (void)sys$setef(efn);
}
