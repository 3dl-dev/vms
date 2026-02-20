
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

static $DESCRIPTOR (name_d, "sys_crembx_test");

    /*
    ** Mailboxes are software constructs that act like record oriented
    ** devices.  They are useful for interprocess communication.  You
    ** can create either permanent or temporary mailboxes.  This is an
    ** example of a temp one.  You need the TMPMBX privilege to call this
    ** (most accounts have this privilege).  To create a permanent mailbox,
    ** you need the PRMMBX privilege.  Temp mailboxes are automatically
    ** deleted on image rundown.
    **
    ** This demo does nothing other than create the mailbox.  Usually, you
    ** have a couple of cooperating processes...
    */
    r0_status = sys$crembx (0,
                            &channel,
                            512,            /* Max message size */
                            512*10,         /* Buffer space for messages */
                            prot,
                            PSL$C_USER,
                            &name_d,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Temporary mailbox created\n");
}
