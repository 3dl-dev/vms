
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <assert.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
static int user_procedure (unsigned int a1,
                           unsigned int a2,
                           struct dsc$descriptor_s *a3_d) {

    (void)printf ("In user_procedure with a1 = %u, a2 = %u, a3 = %-.*s\n",
                  a1,
                  a2,
                  a3_d->dsc$w_length,
                  a3_d->dsc$a_pointer);

    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int args[4];
static const $DESCRIPTOR (test_d, "test");

    /*
    ** Set up argument list.  First longword is count of remaining
    ** arguments.  Bad practice casting an address to a int.  Test
    ** with assert() before doing so.
    */
    args[0] = 3;
    args[1] = 1;
    args[2] = 2;
    assert (sizeof (unsigned int) == sizeof (void *));
    args[3] = (unsigned int)&test_d;

    /*
    ** Note that r0_status will receive the status returned from the
    ** called procedure.  The horrible cast is there to silence the
    ** compiler due to the fact that if we are using new starlet,
    ** the headers define the second argument of lib$callg as a
    ** pointer to a function with no parameters, returning int. 
    ** That's clearly not what we want unless we want to start
    ** digging into call frames to find our argument list.
    */
    r0_status = lib$callg (args,
                           (int (*)())user_procedure);
    errchk_sig (r0_status);
}
