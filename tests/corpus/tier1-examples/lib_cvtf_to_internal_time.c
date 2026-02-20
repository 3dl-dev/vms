
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
#  error "Please compile with CC/FLOAT=G_FLOAT (which is the default on alpha)"
#endif


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int operation = LIB$K_DELTA_DAYS_F;
static GENERIC_64 binary_time;
static float input_time = 0.25;
static char output[255+1];
static struct dsc$descriptor_s output_d = { sizeof (output) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

    /*
    ** 0.25 of a day is 6 hours.
    */
    r0_status = lib$cvtf_to_internal_time (&operation,
                                           &input_time,
                                           &binary_time.gen64$q_quadword);
    errchk_sig (r0_status);

    r0_status = lib$sys_asctim (&output_d.dsc$w_length,
                                &output_d,
                                &binary_time,
                                0);
    errchk_sig (r0_status);

    (void)printf ("delta = %-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
