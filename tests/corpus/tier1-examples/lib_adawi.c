
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static int r0_status;

static short int add = 2;
static short int sum = 2;
static short int sign;


    r0_status = lib$adawi (&add,
                           &sum,
                           &sign);
    if (!$VMS_STATUS_SUCCESS (r0_status)) {
        (void)lib$signal (r0_status);
    }

    (void)printf ("sum = %d, sign = %d\n",
                  sum,
                  sign);
}
