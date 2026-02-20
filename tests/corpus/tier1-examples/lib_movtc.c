
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <libdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"

extern char LIB$AB_UPCASE;


/******************************************************************************/
int main (void) {

static int r0_status;

static $DESCRIPTOR (string_d, "THIS is A test, ThIs Is OnLy A tEsT.");
static $DESCRIPTOR (fill_d, " ");

static char output[60];

static struct dsc$descriptor_s table_d = { 256,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           &LIB$AB_UPCASE };
static struct dsc$descriptor_s output_d = { sizeof (output),
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

    r0_status = lib$movtc (&string_d,
                           &fill_d,
                           &table_d,
                           &output_d);
    errchk_sig (r0_status);

    (void)printf ("\"%-.*s\"\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
