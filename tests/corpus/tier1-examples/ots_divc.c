
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <ots$routines.h>
#include <lib$routines.h>

#if __G_FLOAT != 0
#  define DIVC ots$divcg_r3
   extern double complex ots$divcg_r3 (__unknown_params);
#elif __IEEE_FLOAT != 0
#  define DIVC ots$divct_r3
   extern double complex ots$divct_r3 (__unknown_params);
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif


/******************************************************************************/
int main (void) {

static double complex c1 = 8.0 + 4.0*I;
static double complex c2 = 1.0 + 1.0*I;
static double complex r;

    r = c1 / c2;
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));

    r = DIVC (creal (c1),
              cimag (c1),
              creal (c2),
              cimag (c2));
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));
}    
