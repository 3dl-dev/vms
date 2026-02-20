
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int prot_ena = 0xffff;
static unsigned int prot_val = 0xff00;
static unsigned int max_versions = 5;
static unsigned int init_alloc = 200;

static char dir_spec[256];
static char prev_dir[256];
static char file_spec[256]; 
static char created = TRUE;

static $DESCRIPTOR (dir_d, "LOG");
static struct dsc$descriptor_s dir_spec_d = { sizeof (dir_spec),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              dir_spec };
static struct dsc$descriptor_s prev_dir_d = { sizeof (prev_dir),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              prev_dir };


    dir_spec_d.dsc$w_length = sprintf (dir_spec,
                                       "[.%-.*s]",
                                       dir_d.dsc$w_length,
                                       dir_d.dsc$a_pointer);
    (void)sprintf (file_spec,
                   "%-.*s.DIR;1",
                   dir_d.dsc$w_length,
                   dir_d.dsc$a_pointer);

    (void)printf ("Creating directory %-.*s\n",
                  dir_spec_d.dsc$w_length,
                  dir_spec_d.dsc$a_pointer);

    r0_status = lib$create_dir (&dir_spec_d,
                                0,
                                &prot_ena,
                                &prot_val,
                                &max_versions,
                                0,
                                &init_alloc);
    switch (r0_status) {
        case SS$_CREATED :
            (void)printf ("Created.\n");
            break;
        case SS$_NORMAL :
            (void)printf ("Directory already existed.\n");
            created = FALSE;
            break;
        default :
            errchk_sig (r0_status);
            break;
    }

    (void)printf ("Set default to directory %-.*s\n",
                  dir_spec_d.dsc$w_length,
                  dir_spec_d.dsc$a_pointer);

    r0_status = sys$setddir (&dir_spec_d,
                             &prev_dir_d.dsc$w_length,
                             &prev_dir_d);
    errchk_sig (r0_status);

    (void)printf ("Set default to directory %-.*s\n",
                  prev_dir_d.dsc$w_length,
                  prev_dir_d.dsc$a_pointer);

    r0_status = sys$setddir (&prev_dir_d,
                             0,
                             0);
    errchk_sig (r0_status);

    if (created) {
        (void)printf ("Deleting %s\n",
                      file_spec);

        (void)remove (file_spec);
    }
}
