
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>


/******************************************************************************/
int main (void) {

static GENERIC_64 binary_time;

static int r0_status;

static $DESCRIPTOR (time_d, "25-Sep-2003 11:05:03.10");

    r0_status = sys$bintim (&time_d,
                            &binary_time);
    if (!$VMS_STATUS_SUCCESS (r0_status)) {
        (void)lib$signal (r0_status);
    }
}
