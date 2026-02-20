
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

static int r0_status;
static unsigned int utc_time[4];

static $DESCRIPTOR (input_time_d, "29-FEB-2000 12:23:45.67");

static char output_time[23+1];
static struct dsc$descriptor_s output_time_d = { sizeof (output_time) - 1,
                                                 DSC$K_DTYPE_T,
                                                 DSC$K_CLASS_S,
                                                 output_time }; 
static unsigned short int time_vec[7];

    r0_status = sys$binutc (&input_time_d,
                            utc_time);
    errchk_sig (r0_status);

    r0_status = sys$numutc (time_vec,
                            utc_time);
    errchk_sig (r0_status);

    r0_status = sys$ascutc (&output_time_d.dsc$w_length,
                            &output_time_d,
                            utc_time,
                            0);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n\n"
                  "     year = %hu\n"
                  "    month = %hu\n"
                  "      day = %hu\n"
                  "     hour = %hu\n"
                  "   minute = %hu\n"
                  "   second = %hu\n"
                  "hundredth = %hu\n",
                  output_time_d.dsc$w_length,
                  output_time_d.dsc$a_pointer,
                  time_vec[0],
                  time_vec[1],
                  time_vec[2],
                  time_vec[3],
                  time_vec[4],
                  time_vec[5],
                  time_vec[6]);
}
