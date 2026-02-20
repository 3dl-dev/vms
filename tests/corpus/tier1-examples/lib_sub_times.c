
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

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 time1;
static GENERIC_64 time2;
static GENERIC_64 resultant_time;

static int r0_status;

static $DESCRIPTOR (time1_d, "25-Sep-2003 19:00:00.00");
static $DESCRIPTOR (time2_d, "1 01:00:00.00");

    r0_status = sys$bintim (&time1_d,
                            &time1);
    errchk_sig (r0_status);

    r0_status = sys$bintim (&time2_d,
                            &time2);
    errchk_sig (r0_status);

    r0_status = lib$sub_times (&time1.gen64$q_quadword,
                               &time2.gen64$q_quadword,
                               &resultant_time.gen64$q_quadword);
    errchk_sig (r0_status);
}
