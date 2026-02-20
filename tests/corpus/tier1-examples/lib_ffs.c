
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"


/*******************************************************************/
int main (void) {

static int r0_status;

static int x = 32+16;
static int ffb;
static int pos = 0;
static int find;
static unsigned char siz = 32;

    /*
    ** 2's complement bit hack.
    */
    ffb = (x&-x);

    r0_status = lib$ffs (&pos,
                         &siz,
                         &x,
                         &find);
    errchk_sig (r0_status);
    (void)printf ("ffb = %d which should equal %d\n",
                  ffb,
                  (1<<find));
}
