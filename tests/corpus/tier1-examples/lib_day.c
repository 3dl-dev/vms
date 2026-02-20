
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int days;

    r0_status = lib$day (&days,
                         0,
                         0);
    errchk_sig (r0_status);

    (void)printf ("It has been %d days "
                  "since the system zero time of 17-Nov-1858\n",
                  days);
}
