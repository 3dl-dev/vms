
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <syidef.h>
#include <descrip.h>
#include <efndef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb1;
static IOSB iosb2;

static int r0_status;
static unsigned int cpus;
static unsigned int efn;

static char name[255+1];
static struct dsc$descriptor_s name_d = { sizeof (name) - 1,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          name };

static ILE3 dviitms1[] = { 4, SYI$_ACTIVECPU_CNT, &cpus, NULL,
                           0, 0, NULL, NULL };

static ILE3 dviitms2[] = { sizeof (name) - 1,
                           SYI$_HW_NAME,
                           name,
                           &name_d.dsc$w_length,
                           0, 0, NULL, NULL };


    /*
    ** Get an event flag for the asynch call to sys$getsyi.
    */
    r0_status = lib$get_ef (&efn);
    errchk_sig (r0_status);

    /*
    ** This is an asynch call.  The code continues immediately without waiting
    ** for the operating system to return the value requested.
    */
    r0_status = sys$getsyi (efn,
                            0,
                            0,
                            &dviitms1,
                            &iosb1,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Perform a synchronous call this time.
    */
    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             &dviitms2,
                             &iosb2,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb2.iosb$l_getxxi_status);

    /*
    ** Check that the asynch call really completed.
    */
    r0_status = sys$synch (efn,
                           &iosb1);
    errchk_sig (iosb1.iosb$l_getxxi_status);

    /*
    ** Free the event flag.
    */
    r0_status = lib$free_ef (&efn);
    errchk_sig (r0_status);

    (void)printf ("This system is a %-.*s containing %u %s\n",
                  name_d.dsc$w_length,
                  name_d.dsc$a_pointer,
                  cpus,
                  cpus == 1 ? "CPU" : "CPUs");
}
