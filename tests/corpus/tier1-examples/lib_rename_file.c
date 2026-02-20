
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

#define OLD_NAME "lib_rename_file_old.txt"
#define NEW_NAME "lib_rename_file_new.txt"


/******************************************************************************/
static int success_routine (struct dsc$descriptor_s *old_d,
                            struct dsc$descriptor_s *new_d,
                            void *user_arg) {

    (void)printf ("%-.*s renamed to %-.*s\n",
                  old_d->dsc$w_length,
                  old_d->dsc$a_pointer,
                  new_d->dsc$w_length,
                  new_d->dsc$a_pointer);

    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {

static int r0_status;

static $DESCRIPTOR (old_name_d, OLD_NAME);
static $DESCRIPTOR (new_name_d, NEW_NAME);

static FILE *fp;

    /*
    ** Create a file.
    */
    fp = fopen (OLD_NAME, "w");
    if (fp == NULL) {
        (void)printf ("Couldn't create file!\n");
        exit (EXIT_FAILURE);
    } else {
        if (fclose (fp) != 0) {
            (void)printf ("Could not close file!\n");
            exit (EXIT_FAILURE);
        }
    }

    r0_status = lib$rename_file (&old_name_d,
                                 &new_name_d,
                                 0,
                                 0,
                                 0,
                                 success_routine);
    errchk_sig (r0_status);

    remove (NEW_NAME);
}
