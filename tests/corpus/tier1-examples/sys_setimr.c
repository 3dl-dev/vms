
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <efndef.h>
#include <descrip.h>
#include <time.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void timer_routine (void) {

static int r0_status;

    r0_status = sys$wake (0,
                          0);
    errchk_sig (r0_status);
}


/******************************************************************************/
int main (void) {

static GENERIC_64 time;

static int r0_status;
static int i;

static $DESCRIPTOR (time_d, "0 00:00:15.00");

    r0_status = sys$bintim (&time_d,
                            &time);
    errchk_sig (r0_status);

    for (i = 0; i < 2; i++) {
        r0_status = sys$setimr (EFN$C_ENF,
                                &time,
                                timer_routine,
                                1,
                                0);
        errchk_sig (r0_status);

        (void)printf ("Sleeping 15 seconds... ");
        (void)fflush (stdout);

        if (i == 0) {
	    (void)printf ("err, changed my mind.\n");
            r0_status = sys$cantim (1,
                                    0);
            errchk_sig (r0_status);
        } else {
            r0_status = sys$hiber ();
            errchk_sig (r0_status);
            (void)printf ("done!\n");
        }
    }
}
