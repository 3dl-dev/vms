
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static unsigned short int str_len;

static char str[] = "Generosity with strings is not generosity; It's a deal";

static struct dsc$descriptor_d output_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_D,
                                            NULL };

    str_len = strlen (str);
    r0_status = lib$scopy_r_dx (&str_len,
                                str,
                                &output_d);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    r0_status = lib$sfree1_dd ((unsigned __int64 *)&output_d);
    errchk_sig (r0_status);
}
