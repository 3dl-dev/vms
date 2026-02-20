
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <ctype.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void usage (void) {

    (void)fprintf (stderr, "Usage: sys_perm_align_fault [on|off]\n");
}


/******************************************************************************/
int main (int argc, char *argv[]) {
/*
** Enables or disables process wide permanent alignment fault reporting. To
** use:
**
** $ cc sys_perm_align_fault
** $ link sys_perm_align_fault
** $ mcr []sys_perm_align_fault on
** $ run program    ! Program you wish to test for alignment faults
** $ mcr []sys_perm_align_fault off
**
** or you can define a foreign command instead of using mcr.
**
*/

static char *p;

static int r0_status;

    if (argc < 2) {
        usage ();
    } else {
        for (p = argv[1]; *p != '\0'; p++) {
             *p = tolower (*p);
        }
        if (strcmp (argv[1], "on") == 0) {
            r0_status = sys$perm_report_align_fault ();
            errchk_sig (r0_status);
            (void)fprintf (stderr, "Alignment fault reporting now on\n");
        } else {
            if (strcmp (argv[1], "off") == 0) {
                r0_status = sys$perm_dis_align_fault_report ();
                errchk_sig (r0_status);
                (void)fprintf (stderr, "Alignment fault reporting now off\n");
            } else {
                usage ();
            }
        }
    }
}
