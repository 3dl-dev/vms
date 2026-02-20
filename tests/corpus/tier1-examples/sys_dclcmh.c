
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <c_asm.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static int usermode_handler (int i) {

    (void)printf ("In user mode handler with code %ld\n", i);
    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {
/*
** This demos the SYS$DCLCMH system service.  Note to dispatch to the handler,
** the code needs to execute the CHMU instruction.  This is accomplished
** with in-lined assembler specific to the Alpha architecure.  For more
** information on in-line asm, see the "HP C Users's Guide for OpenVMS
** Systems".
*/

#ifndef __ALPHA
#  error "This is Alpha specific code"
#endif

static int r0_status;

static void *prev_handler;

    (void)printf ("Establishing a CHMU handler...\n");
    r0_status = sys$dclcmh (usermode_handler,
                            &prev_handler,
                            0);
    errchk_sig (r0_status);


    (void)printf ("Dispatching to handler...\n");
    (void)asm ("MOV 1, %a0;"
               "CHMU;");

    (void)printf ("Resuming after dispatch...\n");

    (void)printf ("Disabling our CHMU handler...\n");
    r0_status = sys$dclcmh (0,
                            &prev_handler,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Dispatching to handler will now crash the image with a "
                  "%%SYSTEM-F-CMODUSER error.\nThis is expected...\n");
    (void)asm ("MOV 1, %a0;"
               "CHMU;");
}
