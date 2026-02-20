
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 quad_time;

static int r0_status;
static unsigned int utc_time[4];

    r0_status = sys$getutc (utc_time);
    errchk_sig (r0_status);

    r0_status = sys$timcon (&quad_time,
                            utc_time,
                            0);
    errchk_sig (r0_status);
}
