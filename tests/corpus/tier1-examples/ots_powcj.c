
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <ots$routines.h>
#include <lib$routines.h>

#if __G_FLOAT != 0
#  define POWCJ ots$powcgj_r3
   extern double complex ots$powcgj_r3 (__unknown_params);
#elif __IEEE_FLOAT != 0
#  define POWCJ ots$powctj_r3
   extern double complex ots$powctj_r3 (__unknown_params);
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif


/******************************************************************************/
int main (void) {

static double complex c1 = 2.0 + 3.0*I;
static int j = 2;
static double complex r;

    r = cpowl (c1, j);
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));

    r = POWCJ (creal (c1),
               cimag (c1),
               j);
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));
}    
