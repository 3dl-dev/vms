
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <psldef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int prot = 0x0000ff00;    /* Allow only system and owner */

static unsigned short int channel;

static $DESCRIPTOR (name_d, "sys_delmbx_test");

    /*
    ** Create a permanent mailbox.  You need PRMMBX privilege to do this.
    ** Perm mailboxes persist over image rundown, so if the mailbox already
    ** exists, the call just assigns a channel to the device.
    */
    r0_status = sys$crembx (1,
                            &channel,
                            512,            /* Max message size */
                            512*10,         /* Buffer space for messages */
                            prot,
                            PSL$C_USER,
                            &name_d,
                            0,
                            0);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need the PRMMBX privilege to create a "
                      "permanent mailbox\n");
    } else {
        errchk_sig (r0_status);

        (void)printf ("Permanent mailbox created\n");

        /*
        ** Mark the mailbox for delete.  Server processes often do this.  The
        ** client program then also creates the mailbox (which just assigns
        ** a channel as the server already created it).  The server marks the
        ** mailbox for delete, so that when the last channel is deassigned from
        ** the device, the mailbox actually deletes.
        */
        r0_status = sys$delmbx (channel);
        errchk_sig (r0_status);

        /*
        ** Deassign the last channel from the mailbox.
        */
        r0_status = sys$dassgn (channel);
        errchk_sig (r0_status);
    }
}
