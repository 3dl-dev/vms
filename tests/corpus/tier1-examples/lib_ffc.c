
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

#define MAX_SIZE 31

static int r0_status;
static unsigned int array;
static int result;
static unsigned char size = MAX_SIZE;
static int pos;
static int i;
static char finished = FALSE;

    /*
    ** Each bit in our array will respresent an integer number, from 0 to
    ** 31.  Setting a bit will indicate that that number is not a prime
    ** number.  Initialize the array to all zero and mark off the "special"
    ** primes 1 and 2.
    */
    array = 0x00000006;
    pos = 2;

    do {
        for (i = pos; i < MAX_SIZE; i += pos) {
            array |= 1<<i;
        }
        if (MAX_SIZE - pos > 0) {
            size = MAX_SIZE - pos;
            r0_status = lib$ffc (&pos,
                                 &size,
                                 &array,
                                 &result);
            if (r0_status == LIB$_NOTFOU) {
                finished = TRUE;
            } else {
                errchk_sig (r0_status);
            }
            (void)printf ("%d is prime\n", result);
            pos = result;
        } else {
            finished = TRUE;
        }
    } while (!finished);
}
