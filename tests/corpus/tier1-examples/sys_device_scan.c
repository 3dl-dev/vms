
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <dcdef.h>
#include <dvsdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 context = { 0 };
static int r0_status;
static unsigned int devclass = DC$_TERM;

static char device[255+1];
static struct dsc$descriptor_s device_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            device };
static $DESCRIPTOR (wild_d, "*OPA*");

static ILE3 dvsitms[] = { 4, DVS$_DEVCLASS, &devclass, NULL,
                          0, 0, NULL, NULL };

static char finished = FALSE;

    while (!finished) {
        device_d.dsc$w_length = sizeof (device) - 1;
        r0_status = sys$device_scan (&device_d,
                                     &device_d.dsc$w_length,
                                     &wild_d,
                                     dvsitms,
                                     &context);
        switch (r0_status) {
            case SS$_NOMOREDEV :
                finished = TRUE;
                break;
            case SS$_NOSUCHDEV :
                finished = TRUE;
                (void)printf ("No devices matching search criteria!\n");
                exit (EXIT_SUCCESS);
                break;
            default :
                errchk_sig (r0_status);
                (void)printf ("%-.*s\n",
                              device_d.dsc$w_length,
                              device_d.dsc$a_pointer);
                break;
        }
    }
}
