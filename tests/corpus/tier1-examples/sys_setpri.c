
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
static unsigned int old_prio;
static unsigned int new_prio = 1;

    /*
    ** Set the current process's priority to 1.  Note to set priority
    ** above your authorized base priority, you need the ALTPRI privilege.
    */
    r0_status = sys$setpri (0,
                            0,
                            new_prio,
                            &old_prio,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Process base priority set to %u\n",
                  new_prio);

    /*
    ** Retstore the process's priority.
    */
    r0_status = sys$setpri (0,
                            0,
                            old_prio,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Process base priority restored to %u\n",
                  old_prio);
}
