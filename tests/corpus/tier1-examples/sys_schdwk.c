
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <time.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 delay;

static int r0_status;
static int i;

static $DESCRIPTOR (delay_d, "0 00:00:05.00");


    r0_status = sys$bintim (&delay_d,
                            &delay);
    errchk_sig (r0_status);

    for (i = 0; i < 2; i++) {
        r0_status = sys$schdwk (0,
                                0,
                                &delay,
                                0);
        errchk_sig (r0_status);

        if (i == 0) {
            (void)printf ("Sleeping...\n");
            r0_status = sys$hiber ();
            errchk_sig (r0_status);
            (void)printf ("Awake!\n");
        } else {
            (void)printf ("Decided against a nap\n");
            r0_status = sys$canwak (0,
                                    0);
            errchk_sig (r0_status);
        }
    }
}
