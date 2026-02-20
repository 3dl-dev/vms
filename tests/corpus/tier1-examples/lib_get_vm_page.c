
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
static int pages;
static int address;

    pages = 4;
    r0_status = lib$get_vm_page (&pages,
                                 &address);
    errchk_sig (r0_status);

    (void)printf ("Got %d pages at address %08x\n",
                  pages,
                  address);

    r0_status = lib$free_vm_page (&pages,
                                  &address);
    errchk_sig (r0_status);

    (void)printf ("Freed %d pages at address %08x\n",
                  pages,
                  address);
}
