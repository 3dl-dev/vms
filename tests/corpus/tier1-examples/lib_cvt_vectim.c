
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

static char buffer[255+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

static GENERIC_64 binary_time;

static unsigned short vector[7] = { 2003, 2, 1, 3, 4, 5, 6 };

    r0_status = lib$cvt_vectim (vector,
                                &binary_time.gen64$q_quadword);
    errchk_sig (r0_status);

    r0_status = lib$sys_asctim (&buffer_d.dsc$w_length,
                                &buffer_d,
                                &binary_time,
                                0);
    errchk_sig (r0_status);

    (void)printf ("time = %-.*s\n",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
