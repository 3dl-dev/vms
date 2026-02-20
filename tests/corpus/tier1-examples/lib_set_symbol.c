
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

static const int table_ind = LIB$K_CLI_GLOBAL_SYM;

static $DESCRIPTOR (symbol_d, "LIB_SET_SYMBOL_TEST");
static $DESCRIPTOR (value_d, "Every good boy deserves fruit");

    r0_status = lib$set_symbol (&symbol_d,
                                &value_d,
                                &table_ind);
    errchk_sig (r0_status);

    (void)printf ("Symbol defined.\n"
                  "Use SHOW SYMBOL/GLOBAL %-.*s to see it\n"
                  "and DELETE/SYMBOL/GLOBAL %-.*s to delete it.\n",
                  symbol_d.dsc$w_length,
                  symbol_d.dsc$a_pointer,
                  symbol_d.dsc$w_length,
                  symbol_d.dsc$a_pointer);
}                  
