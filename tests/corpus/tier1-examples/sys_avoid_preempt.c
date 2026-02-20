
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

#define DISABLE 0
#define ENABLE 1


/******************************************************************************/
int main (void) {

static int r0_status;

    r0_status = sys$setup_avoid_preempt (ENABLE);
    errchk_sig (r0_status);

    r0_status = sys$avoid_preempt (ENABLE);
    errchk_sig (r0_status);

    r0_status = sys$avoid_preempt (DISABLE);
    errchk_sig (r0_status);

    r0_status = sys$setup_avoid_preempt (DISABLE);
    errchk_sig (r0_status);
}
