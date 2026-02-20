
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


/******************************************************************************/
static void routine (void) {

static int r0_status;

    r0_status = lib$ast_in_prog ();
    if (r0_status) {
        (void)printf ("routine executing as an AST\n");
    } else {
        (void)printf ("routine called normally\n");
    }
}


/******************************************************************************/
int main (void) {

static int r0_status;

    routine ();

    r0_status = sys$dclast (routine,
                            0,
                            0);
    errchk_sig (r0_status);
}
