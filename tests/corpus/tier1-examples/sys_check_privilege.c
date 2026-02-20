
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <prvdef.h>
#include <nsadef.h>
#include <efndef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program requires you have the AUDIT privilege.
** Note that if you have security alarms or audits switched on for
** privilege success or failure, this program will generate one.
**
** Use $ SET PROC/PRIV=NETM and $ SET PROC/PRIV=NONETM to see results.
*/

static int r0_status;
static unsigned int audit_status;

static char image_name[] = "SYS_CHECK_PRIVILEGE";

static ILE3 auditms[] =
      { sizeof (image_name), NSA$_IMAGE_NAME, image_name, NULL,
        0, 0, NULL, NULL };

static PRVDEF req_privs;

    req_privs.prv$v_netmbx = TRUE;

    /*
    ** Not all privs have a SS$_NOxxx message.  Uncomment the following line
    ** to see the generic SS$_NOPRIV message returned.
    */
    /* req_privs.prv$v_pfnmap = TRUE; */

    r0_status = sys$check_privilegew (EFN$C_ENF,
                                      (GENERIC_64 *)&req_privs,
                                      0,
                                      0,
                                      &auditms,
                                      &audit_status,
                                      0,
                                      0);
    switch (r0_status) {
        case SS$_NOAUDIT :
            (void)printf ("This program requires the AUDIT privilege\n");
            break;
        case SS$_NONETMBX :
            (void)printf ("You do not have NETMBX in your current priv mask\n");
            break;
        case SS$_NOPRIV :
            /*
            ** You'll only get here if you play with changing the bits set
            ** in the req_privs structure.
            */
            (void)printf ("You don't have a priv in your current mask that "
                          "you are testing for\n");
            break;
        default :
            errchk_sig (r0_status);
            (void)printf ("You have NETMBX in your current priv mask\n");
            break;
    }
}
