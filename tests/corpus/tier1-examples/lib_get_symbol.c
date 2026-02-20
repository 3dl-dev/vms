
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <libclidef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int table;
static char buffer[255+1];
static struct dsc$descriptor_s buffer_d = { sizeof (buffer) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            buffer };

static $DESCRIPTOR (symbol_d, "$STATUS");


    r0_status = lib$get_symbol (&symbol_d,
                                &buffer_d,
                                &buffer_d.dsc$w_length,
                                &table);
    errchk_sig (r0_status);

    (void)printf ("Symbol \"%-.*s\" %s \"%-.*s\"\n",
                  symbol_d.dsc$w_length,
                  symbol_d.dsc$a_pointer,
                  (table == LIB$K_CLI_LOCAL_SYM) ? "=" : "==",
                  buffer_d.dsc$w_length,
                  buffer_d.dsc$a_pointer);
}
