
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

#define MSG_LEN 512


/******************************************************************************/
int main (void) {

static int r0_status;

static int max_msg = MSG_LEN;
static int buffers = MSG_LEN * 2;

static unsigned short int dev_chan;
static unsigned short int mbx_chan;

static $DESCRIPTOR (tt_d, "TT:");

    r0_status = lib$asn_wth_mbx (&tt_d,
                                 &max_msg,
                                 &buffers,
                                 &dev_chan,
                                 &mbx_chan);
    errchk_sig (r0_status);

    r0_status = sys$dassgn (mbx_chan);
    errchk_sig (r0_status);

    r0_status = sys$dassgn (dev_chan);
    errchk_sig (r0_status);

}
