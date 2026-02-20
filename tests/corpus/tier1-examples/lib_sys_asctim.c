
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
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

static $DESCRIPTOR (input_d, "2 00:00:01.00");

static GENERIC_64 binary_time;

static char output[255+1];

static struct dsc$descriptor_s output_d = { sizeof (output) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

    r0_status = sys$bintim (&input_d,
                            &binary_time);
    errchk_sig (r0_status);

    r0_status = lib$sys_asctim (&output_d.dsc$w_length,
                                &output_d,
                                &binary_time,
                                0);
    errchk_sig (r0_status);

    (void)printf ("time = %-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
