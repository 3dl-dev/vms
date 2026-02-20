
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

static char buffer[23+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

    /*
    ** Get the time and date.  Note this routine does not update the
    ** length of the string, but as it always returns 23, just set
    ** up the string descriptor to reflect that.
    */
    r0_status = lib$date_time (&buffer_d);
    errchk_sig (r0_status);

    (void)printf ("Time now is %-.*s\n",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
