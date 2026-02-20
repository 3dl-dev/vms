
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <va_rangedef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Purge the process's entire working set.
*/

static VA_RANGE inadr;
static int r0_status;

    inadr.va_range$ps_start_va = 0x00000000;
    inadr.va_range$ps_end_va = (void *)0x7FFFFFFF;
    r0_status = sys$purgws (&inadr);
    errchk_sig (r0_status);
    (void)printf ("Success\n");
}
