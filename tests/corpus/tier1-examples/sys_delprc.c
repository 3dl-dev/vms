
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This code will delete the process that runs it.
*/

static IOSB iosb;

static int r0_status;
static unsigned int pid;

static char process_name[15+1];
static struct dsc$descriptor_s process_name_d = { sizeof (process_name) - 1,
                                                  DSC$K_DTYPE_T,
                                                  DSC$K_CLASS_S,
                                                  process_name };

static ILE3 jpiitms[] =
              {  4, JPI$_PID, &pid, NULL,
                15, JPI$_PRCNAM, process_name, &process_name_d.dsc$w_length,
                 0, 0, NULL, NULL };

    /*
    ** Get our process ID and process name.  Neither of these are needed
    ** to delete our own process, but of course you have to get one or
    ** the other to delete a process other than your own.
    */
    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    r0_status = sys$delprc (&pid,
                            &process_name_d);
    errchk_sig (r0_status);
}
