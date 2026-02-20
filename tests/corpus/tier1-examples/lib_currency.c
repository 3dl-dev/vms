
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

static char buffer[255+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

    r0_status = lib$currency (&buffer_d,
                              &buffer_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("Currency symbol = \"%-.*s\"\n",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
