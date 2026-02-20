
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>

#include "errchk.h"

#define NUM_DESC 20


/******************************************************************************/
int main (void) {

static int r0_status;
static int i;
static int length;
static int num_desc = NUM_DESC;

static struct dsc$descriptor_d darray[NUM_DESC] = { 0,
                                                    DSC$K_DTYPE_T,
                                                    DSC$K_CLASS_D,
                                                    NULL };

    /*
    ** Create some dynamic descriptors.
    */
    (void)printf ("Getting %i descriptors... ", num_desc);
    (void)fflush (stdout);
    for (i = 0; i < num_desc; i++) {
        length = i + 10;
        r0_status = lib$sget1_dd (&length,
                                  &darray[i]);
        errchk_sig (r0_status);
    }
    (void)printf ("done.\n");

    /*
    ** Free them all with one call.
    */
    r0_status = lib$sfreen_dd (&num_desc,
                               darray);
    errchk_sig (r0_status);
    (void)printf ("And deleted them successfully\n");
}
