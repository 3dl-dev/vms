
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static unsigned short int offset;

/*
** This is a dynamic string descriptor.  I highly recommend that unless you
** have a blindingly good reason for using one, you avoid them in C.  They
** are an easy way to shoot yourself in the foot, and are usually
** unnecessary anyway.
*/
static struct dsc$descriptor_d dynamic_d = { 0,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_D,
                                             NULL };

static $DESCRIPTOR (dummy_d, "test");

    r0_status = lib$sget1_dd (&dummy_d.dsc$w_length,
                              (unsigned __int64 *)&dynamic_d);
    errchk_sig (r0_status);

    (void)memcpy (dynamic_d.dsc$a_pointer,
                  dummy_d.dsc$a_pointer,
                  dummy_d.dsc$w_length);

    (void)printf ("Contents of dynamic_d = \"%-.*s\"\n",
                  dynamic_d.dsc$w_length,
                  dynamic_d.dsc$a_pointer);

    r0_status = lib$get_hostname (&dynamic_d,
                                  &dynamic_d.dsc$w_length,
                                  0);
    errchk_sig (r0_status);

    (void)printf ("Nodename is \"%-.*s\"\n",
                  dynamic_d.dsc$w_length,
                  dynamic_d.dsc$a_pointer);

    r0_status = lib$expand_nodename (&dynamic_d,
                                     &dynamic_d,
                                     &dynamic_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("Expanded nodename is \"%-.*s\"\n",
                  dynamic_d.dsc$w_length,
                  dynamic_d.dsc$a_pointer);

    r0_status = lib$get_fullname_offset (&dynamic_d,
                                         &offset);
    errchk_sig (r0_status);

    (void)printf ("Offset to full nodename is %u\n",
                  offset);

    r0_status = lib$sfree1_dd ((unsigned __int64 *)&dynamic_d);
    errchk_sig (r0_status);
}
