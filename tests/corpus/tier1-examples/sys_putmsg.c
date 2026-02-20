
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
static struct {
    unsigned short int arg_count;
    unsigned short int msg_options;
    unsigned int code;
    unsigned int fao1;
    unsigned int fao2;
    unsigned int fao3;
    unsigned int fao4;
} sig_array;


    /*
    ** SYS$PUTMSG is usually called from a condition handler (see the
    ** "Programming Concepts Manual" for a discussion of error signalling
    ** and handling.  Here we are going to dummy one up so we can demo
    ** a call to the routine.  Normally, the operating system constructs
    ** the signal array (and another array called the mechanism array).
    **
    ** See the code linked to LIB$ESTABLISH for a demo of SYS$PUTMSG used
    ** in its normal environment.
    */

    sig_array.arg_count = 4;
    sig_array.msg_options = 0;
    sig_array.code = SS$_ACCVIO;
    sig_array.fao1 = 1;
    sig_array.fao2 = 2;
    sig_array.fao3 = 3;
    sig_array.fao4 = 4;

    r0_status = sys$putmsg (&sig_array,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);
}
