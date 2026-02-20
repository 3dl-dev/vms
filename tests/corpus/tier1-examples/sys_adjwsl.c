
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program attempts to set the working set limit up to the user's
** WSEXTENT.
*/

static int r0_status;
static unsigned int wsextent;
static unsigned int old_limit;

static IOSB iosb;

static ILE3 jpiitms[] = { 4, JPI$_WSEXTENT, &wsextent, NULL,
                          0, 0, NULL, NULL };

    /*
    ** Get the process's maximum allowed working set.
    */
    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Set it up there.
    */
    r0_status = sys$adjwsl (wsextent,
                            &old_limit);
    errchk_sig (r0_status);
}
