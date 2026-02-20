
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

static $DESCRIPTOR (program_d, "SYS$SYSTEM:MAIL.EXE");

    (void)printf ("About to run %-.*s.\nIf successful, control will not "
                  "return to this program.\n",
                  program_d.dsc$w_length,
                  program_d.dsc$a_pointer);

    r0_status = lib$run_program (&program_d);
    errchk_sig (r0_status);
}
