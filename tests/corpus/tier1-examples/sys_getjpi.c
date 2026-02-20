
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void ast_routine (unsigned int efn) {

static int r0_status;

    r0_status = sys$setef (efn);
    errchk_sig (r0_status);
}


/******************************************************************************/
int main (void) {

static IOSB iosb;

static int r0_status;
static unsigned int pid;
static unsigned int group;
static unsigned int member;

static char username[12];
static unsigned short int username_len;
static unsigned int efn;

static ILE3 jpiitms[] = {  4, JPI$_PID, &pid, NULL,
                           4, JPI$_GRP, &group, NULL,
                           4, JPI$_MEM, &member, NULL,
                          12, JPI$_USERNAME, username, &username_len,
                           0, 0, NULL, NULL };

    /*
    ** There are quite a few important system services have both a
    ** synchronous and asynchronous version.  Both versions can set
    ** an event flag and call an AST routine on completion.  Here we
    ** will get an event flag to wait on, and rather than letting
    ** getjpi set it, we will set it in an AST routine.  Just for
    ** fun :-)
    */
    r0_status = lib$get_ef (&efn);
    errchk_sig (r0_status);

    /*
    ** Normally, we would pass the event flag to the system service
    ** which would take care of ensuring it was clear.  As we are
    ** not doing that, we need to clear it outselves.
    */
    r0_status = sys$clref (efn);
    errchk_sig (r0_status);

    /*
    ** Note the first parameter is EFN$C_ENF.  This tells the system
    ** service that we don't care about event flags.  It's a _very_
    ** good idea to specify this value if you don't care, because if
    ** you don't specify it, the value defaults to zero, which is a
    ** valid event flag that _will_ be set when the routine completes.
    ** If you have multiple threads of execution that default this to
    ** zero, you could easily get in trouble really quickly.
    **
    ** The last parameter is the user parameter passed to the AST
    ** routine.  In this case we are passing the value of the event
    ** flag.  We could just as easily have stuck it in a structure
    ** and passed the address of the structure.
    */
    r0_status = sys$getjpi (EFN$C_ENF,
                            0,
                            0,
                            jpiitms,
                            &iosb,
                            ast_routine,
                            efn);
    errchk_sig (r0_status);

    /*
    ** Wait for the event flag to be set by the AST.
    */
    r0_status = sys$waitfr (efn);
    errchk_sig (r0_status);

    /*
    ** Note that the I/O status block status is not set until after
    ** the asynch operation completes.
    */
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** The username is returned as a space filled 12 byte string.
    */
    for (; username_len - 1 >= 1; username_len--) {
        if (!isspace (username[username_len - 1])) {
            break;
        }
    }

    (void)printf ("Hello %-.*s, your process ID is %08x "
                  "and your UIC is [%o,%o]\n",
                  username_len,
                  username,
                  pid,
                  group,
                  member);

    r0_status = lib$free_ef (&efn);
    errchk_sig (r0_status);
}
