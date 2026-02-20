
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>

/*
** This header includes item codes for getjpi calls.
*/
#include <jpidef.h>

#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static int item_code = JPI$_PRCNAM;

static char process_name[15+1];

static struct dsc$descriptor_s process_name_d = { sizeof (process_name) - 1,
                                                  DSC$K_DTYPE_T,
                                                  DSC$K_CLASS_S,
                                                  process_name };

    r0_status = lib$getjpi (&item_code,
                            0,
                            0,
                            0,
                            &process_name_d,
                            &process_name_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("This process is named \"%-.*s\"\n",
                  process_name_d.dsc$w_length,
                  process_name_d.dsc$a_pointer);
}
                            

