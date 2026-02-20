
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program requires you to hold the BUGCHK privilege.  Please be aware
** that this code will write a user specific message to the system error log.
** On systems that are rigorously audited, this may be a Bad Thing.
*/

static int r0_status;

static $DESCRIPTOR (msg_d, "This is a test of the SYS$SNDERR routine");

    r0_status = sys$snderr (&msg_d);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need the BUGCHK privilege to run this code\n");
    } else {
        errchk_sig (r0_status);
    }
}
