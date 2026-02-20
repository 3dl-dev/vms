
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

/*
** Divide 4600387192 by 4096.
*/

static int r0_status;

static int divisor = 4096;
static __int64 dividend = 4600387192;
static int quotient;
static int remainder;

    r0_status = lib$ediv (&divisor,
                          &dividend,
                          &quotient,
                          &remainder);
    errchk_sig (r0_status);

    (void)printf ("quotient = %d, remainder = %d\n",
                  quotient,
                  remainder);
}
