
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

    (void)printf ("Control returning to command level.\n"
                  "Type \"CONTINUE\" to resume...\n");
    r0_status = lib$pause ();
    errchk_sig (r0_status);

    (void)printf ("Program resumed.\n");
}
