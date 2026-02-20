
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ints.h>
#include <assert.h>
#include <prdef.h>
#include <psldef.h>
#include <builtins.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifdef __VAX
#  error "Alpha/IPF specific code"
#endif /* __VAX */

extern uint16 SYS$GW_IJOBLIM;


/******************************************************************************/
static int krnl_routine (uint16 *new, uint16 *old) {
/*
** This routine will be executed in kernel mode at IPL 0.  Errors in kernel
** mode can do exciting things such as corrupting data and crashing the
** operating system.  See the disclaimer comment in the main routine before
** running this code.
**/

register int ps;

    /*
    ** Get the processor status
    */
    ps = __PAL_RD_PS();

    /*
    ** Verify read and write access to the our arguments from the previous mode
    */
    if ((__PAL_PROBER (new, 4, ps & PR$M_PS_PRVMOD) == 0) ||
        (__PAL_PROBEW (old, 4, ps & PR$M_PS_PRVMOD) == 0)) {
        return SS$_ACCVIO;
    }

    *old = SYS$GW_IJOBLIM;
    SYS$GW_IJOBLIM = *new;

    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {
/*
** This program will do the same thing as
**
**        $ SET LOGIN/INTERACTIVE=23
**
** You need CMKRNL privilege to run this program.
**
** You need to link this program against the system base image.
** To do this on an alpha or itanium, use
**
**        $ LINK/SYSEXE SYS_CMKRNL
**
** Standard disclaimers apply.  See http://www.eight-cubed.com/disclaimer.html
**
** By downloading this code, you explicitly agree to that license.
**
*/

static uint32 r0_status;

static uint16 new_count = 23;
static uint16 old_count;

#pragma member_alignment save
#pragma nomember_alignment
static struct {
    uint32 list_length;
    uint16 *new;
    uint16 *old;
#pragma member_alignment restore

} kargs = { 2, &new_count, &old_count };

    r0_status = sys$cmkrnl (krnl_routine,
                            (unsigned int *)&kargs);
    errchk_sig (r0_status);

    (void)printf ("Interactive limit changed from %u to %u\n",
                  old_count,
                  new_count);
}
