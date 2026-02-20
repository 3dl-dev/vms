
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <rms.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** This code is very similar to lib_get_input.c.  lib$get_input () reads from
** SYS$INPUT.  lib$get_command reads from SYS$COMMAND
*/

static int r0_status;

static char input[255+1];

static struct dsc$descriptor_s input_d = { sizeof (input) - 1,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };

static const $DESCRIPTOR (prompt_d, "Enter your input: ");

    r0_status = lib$get_command (&input_d,
                                 &prompt_d,
                                 &input_d.dsc$w_length);
    if (r0_status == RMS$_EOF) {
        (void)printf ("End of file detected\n");
    } else {
	errchk_sig (r0_status);
        (void)printf ("Input was: \"%-.*s\"\n", 
                      input_d.dsc$w_length,
                      input_d.dsc$a_pointer);
    }
}
