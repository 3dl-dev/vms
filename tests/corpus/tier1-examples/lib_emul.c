
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int multiplier = 4096;
static int multiplicand = 268435456;
static int addend = 0;
static __int64 product;

    r0_status = lib$emul (&multiplier,
                          &multiplicand,
                          &addend,
                          &product);
    errchk_sig (r0_status);

    (void)printf ("product = %%X%016Lx\n",
                  product);
}
