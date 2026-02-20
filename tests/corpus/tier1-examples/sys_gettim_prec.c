
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

static GENERIC_64 time;
static int r0_status;

    r0_status = sys$gettim_prec (&time.gen64$q_quadword);
    if (r0_status == SS$_LOWPREC) {
        (void)printf ("Warning: low precision time returned\n");
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("100 nanosecond tics since November 17, 1858: %lld\n",
                  time.gen64$q_quadword);
} 
