
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static unsigned long int index;
static $DESCRIPTOR (quote_d,
         "   When a thing is funny, search it carefully for a hidden truth");
static $DESCRIPTOR (string_d, " ");

    index = lib$skpc (&string_d,
                      &quote_d);

    (void)printf ("%-.*s\n",
                  quote_d.dsc$w_length,
                  quote_d.dsc$a_pointer);
    if (index == 0) {
        (void)printf ("String contains no spaces\n");
    } else {
        index--;
        for (int i = 0; i < index; i++) {
            (void)printf ("-");
        }
        (void)printf ("^    (Point to first non-space: index = %d)\n", index);
    }
}
