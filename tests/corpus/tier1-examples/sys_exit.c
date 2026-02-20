
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <starlet.h>


/******************************************************************************/
int main (void) {

static int r0_status;

    /*
    ** OK, for consistancy I can see it, but why does a call thats sole
    ** purpose in life is to exit the program return a status?  I mean,
    ** what's going to check it?
    */

    r0_status = sys$exit (SS$_POWERFAIL);
}
