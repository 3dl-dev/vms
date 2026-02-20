
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int reset;

    r0_status = sys$setrwm (1);
    switch (r0_status) {
        case SS$_WASCLR:
            (void)printf ("Resource wait mode was previously on\n");
            reset = 0;
            break;
        case SS$_WASSET:
            (void)printf ("Resource wait mode was previously off\n");
            reset = 1;
            break;
        default:
            errchk_sig (r0_status);
            break;
    }
    (void)printf ("And is now off\n");
    r0_status = sys$setrwm (reset);
    errchk_sig (r0_status);

    (void)printf ("And is now reset to %s\n", reset ? "off" : "on");
}
