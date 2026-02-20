
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <va_rangedef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program requires the PSWAPM privilege.
*/

static unsigned int lock_me;

static int r0_status;

static VA_RANGE inadr;
static VA_RANGE retadr;
static VA_RANGE ulkadr;

    /*
    ** Lock the page containing the address of variable lock_me. 
    ** Note that results will be different on a vax/alpha/ipf.  The latter
    ** two architectures have 8K pages by default, while the vax has 
    ** pages of 512 bytes (known as "pagelets" on the other two architectures).
    */
    inadr.va_range$ps_start_va = inadr.va_range$ps_end_va =  (void *)&lock_me;

    r0_status = sys$lckpag (&inadr,
                            &retadr,
                            0);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need PSWAPM privilege to run this code\n");
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("Locked %d \"paglets\" from address %08x to %08x\n",
                  (((char *)retadr.va_range$ps_end_va -
		    (char *)retadr.va_range$ps_start_va) / 512) + 1,
                  retadr.va_range$ps_start_va,
                  retadr.va_range$ps_end_va);

    r0_status = sys$ulkpag (&retadr,
                            &ulkadr,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Unlocked %d \"paglets\" from address %08x to %08x\n",
                  (((char *)ulkadr.va_range$ps_end_va - 
		    (char *)ulkadr.va_range$ps_start_va) / 512) + 1,
                  ulkadr.va_range$ps_start_va,
                  ulkadr.va_range$ps_end_va);
}
