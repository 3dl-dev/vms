
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


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int code = SS$_ABORT;
static unsigned int flags = 0x000f;
static char msg[255+1];
static struct dsc$descriptor_s msg_d = { sizeof (msg) - 1,
                                         DSC$K_DTYPE_T,
                                         DSC$K_CLASS_S,
                                         msg };

    r0_status = lib$sys_getmsg (&code,
                                &msg_d.dsc$w_length,
                                &msg_d,
                                &flags);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  msg_d.dsc$w_length,
                  msg_d.dsc$a_pointer);
}
                                
