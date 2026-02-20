
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <hwrpbdef.h>
#include <pal_services.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Demo SYS$RPCC_64().  Compile and link as follows:
**
** $ cc sys_rpcc_64+sys$library:sys$lib_c/library
** $ link/sysexe sys_rpcc_64
**
*/

#ifdef __VAX
#  error "Alpha/IPF specific code"
#endif /* __VAX */

static unsigned __int64 count1;
static unsigned __int64 count2;
static double result;

extern HWRPB *exe$gpq_hwrpb;

register int i = 1;
register int a = 20;

    /*
    ** How long to do two divides?  If I was doing this for real, I'd do it
    ** in assembler to avoid the call overhead of the system service and the
    ** loop processing...
    */
    count1 = sys$rpcc_64 ();
    for (; i < 2; i++) {
        a /= i;
    }
    count2 = sys$rpcc_64 ();
    
    /*
    ** Print out the average of the two...
    */
    result = (count2 - count1) * 1000000.0 /
              exe$gpq_hwrpb->hwrpb$iq_cycle_count_freq / 2.0;

    (void)printf ("%f microseconds\n", result);
}
