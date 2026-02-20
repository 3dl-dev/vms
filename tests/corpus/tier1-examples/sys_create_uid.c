
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int uid[4];

    r0_status = sys$create_uid (uid);
    errchk_sig (r0_status);

    (void)printf ("uid: %08lX.%08lX.%08lX.%08lX\n",
                  uid[0],
                  uid[1],
                  uid[2],
                  uid[3]);
}
