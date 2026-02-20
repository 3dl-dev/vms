
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

    reset = sys$subsystem (1);
    switch (reset) {
        case SS$_WASCLR:
            (void)printf ("Subsystem identifiers were enabled\n");
            reset = 0;
            break;
        case SS$_WASSET:
            (void)printf ("Subsystem identifiers were disabled\n");
            reset = 1;
            break;
        default:
            errchk_sig (r0_status);
            break;
    }
    (void)printf ("And are now disabled\n");

    r0_status = sys$subsystem (reset);
    errchk_sig (r0_status);
    (void)printf ("And are now reset to %s\n", reset ? "disabled" : "enabled");
}
