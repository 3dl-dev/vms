
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
static int show_ret (void) {

    /*
    ** Establish lib$sig_to_ret as our condition handling routine.  This
    ** means that when we signal an exception from this routine (the
    ** show_ret () routine), the condition handler will handle the exception
    ** and cause our routine to return it as a return value rather than crash
    ** uncontrollably.
    */
    (void)lib$establish (lib$sig_to_ret);

    /*
    ** Signal an exception.
    */
    (void)lib$signal (SS$_ABORT);

    return SS$_NORMAL;
}


/******************************************************************************/
static int show_stop (void) {

    /*
    ** Establish lib$sig_to_stop as our condition handling routine.  This
    ** means that when we signal an exception from this routine (the
    ** show_stop () routine), the condition handler will handle the exception
    ** and cause the program to stop with the signalled status as the final
    ** status of the program.  When the program terminates, you can do a
    **
    **     $ SHOW SYMBOL $STATUS
    **
    ** to verify that the symbol's value is %x10000A84.
    */
    (void)lib$establish (lib$sig_to_stop);

    /*
    ** Signal an exception.
    */
    (void)lib$signal (0x00000a84);

    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {
/*
** Demonstrate two O/S supplied condition handlers.  For information on
** condition signalling and handling, see the "OpenVMS Programming Concepts
** Manual".
*/

static int r0_status;

    r0_status = show_ret ();
    (void)printf ("Return status from show_ret () is %08x, which "
                  "should equal %08x.\n",
                  r0_status,
                  SS$_ABORT);

    r0_status = show_stop ();
    (void)printf ("We will never get here\n");
}
