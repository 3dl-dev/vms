
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"

#if __G_FLOAT == 0
#  error "Compile with /FLOAT=G_FLOAT (which is the default on alpha)"
#endif


/******************************************************************************/
int main (void) {

/*
** Compute X^4 + 2*X^3 - X^2 + X - 3 using POLYG.
*/

static int r0_status;
static short int degree = 4;

static double x = 2.0;
static double coeff[] = { 1.0, 2.0, -1.0, 1.0, -3.0 };
static double result;

    r0_status = lib$polyg (&x,
                           &degree,
                           coeff,
                           &result);
    errchk_sig (r0_status);

    (void)printf ("Result = %f\n",
                  result);
}
