
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static $DESCRIPTOR (ctrl_d, "!SL !AS !%T");
static char output[255+1];
static struct dsc$descriptor_s output_d = { sizeof (output) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };
static int integer_arg = 5;
static $DESCRIPTOR (string_arg_d, "seconds from now:"); 


    r0_status = lib$sys_fao (&ctrl_d,
                             &output_d.dsc$w_length,
                             &output_d,
                             integer_arg,
                             &string_arg_d,
                             0);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
