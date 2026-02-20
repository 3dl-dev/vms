
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <chfdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"



/******************************************************************************/
static int handler (struct chf$signal_array *sig_array,
                    struct chf$mech_array *mech_array) {
/*
** This function is a VMS condition handler.  See the "Programming Concepts
** Manual" for a discussion of condition signalling and handling.
*/

static int r0_status; 

    (void)printf ("Executing handler\n");
    r0_status = sys$putmsg (sig_array,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Returning control to main ()\n");
    return SS$_CONTINUE;
}


/******************************************************************************/
int main (void) {

    /*
    ** Establish a condition handler
    */
    (void)lib$establish (&handler);

    /*
    ** Signal a condition that will be handled by the handler.
    */
    (void)lib$signal (SS$_ACCVIO);

    (void)printf ("Note we didn't crash the program "
                  "and are now back in main ()\n");

    /*
    ** Now remove our condition handler.  If we signal a condition after
    ** the lib$revert () call, the program will crash.
    */
    (void)lib$revert ();

    /*
    ** Crash the program with an access violation.
    */
    (void)lib$signal (SS$_ACCVIO);
}
