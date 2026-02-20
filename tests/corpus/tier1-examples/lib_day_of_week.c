
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int day;

static GENERIC_64 binary_time;

static $DESCRIPTOR (date_d, "29-Feb-2000");

    r0_status = sys$bintim (&date_d,
                            &binary_time);
    errchk_sig (r0_status);

    r0_status = lib$day_of_week (&binary_time.gen64$q_quadword,
                                 &day);
    errchk_sig (r0_status);

    (void)printf ("%-.*s was a ",
                  date_d.dsc$w_length,
                  date_d.dsc$a_pointer);

    switch (day) {
        case 1:
            (void)printf ("Monday");
            break;
        case 2:
            (void)printf ("Tuesday");
            break;
        case 3:
            (void)printf ("Wednesday");
            break;
        case 4:
            (void)printf ("Thursday");
            break;
        case 5:
            (void)printf ("Friday");
            break;
        case 6:
            (void)printf ("Saturday");
            break;
        case 7:
            (void)printf ("Sunday");
            break;
        default:
            /*
            ** Never get here.
            */
            assert (0);
    }
    (void)printf ("\n");
}
