
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#if __G_FLOAT == 0
#  error "Please compile with CC/FLOAT=G_FLOAT (which is the default on alpha)"
#endif


/******************************************************************************/
void display_time (GENERIC_64 *time) {

static int r0_status;
static char str[23+1];
static struct dsc$descriptor_s str_d = { 0,
                                         DSC$K_DTYPE_T,
                                         DSC$K_CLASS_S,
                                         str };

    str_d.dsc$w_length = sizeof (str) - 1;
    r0_status = sys$asctim (&str_d.dsc$w_length,
                            &str_d,
                            time,
                            0);
    errchk_sig (r0_status);
    (void)printf ("%-.*s\n",
                  str_d.dsc$w_length,
                  str_d.dsc$a_pointer);
}


/******************************************************************************/
int main (void) {

static GENERIC_64 time;
static int multiplier = 2;
static float divisor = 0.5;

static int r0_status;
static $DESCRIPTOR (time_d, "0 01:30:00.00");

    r0_status = sys$bintim (&time_d,
                            &time);
    errchk_sig (r0_status);

    r0_status = lib$mult_delta_time (&multiplier,
                                     &time.gen64$q_quadword);
    errchk_sig (r0_status);
    display_time (&time);

    r0_status = lib$multf_delta_time (&divisor,
                                      &time.gen64$q_quadword);
    errchk_sig (r0_status);
    display_time (&time);
}
