
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
** This looks strangely like lib$get_input.  However, if you define a 
** foreign command, you can type parameters after the program invokation,
** and have the program retrieve them.
**
** For example, if you compile this program to DISK:[DIR]LIB_GET_FOREIGN.EXE,
** you can say:
**
**    $ TEST :== $DISK:[DIR]LIB_GET_FOREIGN
**
** and then run the program like this:
**
**    $ TEST PARAM1 PARAM2
**
** which should produce:
**
**    Input was: "PARAM1 PARAM2"
**
** If you don't specify parameters, the program will just behave like
** lib_get_input.c
**
*/

static int r0_status;

static char input[255+1];
static struct dsc$descriptor_s input_d = { sizeof (input) - 1,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };

static const $DESCRIPTOR (prompt_d, "Enter your input: ");

    r0_status = lib$get_foreign (&input_d,
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
