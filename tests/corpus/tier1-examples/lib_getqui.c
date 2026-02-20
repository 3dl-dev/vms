
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <quidef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;

static unsigned short int protection;
static unsigned __int64 *acc_tbl;

static char output[255+1];
static struct dsc$descriptor_s output_d = { sizeof (output) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };

static int function = QUI$_DISPLAY_QUEUE;
static int item = QUI$_PROTECTION;

static $DESCRIPTOR (queue_d, "SYS$BATCH");
static $DESCRIPTOR (object_d, "QUEUE");

    r0_status = lib$get_accnam (&object_d,
                                0,
                                (unsigned int *)&acc_tbl);
    errchk_sig (r0_status);

    r0_status = lib$getqui (&function,
                            &item,
                            0,
                            &queue_d,
                            0,
                            &protection,
                            0,
                            0);
    errchk_sig (r0_status);

    r0_status = lib$format_sogw_prot (&protection,
                                      acc_tbl,
                                      0,
                                      0,
                                      0,
                                      &output_d,
                                      &output_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("Protection for %-.*s is %-.*s\n",
                  queue_d.dsc$w_length,
                  queue_d.dsc$a_pointer,
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
