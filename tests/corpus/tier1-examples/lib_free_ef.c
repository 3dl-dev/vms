
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>

/*
** Include system service message codes
*/
#include <ssdef.h>

/*
** Include status processing
*/
#include <stsdef.h>

/*
** Include the lib$ routines
*/
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int efn;

    /*
    ** Get an event flag so we can demonstrate freeing it.
    */
    r0_status = lib$get_ef (&efn);
    if (!$VMS_STATUS_SUCCESS (r0_status)) {
        (void)lib$signal (r0_status);
    }

    (void)printf ("Got event flag %u to play with\n", efn);

    /*
    ** Free the event flag.
    */
    r0_status = lib$free_ef (&efn);
    if (!$VMS_STATUS_SUCCESS (r0_status)) {
        (void)lib$signal (r0_status);
    }
}
