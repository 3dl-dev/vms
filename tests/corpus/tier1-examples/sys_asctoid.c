
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
static unsigned int id_bin;
static unsigned int attrib;

static $DESCRIPTOR (id_d, "LOCAL");

    r0_status = sys$asctoid (&id_d,
                             &id_bin,
                             &attrib);
    if (r0_status == SS$_NOSUCHID) {
        (void)printf ("You have a rather broken system.  The LOCAL identifier"
                      "is missing.\n");
    } else {
        errchk_sig (r0_status);
        (void)printf ("Value of LOCAL identifier is %%X%x\n",
                      id_bin);
    }
}
