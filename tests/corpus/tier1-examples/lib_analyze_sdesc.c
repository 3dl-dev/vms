
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"

#ifdef __VAX
#  error "64 bit integers required"
#endif


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned short int data_length;
static void *data_address;

static $DESCRIPTOR (descriptor_d, "test descriptor");

    r0_status = lib$analyze_sdesc ((unsigned __int64 *)&descriptor_d,
                                   &data_length,
                                   &data_address);
    errchk_sig (r0_status);

    (void)printf ("data_length = %u, data_address = %08x\n",
                  data_length,
                  data_address);
}
