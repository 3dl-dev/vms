
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1
#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <va_rangedef.h>
#include <starlet.h>

#include "errchk.h"

#ifdef __VAX
#  error "Alpha or IA64 code only"
#endif /* __VAX */


/******************************************************************************/
int main (void) {

static VA_RANGE address = { 0 };

static unsigned __int64 count = { 0xffffffffffffffff };

static int r0_status;

    r0_status = sys$purge_ws (&address,
                              count);
    errchk_sig (r0_status);
    (void)printf ("Success\n");
}
