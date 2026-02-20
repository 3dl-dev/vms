
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <libdtdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int date_length;
static unsigned int flags = LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS;

    r0_status = lib$get_maximum_date_length (&date_length,
                                             0,
                                             &flags);
    errchk_sig (r0_status);

    {
        char date_str[date_length];
	struct dsc$descriptor_s date_str_d = { date_length,
                                               DSC$K_DTYPE_T,
                                               DSC$K_CLASS_S,
                                               date_str };

        r0_status = lib$format_date_time (&date_str_d,
                                          0,
                                          0,
                                          &date_str_d.dsc$w_length,
                                          &flags);
        errchk_sig (r0_status);

        (void)printf ("%-.*s\n",
                      date_str_d.dsc$w_length,
                      date_str_d.dsc$a_pointer);
    }
}
