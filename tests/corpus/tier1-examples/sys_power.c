
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <powerdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifndef __ia64
#  error "IA64 specific code"
#endif /* __IA64 */


/******************************************************************************/
int main (void) {

static unsigned __int64 new_setting;
static unsigned __int64 old_setting;
static int r0_status;
static char was[30];

    new_setting = POWER$C_EFFICIENCY;
    r0_status = sys$power_control (new_setting,
                                   &old_setting);
    if (r0_status == SS$_WRONGSTATE) {
        (void)printf ("You need to set OS control in the iLO for this to "
                      "work\n");
    } else {
        errchk_sig (r0_status);
        (void)printf ("Was set to ");
        switch (old_setting) {
            case POWER$C_HIGH_PERF:
                (void)sprintf (was, "high performance");
                break;
            case POWER$C_LOW_POWER:
                (void)sprintf (was, "low power");
                break;
            case POWER$C_EFFICIENCY:
                (void)sprintf (was, "efficiency");
                break;
            default:
                (void)sprintf (was, "unknown");
                break;
        }

        (void)printf ("Was set to %s\n", was);
        (void)printf ("Now set to efficiency\n");

        r0_status = sys$power_control (old_setting);
        errchk_sig (r0_status);

        (void)printf ("Now reset to %s\n", was);
    }
}
