
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
static $DESCRIPTOR (command_d, "SHOW SYMBOL $STATUS");

    /*
    ** If the following call is coded correctly, the program will exit
    ** and the command interpretor will execute a SHOW SYMBOL $STATUS.
    ** Control will not return to this program.  Why have the error
    ** check code then?  In case someone modifies the code at a later
    ** time so the call is somehow passed the argument incorrectly.
    ** Try removing the "&" and see what happens.  Defensive code is
    ** good.
    */
    r0_status = lib$do_command (&command_d);
    errchk_sig (r0_status);
}
