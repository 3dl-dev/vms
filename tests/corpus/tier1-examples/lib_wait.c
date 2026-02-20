
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <libwaitdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Demos lib$wait () which uses a floating point value to wait a number of
** seconds between 0 and 100000.  Thanks to John Covert for pointing out that
** the program needed to do better floating point format handling.  This
** code should now work on both Alpha and IA64 without modification.
*/

#if __G_FLOAT != 0
#  define FLOAT_TYPE LIB$K_VAX_F
#elif __D_FLOAT != 0
#  define FLOAT_TYPE LIB$K_VAX_D
#elif __IEEE_FLOAT != 0
#  define FLOAT_TYPE LIB$K_IEEE_S
#else
#  error "Try specifying a floating point qualifier on the compile"
#endif 

static int r0_status;

static float time = 2.5;
static unsigned int float_type = FLOAT_TYPE;


    (void)printf ("Sleeping for %f seconds... ",
                  time);
    (void)fflush (stdout);
    r0_status = lib$wait (&time,
                          0,
                          &float_type);
    errchk_sig (r0_status);

    (void)printf ("Awake!\n");
}
