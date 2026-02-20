
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
static int sp_status;

static const $DESCRIPTOR (prompt_d, "Subprocess> ");

    (void)printf ("Spawning subprocess...\n");
    r0_status = lib$spawn (0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           &sp_status,
                           0,
                           0,
                           0,
                           &prompt_d,
                           0,
                           0);
    errchk_sig (r0_status);
    (void)printf ("Back in parent process.  The subprocess existed with "
                  "a status of %%x%08x\n",
                  sp_status);
}
