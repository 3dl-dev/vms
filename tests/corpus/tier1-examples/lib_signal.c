
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

/*
** Around the time OpenVMS was ported from VAX to Alpha, OpenVMS
** Engineering started a project to write function prototypes for
** all the APIs available to a C program.  They made it conditional
** on the __NEW_STARLET declaration, meaning you can revert to the
** old function prototypes (that declare "unknown parameters" for
** all routines) by removing the define.  While this was a pretty good
** idea, I think it could have been better implemented, as you will
** see if you keep reading various code examples on this site.
**
** For now, all you need to know is that this line enforces type
** checking at compile time for the APIs.
*/
#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>

/*
** Include lib$ routines.
*/
#include <lib$routines.h>

/*
** This header contains the system service messages including lots of
** success and failure status codes.
*/
#include <ssdef.h>

/*
** This header includes structures and values associated with processing
** status codes, that is, signalling.
*/
#include <stsdef.h>


/******************************************************************************/
int main (void) {

static int r0_status;

    /*
    ** Demonstrate how to signal an error.  In this case, an access
    ** violation.
    */
    r0_status = SS$_ACCVIO;
    (void)lib$signal (r0_status);
}
