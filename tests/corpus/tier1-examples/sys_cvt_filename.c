
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <cvtfnmdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

#ifdef __VAX
#  error "Code is Alpha/IPF specific"
#endif /* __VAX */

static int r0_status;
static unsigned int outflags;
static $DESCRIPTOR (infile_d, ".file.with.lots.of.dots.txt;");
static char buffer[512];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

    r0_status = sys$cvt_filename (CVTFNM$C_ACPQIO_TO_RMS,
                                  &infile_d,
                                  0,
                                  &buffer_d,
                                  &buffer_d.dsc$w_length,
                                  &outflags);
    errchk_sig (r0_status);

    (void)printf ("RMS version of the filename is %-.*s\n",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
