
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <libdtdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#if __G_FLOAT == 0
#  error "Compile with CC/FLOAT=G_FLOAT (which is the default on alpha)
#endif


/******************************************************************************/
int main (void) {

static int r0_status;
static float result;
static $DESCRIPTOR (delta_d, "1 00:00:00.00");
static unsigned int operation = LIB$K_DELTA_WEEKS_F;
static GENERIC_64 binary_time;

    r0_status = sys$bintim (&delta_d,
                            &binary_time);
    errchk_sig (r0_status);

    /*
    ** 1 day / 1 week = 1 / 7 = 0.14286
    */
    r0_status = lib$cvtf_from_internal_time (&operation,
                                             &result,
                                             &binary_time.gen64$q_quadword);
    errchk_sig (r0_status);

    (void)printf ("1 day is %1.5f of a week\n",
                  result);
}
