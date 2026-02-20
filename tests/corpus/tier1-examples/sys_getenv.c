
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <assert.h>
#include <stenvdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifndef __ALPHA
#  error "This is Alpha specific code"
#endif /* __ALPHA */


/******************************************************************************/
int main (void) {
/*
** Because this code uses both 32 and 64 bit addresses, you must compile it
** with:
**
**      $ CC/POINTER_SIZE=32 SYS_GETENV.C
**
*/

static unsigned __int64 bootdef_dev_len;
static unsigned __int64 booted_dev_len;

static int r0_status;

static char booted_dev[255+1];
static char bootdef_dev[255+1];

static struct dsc$descriptor_s booted_dev_d = { 0,
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                booted_dev };

static struct dsc$descriptor_s bootdef_dev_d = { 0,
                                                 DSC$K_DTYPE_T,
                                                 DSC$K_CLASS_S,
                                                 bootdef_dev };

#pragma environment save
#pragma pointer_size 64
static struct {
    unsigned int function;
    unsigned int length;
    void *buffer;
    void *retlen;
} envitms[] = { STENV$K_BOOTED_DEV,
                sizeof (booted_dev) - 1,
                booted_dev,
                &booted_dev_len,
                STENV$K_BOOTDEF_DEV,
                sizeof (bootdef_dev) - 1,
                bootdef_dev,
                &bootdef_dev_len,
                0, 0, NULL, NULL };
#pragma environment restore

    r0_status = sys$getenv (envitms);
    errchk_sig (r0_status);

    assert (bootdef_dev_len < sizeof (bootdef_dev));
    bootdef_dev_d.dsc$w_length = (unsigned short int)bootdef_dev_len;

    assert (booted_dev_len < sizeof (booted_dev));
    booted_dev_d.dsc$w_length = (unsigned short int)booted_dev_len;

    (void)printf ("BOOTDEF_DEV: %-.*s\n",
                  bootdef_dev_d.dsc$w_length,
                  bootdef_dev_d.dsc$a_pointer);
    (void)printf (" BOOTED_DEV: %-.*s\n",
                  booted_dev_d.dsc$w_length,
                  booted_dev_d.dsc$a_pointer);
}
