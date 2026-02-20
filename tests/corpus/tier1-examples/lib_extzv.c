
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned long int array[2] = { 0x00000000,
                                      0x00000f00 };

static int result;
static unsigned char size = 3;
static int pos = 39;

    result = lib$extzv (&pos,
                        &size,
                        array);
    printf ("result = %d\n",
            result);
}
