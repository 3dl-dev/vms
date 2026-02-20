
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

static unsigned int ui;
static int si;

static $DESCRIPTOR (binary_d, "11000000111001");
static $DESCRIPTOR (si_d, "-54321");
static $DESCRIPTOR (fortran_true_d, ".True.");
static $DESCRIPTOR (octal_d, "30071");
static $DESCRIPTOR (ui_d, "12345");
static $DESCRIPTOR (hex_d, "3039");

    r0_status = ots$cvt_tb_l (&binary_d,
                              &ui,
                              sizeof (ui),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%u\n",
                  ui);

    /*
    ** The cast is there because new starlet is incorrect.
    */
    r0_status = ots$cvt_ti_l (&si_d,
                              (unsigned int *)&si,
                              sizeof (si),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%d\n",
                  si);

    /*
    ** New starlet is a tramp.
    */
    r0_status = ots$cvt_tl_l (&fortran_true_d,
                              (unsigned int *)&si,
                              sizeof (si),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%d\n",
                  si);

    r0_status = ots$cvt_to_l (&octal_d,
                              &ui,
                              sizeof (ui),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%u\n",
                  ui);

    r0_status = ots$cvt_tu_l (&ui_d,
                              &ui,
                              sizeof (ui),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%u\n",
                  ui);

    r0_status = ots$cvt_tz_l (&hex_d,
                              &ui,
                              sizeof (ui),
                              0);
    errchk_sig (r0_status);

    (void)printf ("%u\n",
                  ui);

}
