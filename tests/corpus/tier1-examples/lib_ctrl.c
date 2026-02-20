
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <iodef.h>
#include <libclidef.h>
#include <rms.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void get_input (void) {

static int r0_status;

static char input[255+1];
static struct dsc$descriptor_s input_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };
static const $DESCRIPTOR (prompt_d, "Type CTRL-T now.  CTRL-Z to exit: ");

    do {
        input_d.dsc$w_length = sizeof (input) - 1;
        r0_status = lib$get_input (&input_d,
                                   &prompt_d,
                                   &input_d.dsc$w_length);
        if (r0_status != RMS$_EOF) {
            errchk_sig (r0_status);
        }
    } while (r0_status != RMS$_EOF);
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int old_mask;
static unsigned int mask = LIB$M_CLI_CTRLT;

    r0_status = lib$enable_ctrl (&mask,
                                 &old_mask);
    errchk_sig (r0_status);

    (void)printf ("Typing CTRL-T should show basic process information.\n");
    get_input ();

    (void)printf ("Disabling CTRL-T...\n");
    r0_status = lib$disable_ctrl (&mask,
                                  0);
    errchk_sig (r0_status);

    (void)printf ("Typing CTRL-T now should produce nothing.\n");
    get_input ();

    (void)printf ("Restoring settings...\n");
    r0_status = lib$enable_ctrl (&old_mask,
                                 0);
    errchk_sig (r0_status);
}
