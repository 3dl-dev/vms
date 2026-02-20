
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static struct dsc$descriptor_d date_d = { 0,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_D,
                                          NULL };

    r0_status = lib$get_date_format (&date_d, 0);
    switch (r0_status) {
        case LIB$_DEFFORUSE:
            (void)printf ("Default format used; unable to determine desired "
                          "format.\n");
            break;
        case LIB$_ENGLUSED:
            (void)printf ("English used; unable to determine or use desired "
                          "language.\n");
            break;
        default:
            errchk_sig (r0_status);
            printf ("%-.*s\n", date_d.dsc$w_length, date_d.dsc$a_pointer);
            break;
    }
    if (date_d.dsc$w_length != 0) {
        r0_status = lib$sfree1_dd ((unsigned __int64 *)&date_d);
        errchk_sig (r0_status);
    }
}
