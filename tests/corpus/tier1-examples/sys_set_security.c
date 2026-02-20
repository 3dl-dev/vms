
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ossdef.h>
#include <descrip.h>
#include <string.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static FILE *fp;

static struct {
    unsigned short int length;
    unsigned short int code;
    void *buffer;
    void *retlen;
} ssitms[] = { 0, OSS$_ACL_ADD_ENTRY, NULL, NULL,
               0, 0, NULL, NULL };

static int r0_status;
static unsigned int context;

static char file[] = "SYS$SCRATCH:SYS_SET_SECURITY.DEMO";
static char binace[256];

static struct dsc$descriptor_s file_d = { sizeof (file) - 1,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          file };
static struct dsc$descriptor_s ace_d = { 0,
                                         DSC$K_DTYPE_T,
                                         DSC$K_CLASS_S,
                                         NULL };
static struct dsc$descriptor_s binace_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            binace };

static char *aces[] = { "(identifier=[system],access=r+w+e+d)",
                        "(identifier=*, access=none)",
                        NULL };

static $DESCRIPTOR (class_d, "FILE");

    fp = fopen (file, "w");
    if (fp == NULL) {
        (void)printf ("Can't open file %s!\n", file);
        exit (EXIT_FAILURE);
    }

    if (fclose (fp) != 0) {
        (void)printf ("Could not close file!\n");
        exit (EXIT_FAILURE);
    }

    context = 0;
    for (int i = 0; ; i++) {
        if (aces[i] == NULL) {
            /*
            ** After the last ACE is written to the file, release the context,
            ** which updates the master copy of the security profile.
            */
            r0_status = sys$set_security (0,
                                          0,
                                          0,
                                          OSS$M_RELCTX,
                                          0,
                                          &context,
                                          0);
            errchk_sig (r0_status);
            break;
        }

        /*
        ** Translate the ACE into binary.
        */
        ace_d.dsc$w_length = strlen (aces[i]);
        ace_d.dsc$a_pointer = aces[i];
        binace_d.dsc$w_length = sizeof (binace);

        r0_status = sys$parse_acl (&ace_d,
                                   &binace_d,
                                   0,
                                   0,
                                   0);
        errchk_sig (r0_status);

        /*
        ** Write the ACE to the local copy of the security profile.
        */
        ssitms[0].length = binace[0];
        ssitms[0].buffer = binace;
        r0_status = sys$set_security (&class_d,
                                      &file_d,
                                      0,
                                      OSS$M_LOCAL,
                                      ssitms,
                                      &context,
                                      0);
        errchk_sig (r0_status);
    }
    remove (file);
}
