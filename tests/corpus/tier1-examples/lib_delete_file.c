
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
static void success_routine (struct dsc$descriptor_s *filespec_d) {

    (void)printf ("Deleted %-.*s\n",
                  filespec_d->dsc$w_length,
                  filespec_d->dsc$a_pointer);
}


/******************************************************************************/
int main (void) {

static int r0_status;
static FILE *fp;
static char filename[] = "lib_delete_file_test.txt";
static struct dsc$descriptor_s filename_d = { sizeof (filename) - 1,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              filename };

    /*
    ** Create a file so we can demo deleting it.
    */
    fp = fopen (filename, "w");
    if (fp == NULL) {
        (void)printf ("Could not create file!\n");
        exit (EXIT_FAILURE);
    }

    if (fclose (fp) != 0) {
        (void)printf ("Could not close file!\n");
        exit (EXIT_FAILURE);
    }

    r0_status = lib$delete_file (&filename_d,
                                 0,
                                 0,
                                 success_routine,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0);
    errchk_sig (r0_status);
}
