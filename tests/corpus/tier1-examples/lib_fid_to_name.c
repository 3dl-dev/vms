
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <fiddef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static char buffer[255+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };
static $DESCRIPTOR (device_d, "SYS$SYSDEVICE:");
static FIDDEF fid;

    /*
    ** Load the fid with a file id that we know will be there.  (1,1,0)
    ** is defined to be INDEXF.SYS in the MFD for a disk.
    */
    fid.fid$w_num = 1;
    fid.fid$w_seq = 1;
    fid.fid$w_rvn = 0;

    r0_status = lib$fid_to_name (&device_d,
                                 &fid.fid$w_num,
                                 &buffer_d,
                                 &buffer_d.dsc$w_length,
                                 0,
                                 0);
    errchk_sig (r0_status);

    (void)printf ("File (%u,%u,%u) = %-.*s\n",
                  fid.fid$w_num,
                  fid.fid$w_seq,
                  fid.fid$w_rvn,
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
