
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
static unsigned int array[6];
static unsigned int count = 99;

static char output[255+1];

static struct dsc$descriptor_s output_d = { sizeof (output) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

static $DESCRIPTOR (ctrl_d,
                    "!2SL bottle!%S !AD !AS, !2SL bottle!%S of beer..."
                    "!/If one of those bottles of beer was to fall..."
                    "!/!2SL bottle!%S of beer.!/");
static $DESCRIPTOR (s1_d, "of beer");
static $DESCRIPTOR (s2_d, "on the wall");

    array[1] = s1_d.dsc$w_length;
    array[2] = (unsigned int)s1_d.dsc$a_pointer;
    array[3] = (unsigned int)&s2_d;

    for (count = 99; count > 1; count--) {
        array[0] = array[4] = count;
        array[5] = count - 1;

        r0_status = lib$sys_faol (&ctrl_d,
                                  &output_d.dsc$w_length,
                                  &output_d,
                                  array);
        errchk_sig (r0_status);

        (void)printf ("%-.*s\n",
                      output_d.dsc$w_length,
                      output_d.dsc$a_pointer);
    }
    (void)printf ("Time to earn some more beer vouchers!\n");
}
