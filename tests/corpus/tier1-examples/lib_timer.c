
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

#include "errchk.h"

#define MEGA_LARGE_NUMBER 400000000

/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int context_one = 0;
static unsigned int context_two = 0;
static int code = 1;
static GENERIC_64 elapsed;
static int junk;
static int i;

static char buffer[255+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

    r0_status = lib$init_timer (&context_one);
    errchk_sig (r0_status);

    for (i = 0; i < MEGA_LARGE_NUMBER; i++) {
        if (i == MEGA_LARGE_NUMBER / 2) {
            r0_status = lib$init_timer (&context_two);
            errchk_sig (r0_status);
        }

        /*
        ** Waste some time.
        */
        junk %= 4;
        junk *= junk;
        junk++;
        junk++;
        junk--;
    }

    r0_status = lib$show_timer (&context_one);
    errchk_sig (r0_status);

    r0_status = lib$stat_timer (&code,
                                &elapsed.gen64$l_longword[0],
                                &context_two);
    errchk_sig (r0_status);

    r0_status = lib$sys_asctim (&buffer_d.dsc$w_length,
                                &buffer_d,
                                &elapsed,
                                0);
    errchk_sig (r0_status);

    (void)printf (" Elapsed: %-.*s\n",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);

    r0_status = lib$free_timer (&context_one);
    errchk_sig (r0_status);

    r0_status = lib$free_timer (&context_two);
    errchk_sig (r0_status);
}
