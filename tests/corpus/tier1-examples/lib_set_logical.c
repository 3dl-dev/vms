
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static $DESCRIPTOR (log_d, "LIB_SET_LOGICAL_TEST");
static $DESCRIPTOR (eqv_d, "the quick brown fox jumps over the lazy dog");
static $DESCRIPTOR (tbl_d, "LNM$JOB");

    r0_status = lib$set_logical (&log_d,
                                 &eqv_d,
                                 &tbl_d,
                                 0,
                                 0);
    errchk_sig (r0_status);

    (void)printf ("Logical defined.\n"
                  "Use SHOW LOGICAL %-.*s to see it\n"
                  "and DEASSIGN/JOB %-.*s to delete it.\n",
                  log_d.dsc$w_length,
                  log_d.dsc$a_pointer,
                  log_d.dsc$w_length,
                  log_d.dsc$a_pointer);
}
                  
