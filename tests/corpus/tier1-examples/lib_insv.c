
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

static int source = 0x00000003;
static int target = 0;
static int bit_position = 5;
static unsigned char size = 2;

    lib$insv (&source,
              &bit_position,
              &size,
              &target);
    (void)printf ("target should now be 0x00000060: 0x%08x\n", target);
}
