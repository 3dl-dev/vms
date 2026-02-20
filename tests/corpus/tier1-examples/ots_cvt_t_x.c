
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <ots$routines.h>
#include <lib$routines.h>

#include "errchk.h"

#if __G_FLOAT != 0
#  define CVT_T_X ots$cvt_t_g
#elif __D_FLOAT != 0
#  define CVT_T_X ots$cvt_t_d
#elif __IEEE_FLOAT != 0
#  define CVT_T_X ots$cvt_t_t
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif


/******************************************************************************/
int main (void) {

static int r0_status;

static double value;

static $DESCRIPTOR (value_d, "0.12456789+3");

    r0_status = CVT_T_X (&value_d,
                         &value,
                         0,
                         0,
                         0);
    errchk_sig (r0_status);

    (void)printf ("%f\n",
                  value);
}
