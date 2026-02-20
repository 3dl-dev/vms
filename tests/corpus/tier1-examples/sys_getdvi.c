
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <dvidef.h>
#include <descrip.h>
#include <iledef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void ast_routine (unsigned int efn) {

static int r0_status;

    r0_status = sys$setef (efn);
    errchk_sig (r0_status);
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int max_blocks;
static unsigned int free_blocks;
static unsigned int efn = 32;
static unsigned int ast_efn = 33;
static unsigned int mask;

static IOSB iosb;

static char device_name[64+1];
static struct dsc$descriptor_s device_name_d = { sizeof (device_name) - 1,
                                                 DSC$K_DTYPE_T,
                                                 DSC$K_CLASS_S,
                                                 device_name };

static ILE3 dviitms[] =
              { 64, DVI$_DEVNAM, device_name, &device_name_d.dsc$w_length, 
                 4, DVI$_FREEBLOCKS, &free_blocks, NULL,
                 4, DVI$_MAXBLOCK, &max_blocks, NULL,
                 0, 0, NULL, NULL };

static unsigned short int channel;
static $DESCRIPTOR (device_d, "SYS$SYSDEVICE");

    /*
    ** We are going to wait on two event flags just for fun.  Both of
    ** the event flags are assigned with lib$reserve_ef, which allocates
    ** specific event flags. Generally, this is not such a good idea.
    ** You should allocate event flags with lib$get_ef.
    */

    r0_status = lib$reserve_ef (&efn);
    errchk_sig (r0_status);

    r0_status = lib$reserve_ef (&ast_efn);
    errchk_sig (r0_status);

    r0_status = sys$clref (ast_efn);
    errchk_sig (r0_status);

    /*
    ** Construct a mask for the event flags.  Both these event flags are
    ** in local event flag cluster 1 which encompasses EFNs 32 through 63.
    ** So to get a bit for our mask, subtract 32.
    */
    mask = 1<<(efn - 32) | 1<<(ast_efn - 32);

    r0_status = sys$getdvi (efn,
                            0,
                            &device_d,
                            dviitms,
                            &iosb,
                            ast_routine,
                            ast_efn,
                            0);
    errchk_sig (r0_status);

    /*
    ** Wait for both efns to set.
    */
    r0_status = sys$wfland (efn,
                            mask);
    errchk_sig (r0_status);

    errchk_sig (iosb.iosb$l_getxxi_status);

    r0_status = lib$free_ef (&efn);
    errchk_sig (r0_status);

    r0_status = lib$free_ef (&ast_efn);
    errchk_sig (r0_status);

    (void)printf ("The system disk %-.*s has a total of "
                  "%u blocks with %u free\n",
                  device_name_d.dsc$w_length,
                  device_name_d.dsc$a_pointer,
                  max_blocks,
                  free_blocks);
}
