
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned short int channel;

static $DESCRIPTOR (device_d, "SYS$SYSDEVICE");

    /*
    ** Low level I/O on VMS requires a channel assigned to a device.
    ** Here we are just going to assign a channel to the system disk.
    ** There are multiple different I/O operations that can be performed
    ** on a channel.  For example, infomation about the device can be
    ** obtained directly from the device's driver; you can perform
    ** low level file access directly, bypassing RMS; you can perform
    ** logical or physical I/O, bypassing the entire file system; and
    ** so on.
    **
    ** There is actually an entire manual dedicated to describing
    ** the I/O you can do, called, unsurprisingly, the "I/O User's
    ** Reference Manual". 
    **
    ** Some of these activities require privilege, and some can be
    ** performed by anyone.
    **
    ** Of course, channels are not just for disks.  They are for
    ** any device on the system.
    **
    ** Here we will just assign a channel and deassign it to demo the
    ** calls.  There will be plenty of other examples that actually
    ** use the channel to do stuff.
    */

    r0_status = sys$assign (&device_d,
                            &channel,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Channel assigned to %-.*s\n",
                  device_d.dsc$w_length,
                  device_d.dsc$a_pointer);

    r0_status = sys$dassgn (channel);
    errchk_sig (r0_status);

    (void)printf ("Channel deassigned\n");
}
