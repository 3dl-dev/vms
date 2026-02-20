
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
#  define CNVOUT ots$cnvout_g
#elif __D_FLOAT != 0
#  define CNVOUT ots$cnvout_d
#elif __IEEE_FLOAT != 0
#  define CNVOUT ots$cnvout_t
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif


/******************************************************************************/
int main (void) {

static int r0_status;

static double value = 2.71828182;

#define PRECISION 9
static char output[PRECISION+6];
static struct dsc$descriptor_s output_d = { sizeof (output),
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

    r0_status = CNVOUT (&value,
                        &output_d,
                        PRECISION);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
