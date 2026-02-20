
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 time_now;

static int r0_status;

static unsigned short int numvec[7];

static char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    r0_status = sys$gettim (&time_now);
    errchk_sig (r0_status);

    r0_status = sys$numtim (numvec,
                            &time_now);
    errchk_sig (r0_status);

    (void)printf ("Time is %02hu-%s-%hu %02hu:%02hu:%02hu.%02hu\n",
                  numvec[2],
                  months[numvec[1] - 1],
                  numvec[0],
                  numvec[3],
                  numvec[4],
                  numvec[5],
                  numvec[6]);
}
