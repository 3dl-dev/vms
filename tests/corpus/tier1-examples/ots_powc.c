
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <complex.h>
#include <math.h>
#include <ots$routines.h>
#include <lib$routines.h>

#if __G_FLOAT != 0
#  define POWC ots$powcgcg_r3
   extern double complex ots$powcgcg_r3 (__unknown_params);
#elif __IEEE_FLOAT != 0
#  define POWC ots$powctct_r3
   extern double complex ots$powctct_r3 (__unknown_params);
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif


/******************************************************************************/
int main (void) {

static double complex c1 = 2.0 + 3.0*I;
static double complex c2 = 1.0 + 2.0*I;
static double complex r;
static double x;
static double y;
static double p;
static double q;

    r = cpow (c1, c2);
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));

    x = creal (c1);
    y = cimag (c1);
    p = creal (c2);
    q = cimag (c2);
    r = POWC (x, y, p, q);
    (void)printf ("%f + i%f\n",
                 creal (r),
                 cimag (r));
}    
