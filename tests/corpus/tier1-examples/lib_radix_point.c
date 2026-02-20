
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static char string[255+1];
static struct dsc$descriptor_s string_d = { sizeof (string) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            string };

    r0_status = lib$radix_point (&string_d,
                                 &string_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("Radix point is \"%-.*s\"\n",
                  string_d.dsc$w_length,
                  string_d.dsc$a_pointer);
}
