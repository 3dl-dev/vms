
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <rms.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** This program demonstrates lib$find_file () and ** finding the first three files in SYS$SYSTEM: matching ANALY*.*
*/

static int r0_status;
static int rms_status;
static unsigned int context = 0;
static int count = 0;

/*
** Use a variable string descriptor to be tricky.  You could also use
** a dsc$descriptor_s, but you'd have to manually adjust the length of the
** resultant descriptor.
*/
static struct {
    unsigned short int length;
    char file[NAM$C_MAXRSS+1];
} vs;
static struct dsc$descriptor_vs file_d = { 0,
                                           DSC$K_DTYPE_VT,
                                           DSC$K_CLASS_VS,
                                           (char *)&vs };

static $DESCRIPTOR (wildcard_d, "ANALYZ*.*");
static $DESCRIPTOR (default_d, "SYS$SYSTEM:");

    while (count < 3) {
        file_d.dsc$w_maxstrlen = NAM$C_MAXRSS;
        r0_status = lib$find_file (&wildcard_d,
                                   &file_d,
                                   &context,
                                   &default_d,
                                   0,
                                   &rms_status,
                                   0);
        if (!$VMS_STATUS_SUCCESS (r0_status)) {
            (void)lib$signal (r0_status,
                              1,
                              rms_status);
        }

	(void)printf ("%-.*s\n",
                      vs.length,
                      vs.file);
        count++;
    }

    r0_status = lib$find_file_end (&context);
    errchk_sig (r0_status);
}
