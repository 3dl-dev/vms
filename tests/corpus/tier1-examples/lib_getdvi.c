
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>

/*
** This header includes the item codes for getdvi calls.
*/
#include <dvidef.h>

#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int item_code = DVI$_FREEBLOCKS;
static unsigned int freeblocks;

static const $DESCRIPTOR (device_d, "SYS$SYSDEVICE:");

    /*
    ** This procedure exists to make it simple to obtain one
    ** piece of information about a device.  To obtain multiple
    ** pieces of info in one call, use sys$getdvi.
    */
    r0_status = lib$getdvi (&item_code,
                            0,
                            &device_d,
                            &freeblocks,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("%-.*s has %u free blocks\n",
                  device_d.dsc$w_length,
                  device_d.dsc$a_pointer,
                  freeblocks);
}
