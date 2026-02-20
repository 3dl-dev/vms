
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned long int old_bit_state;
static unsigned long bit_array[20];
static int bit_to_test;

    bit_array[4] = 4;
    bit_to_test = 32 * 4 + 2;
    old_bit_state = lib$bbcci (&bit_to_test,
                               bit_array);

    (void)printf ("Bit %u was %s\n",
                  bit_to_test,
                  old_bit_state ? "set" : "clear");
}
