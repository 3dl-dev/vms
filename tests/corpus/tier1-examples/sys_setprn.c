
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static $DESCRIPTOR (name_d, "DemoProcessName");

    r0_status = sys$setprn (&name_d);
    if (r0_status == SS$_DUPLNAM) {
        (void)printf ("Sorry, someone is using that process name\n");
    } else {
        errchk_sig (r0_status);
    }
}
