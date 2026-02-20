
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <efndef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void timer_ast (void) {
/*
** This timer AST executes in executive mode to resume the current process.
*/
static int r0_status;

    r0_status = sys$resume (0, 0);
    errchk_sig (r0_status);
}


/******************************************************************************/
static int exec_routine (GENERIC_64 *delay) {
/*
** This routine executes in executive mode to establish a timer to expire in
** 15 seconds.  When the timer expires, an AST is delivered in executive mode
** to resume the current process.
*/

    return (sys$setimr (EFN$C_ENF,
                        delay,
                        timer_ast,
                        0,
                        0));
}


/******************************************************************************/
int main (void) {
/*
** You need CMEXEC to run this program.  Note that SYS$SUSPND and SYS$RESUME
** require no privs to act on processes with your own PID.  The only reason
** we are using executive mode here is to demonstrate the calls within the
** one program.
*/

static GENERIC_64 delay;
static int r0_status;

static struct {
    int count;
    GENERIC_64 *delay;
} exec_args = { 1, &delay };

static $DESCRIPTOR (delay_d, "0 00:00:15.00");

    /*
    ** Convert 15 seconds to binary.
    */
    r0_status = sys$bintim (&delay_d,
                            &delay);
    errchk_sig (r0_status);

    /*
    ** Get to executive mode to establish a timer.  We are switching to
    ** exec because a suspended process can only receive exec or kernel ASTs.
    ** Attempts to call SYS$RESUME from a user mode routine will succeed but
    ** accomplish nothing.
    */
    r0_status = sys$cmexec (exec_routine,
                            (unsigned int *)&exec_args);
    errchk_sig (r0_status);

    /*
    ** Suspend ourselves and await the timer AST to resume us.
    */
    (void)printf ("Process suspending for 15 seconds...\n");
    r0_status = sys$suspnd (0,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Process resuming...\n");
}
