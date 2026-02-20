
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

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static GENERIC_64 binary_date;

static $DESCRIPTOR (date_d, "TOMORROW");

    /*
    ** Note that there are 4 defaulted variables here.  You can use
    ** these fields to supply default values, so this call is extremely
    ** useful for accepting date and time information from user input.
    */
    r0_status = lib$convert_date_string (&date_d,
                                         &binary_date.gen64$q_quadword,
                                         0,
                                         0,
                                         0,
                                         0);
    errchk_sig (r0_status);
}
