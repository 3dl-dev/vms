
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <iodef.h>
#include <iosbdef.h>
#include <efndef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void read_ast (IOSB *iosb) {

    (void)printf ("Read completion from mailbox with %u bytes read\n",
                  iosb->iosb$w_bcnt);
}


/******************************************************************************/
int main (void) {
/*
** Demonstrate the SYS$CANCEL () system service.  To do this, we need
** outstanding I/O to actually cancel.  So we will create a mailbox and
** queue an asynchronous read on it.  As there is no cooperating process, the
** read will never complete and we can cancel it...
*/

static IOSB iosb;
static int r0_status;
static unsigned short int channel;
static char buffer[512];

    r0_status = sys$crembx (0,
                            &channel,
                            sizeof (buffer),
                            0,
                            0,
                            0,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    r0_status = sys$qio (EFN$C_ENF,
                         channel,
                         IO$_READVBLK,
                         &iosb,
                         read_ast,
                         (__int64)&iosb,
                         buffer,
                         sizeof (buffer),
                         0,
                         0,
                         0,
                         0);
    errchk_sig (r0_status);

    r0_status = sys$cancel (channel);
    errchk_sig (r0_status);

    (void)printf ("Status in the iosb should be SS$_ABORT (0x%X): 0x%X\n",
                  SS$_ABORT,
                  iosb.iosb$w_status);

    r0_status = sys$dassgn (channel);
    errchk_sig (r0_status);
}
