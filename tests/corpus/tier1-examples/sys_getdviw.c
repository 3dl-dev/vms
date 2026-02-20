
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <dvidef.h>
#include <descrip.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int max_blocks;
static unsigned int free_blocks;

static IOSB iosb;

static char device_name[64+1];
static struct dsc$descriptor_s device_name_d = { sizeof (device_name) - 1,
                                                 DSC$K_DTYPE_T,
                                                 DSC$K_CLASS_S,
                                                 device_name };

static ILE3 dviitms[] =
              { 64, DVI$_DEVNAM, device_name, &device_name_d.dsc$w_length, 
                 4, DVI$_FREEBLOCKS, &free_blocks, NULL,
                 4, DVI$_MAXBLOCK, &max_blocks, NULL,
                 0, 0, NULL, NULL };

static unsigned short int channel;
static $DESCRIPTOR (device_d, "SYS$SYSDEVICE");


    r0_status = sys$assign (&device_d,
                            &channel,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    r0_status = sys$getdviw (EFN$C_ENF,
                             channel,
                             0,
                             dviitms,
                             &iosb,
                             0,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    r0_status = sys$dassgn (channel);
    errchk_sig (r0_status);

    (void)printf ("The system disk %-.*s has a total of "
                  "%u blocks with %u free\n",
                  device_name_d.dsc$w_length,
                  device_name_d.dsc$a_pointer,
                  max_blocks,
                  free_blocks);
}
