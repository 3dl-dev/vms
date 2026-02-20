
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <jpidef.h>
#include <brkdef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** Send a message to all processes on the cluster running under the current
** username.  This doesn't require OPER privilege, but to send to another
** username does.
*/

static int r0_status;
static int item_code = JPI$_USERNAME;

static int i;

static char username[12];
static struct dsc$descriptor_s username_d = { sizeof (username),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

static $DESCRIPTOR (message_d, "Demo of the $BRKTHRUW system service\r");

static IOSB iosb;
    
    r0_status = lib$getjpi (&item_code,
                            0,
                            0,
                            0,
                            &username_d,
                            &username_d.dsc$w_length);
    errchk_sig (r0_status);

    for (i = 0; i < username_d.dsc$w_length; i++) {
        if (username[i] == ' ') {
            username_d.dsc$w_length = i;
            break;
        }
    }

    r0_status = sys$brkthruw (EFN$C_ENF,
                              &message_d,
                              &username_d,
                              BRK$C_USERNAME,
                              &iosb,
                              0,
                              BRK$M_CLUSTER,
                              BRK$C_USER1,
                              10,
                              0,
                              0);
    if (r0_status == SS$_NOOPER) {
        (void)printf ("This code requires OPER privilege\n");
    } else {
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);
        (void)printf ("%d terminal%s notificed on the cluster\n",
                      iosb.iosb$w_bcnt,
                      iosb.iosb$w_bcnt > 1 ? "s" : "");
    }
}
