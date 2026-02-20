
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <descrip.h>
#include <ots$routines.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static unsigned int ui = 12345;
static int si = -54321;

static char output[40];
static struct dsc$descriptor_s output_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_tb (&ui,
                              &output_d,
                              0,
                              sizeof (ui));
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);


    /*
    ** This call is a good example of just how wrong the new starlet stuff can
    ** be.  The documentation (and the entire purpose of the call) is to
    ** convert a signed integer to text.  But the API expects a pointer to an
    ** unsigned integer.  D'oh!
    */
    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_ti ((unsigned int *)&si,
                              &output_d,
                              0,
                              sizeof (si),
                              1);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_tl (&si,
                              &output_d);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_to (&ui,
                              &output_d,
                              0,
                              sizeof (ui));
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_tu (&ui,
                              &output_d,
                              0,
                              sizeof (ui));
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    output_d.dsc$w_length = sizeof (output);
    r0_status = ots$cvt_l_tz (&ui,
                              &output_d,
                              0,
                              sizeof (ui));
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

}
