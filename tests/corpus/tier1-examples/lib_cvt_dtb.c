
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static const $DESCRIPTOR (input_d, "-9847");
static int result;

    r0_status = lib$cvt_dtb (input_d.dsc$w_length,
                             input_d.dsc$a_pointer,
                             &result);
    errchk_sig (r0_status);

    (void)printf ("result = %d\n",
                  result);
}
