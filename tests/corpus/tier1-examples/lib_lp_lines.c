
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned int lines;

    lines = lib$lp_lines ();

    (void)printf ("Lines = %u\n",
                  lines);
}
