
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <uaidef.h>
#include <descrip.h>
#include <iledef.h>
#include <efndef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void change_account (struct dsc$descriptor_s *username_d,
                            unsigned int flags) {

static int r0_status;
static ILE3 uaiitms[] = { 4, UAI$_FLAGS, NULL, NULL,
                          0, 0, NULL, NULL };

    (void)printf ("%sabling %-.*s\n",
                  flags == 0 ? "En" : "Dis",
                  username_d->dsc$w_length,
                  username_d->dsc$a_pointer);

    uaiitms[0].ile3$ps_bufaddr = &flags;

    r0_status = sys$setuai (EFN$C_ENF,
                            0,
                            username_d,
                            uaiitms,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int flags;

static char dev[1+31];
static char dir[1+63];

static ILE3 uaiitms[] = { 63, UAI$_DEFDIR, dir, NULL,
                          31, UAI$_DEFDEV, dev, NULL,
                          4, UAI$_FLAGS, &flags, NULL,
                          0, 0, NULL, NULL };

static $DESCRIPTOR (username_d, "DEFAULT");

    /*
    ** Get default device, directory, and flags for username "DEFAULT".  You
    ** need SYSPRV, BYPASS, or GRPPRV privilege for this, depending on what your
    ** UIC is.
    */
    r0_status = sys$getuai (0,
                            0,
                            &username_d,
                            uaiitms,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Return values for device and directory are counted ascii strings.
    */
    (void)printf ("User %-.*s:%-.*s%-.*s\n",
                  username_d.dsc$w_length,
                  username_d.dsc$a_pointer,
                  dev[0],
                  &dev[1],
                  dir[0],
                  &dir[1]);

    if ((flags & UAI$M_DISACNT) == 0) {
        change_account (&username_d, UAI$M_DISACNT);
        change_account (&username_d, 0);
    } else {
        change_account (&username_d, 0);
        change_account (&username_d, UAI$M_DISACNT);
    }
}
