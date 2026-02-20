
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** You need PSWAPM privilege to run this program.
*/

static int r0_status;
static unsigned int flag = 1;

    (void)printf ("Disable process swapping\n");
    r0_status = sys$setswm (flag);
    switch (r0_status) {
        case SS$_WASCLR:
            (void)printf ("The process was not previously locked in the "
                          "balance set\n");
            flag = 0;
            break;
        case SS$_WASSET:
            (void)printf ("The process was previously locked in the "
                          "balance set\n");
            flag = 1;
            break;
        case SS$_NOPRIV:
            (void)fprintf (stderr, "You need PSWAPM privilege to run this!\n");
            exit (r0_status);
            break;
        default:
            errchk_sig (r0_status);
            break;
    }
    r0_status = sys$setswm (flag);
    errchk_sig (r0_status);

    (void)printf ("Reset to previous state\n");
}
