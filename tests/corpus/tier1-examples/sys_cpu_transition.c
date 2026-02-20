
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <cstdef.h>
#include <iosbdef.h>
#include <efndef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifdef __VAX
#  error "Alpha/IA64 only"
#endif /* __VAX */


/******************************************************************************/
int main (void) {
/*
** This program starts any stopped CPUs and brings them into the active set of
** this instance.  You should understand the implications of doing that before
** you run this program.  You need CMKRNL to run this code.
*/

static IOSB iosb;
static int r0_status;

    r0_status = sys$cpu_transitionw (CST$K_CPU_START,
                                     CST$K_ANY_STOPPED_CPU,
                                     0,
                                     0,
                                     0,
                                     EFN$C_ENF,
                                     &iosb,
                                     0,
                                     0,
                                     0);
    switch (r0_status) {
        case SS$_NOPRIV :
            (void)fprintf (stderr, "You need CMKRNL priv to run this!\n");
            break;
        case SS$_NOSUCHCPU :
            (void)fprintf (stderr,
                           "System is not a multiprocessor or multiprocessing "
                           "is disabled!\n");
            break;
        default :
            errchk_sig (r0_status);
            errchk_sig (iosb.iosb$w_status);
            break;
    }
}
