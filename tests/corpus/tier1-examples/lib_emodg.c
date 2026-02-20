
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

#if __G_FLOAT == 0
#  error "Please compile with CC/FLOAT=G_FLOAT (which is the default on alpha)"
#endif /* __G_FLOAT */

static int r0_status;
static int integer;
static double multiplier = 0.5;
static double multiplicand = 3.14159265;
static double fraction;
static unsigned short mult_ext = 0;

    r0_status = lib$emodg (&multiplier,
                           &mult_ext,
                           &multiplicand,
                           &integer,
                           &fraction);
    errchk_sig (r0_status);

    (void)printf ("%lf * %lf = %d + %lf\n",
                  multiplicand,
                  multiplier,
                  integer,
                  fraction);
}
