
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static int pf_ast (unsigned int ticks) {

static unsigned int seconds;

    seconds = ticks / 100;
    if (seconds >= 1) {
	ticks -= seconds * 100;
    }
    (void)printf ("Power lost for %u.%02u seconds\n",
                  seconds,
                  ticks);
    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {

static int r0_status;

    r0_status = sys$setpra (pf_ast, 0);
    errchk_sig (r0_status);
}
