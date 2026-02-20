
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static $DESCRIPTOR (input_d, "ABCDEFG");
static unsigned int ascii_code;

    ascii_code = lib$ichar (&input_d);

    (void)printf ("First character ascii code is %u\n",
                  ascii_code);
}
