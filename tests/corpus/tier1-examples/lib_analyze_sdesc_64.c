
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
#  error "This is Alpha/IPF specific code"
#endif
 

/******************************************************************************/
int main (void) {

/*
** Save the pointer size and switch to 64 bit addresses.  Note that
** you must compile this code with CC/POINTER_SIZE=32 to enable the
** #pragma pointer_size functionality.
*/
#pragma environment save
#pragma pointer_size 64

static int r0_status;
static unsigned __int64 data_length;
static void *data_address;

static const $DESCRIPTOR64 (descriptor_d, "test descriptor");

    r0_status = lib$analyze_sdesc_64 ((unsigned __int64 *)&descriptor_d,
                                      &data_length,
                                      &data_address);
    errchk_sig (r0_status);

    (void)printf ("data_length = %Lu, data_address = %016x\n",
                  data_length,
                  data_address);

#pragma environment restore

}
