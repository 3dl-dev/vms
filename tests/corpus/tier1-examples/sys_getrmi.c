
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <rmidef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb;

static GENERIC_64 idle;

static int r0_status;
static unsigned int hib;
static unsigned int efn;

static ILE3 itemlist[] = { 8, RMI$_CPUIDLE, &idle.gen64$q_quadword, NULL,
                           4, RMI$_HIB, &hib, NULL,
                           0, 0, NULL, NULL };

    /*
    ** Because there is no synchronous version of SYS$GETRMI, you *must*
    ** get an event flag to synchronize on...
    */
    r0_status = lib$get_ef (&efn);
    errchk_sig (r0_status);

    r0_status = sys$getrmi (efn,
                            0,
                            0,
                            itemlist,
                            &iosb,
                            0,
                            0);
    errchk_sig (r0_status);
    r0_status = sys$synch (efn,
                           &iosb);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    (void)printf ("idle ticks: %Lu\nHIB processes: %u\n",
                  idle.gen64$q_quadword,
                  hib);

    r0_status = lib$free_ef (&efn);
    errchk_sig (r0_status);
}
