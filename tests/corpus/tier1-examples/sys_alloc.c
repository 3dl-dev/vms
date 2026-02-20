
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
/*
** This code allocates the console device.  This may or may not be a good
** idea on your system.  Change the value in the device_d variable to a
** tape device or something if it's a problem.
*/

static int r0_status;
static char phydev[255+1];
static struct dsc$descriptor_s phydev_d = { sizeof (phydev) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            phydev };
static $DESCRIPTOR (device_d, "_OPA0:");

    r0_status = sys$alloc (&device_d,
                           &phydev_d.dsc$w_length,
                           &phydev_d,
                           0,
                           0);
    if (r0_status == SS$_DEVALRALLOC) {
        (void)printf ("Device is already allocated to another user\n");
    } else {
        errchk_sig (r0_status);

        (void)printf ("Device %-.*s allocated\n",
                      phydev_d.dsc$w_length,
                      phydev_d.dsc$a_pointer);

        r0_status = sys$dalloc (&phydev_d,
                                0);
        errchk_sig (r0_status);
        (void)printf ("Device %-.*s deallocated\n",
                      phydev_d.dsc$w_length,
                      phydev_d.dsc$a_pointer);
    }
}
