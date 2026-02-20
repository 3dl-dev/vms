
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <prvdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static PRVDEF old_priv_mask;
static PRVDEF new_priv_mask;

static int r0_status;

    new_priv_mask.prv$v_sysprv = TRUE;

    /*
    ** This switches on the SYSPRV privilege.  Note we are doing it
    ** on a temporary basis, which means that when the image exits,
    ** the operating system will ensure it is switched back to what
    ** it was before we changed it.
    **
    ** You obviously need to be privileged to run this program.  You
    ** need SYSPRV or SETPRV.
    */
    r0_status = sys$setprv (1,
                            (GENERIC_64 *)&new_priv_mask,
                            0,
                            (GENERIC_64 *)&old_priv_mask);
    if (r0_status == SS$_NOTALLPRIV) {
        (void)printf ("You are not authorized to enable the "
                      "SYSPRV privilege\n");
    } else {
        errchk_sig (r0_status);

        if (old_priv_mask.prv$v_sysprv) {
            (void)printf ("SYSPRV was already enabled\n");
        } else {
            (void)printf ("SYSPRV is now enabled\n");
        }
        /*
        ** You would perform actions requiring the privilege here before
        ** program exit.
        */
    }
}
