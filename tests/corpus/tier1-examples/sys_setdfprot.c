
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void format_prot (unsigned short int prot) {

static int i;
static const char names[] = "SOGW";
static const char level[] = "RWED";

    for (i = 0; i < 16; i++) {
        if ((i % 4) == 0) {
            (void)printf ("%c:", names[(i / 4)]);
        }
        if ((prot & (1<<(i))) == 0) {
            (void)printf ("%c", level[i % 4]);
        }
        if (((i % 4) == 3) && (i != 15)) {
            (void)printf (", ");
        }
    }
    (void)printf ("\n");
}


/******************************************************************************/
int main (void) {

static int r0_status;

static unsigned short int old_prot;
static unsigned short int new_prot;

    r0_status = sys$setdfprot (0,
                               &old_prot);
    errchk_sig (r0_status);

    format_prot (old_prot);

    new_prot = old_prot;
    r0_status = sys$setdfprot (&new_prot,
                               &old_prot);
    errchk_sig (r0_status);
}    
