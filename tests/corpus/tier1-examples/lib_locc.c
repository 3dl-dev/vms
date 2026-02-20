
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned int location;

static $DESCRIPTOR (string_d, "abcdefghijklmnopqrstuvwxyz");
static $DESCRIPTOR (search_d, "x");

    location = lib$locc (&search_d,
                         &string_d);

    (void)printf ("location = %u\n",
                  location);
}
