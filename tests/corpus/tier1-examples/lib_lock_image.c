
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Example code calling lib$lock_image and lib$unlock_image, which are
** typically used to lock the image in the working set before getting to
** kernel mode and elevating IPL to 2 or more.
*/

static int r0_status;

    r0_status = lib$lock_image (0);
    errchk_sig (r0_status);

    (void)printf ("Image successfully locked in working set\n");

    r0_status = lib$unlock_image (0);
    errchk_sig (r0_status);

    (void)printf ("Image successfully unlocked\n");
}
