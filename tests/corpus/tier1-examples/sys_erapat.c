
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <iodef.h>
#include <dvidef.h>
#include <dcdef.h>
#include <descrip.h>
#include <devdef.h>
#include <efndef.h>
#include <eradef.h>
#include <mntdef.h>
#include <rms.h>
#include <dmtdef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

struct itm3 {
    unsigned short int length;
    unsigned short int function;
    void *buffer;
    void *retlen;
};


/******************************************************************************/
int main (void) {
/*
** Warning.  This code will dismount and erase data on the device that you
** enter to the "Device to erase: " prompt if it's a disk or tape.  All
** standard disclaimers apply.
**
** Please see http://www.eight-cubed.com/disclaimer.html, particularly the
** bit about holding the author blameless before running this code.
** By downloading this code, you explicitly agree to this license.
*/

static IOSB iosb;

static unsigned int mnt_flags[2] = { MNT$M_FOREIGN, 0 };
static int r0_status;
static unsigned int devchar;
static unsigned int devclass;
static unsigned int mountcnt;
static unsigned int dmt_flags = DMT$M_NOUNLOAD;
static unsigned int era_type;
static unsigned int era_count;
static unsigned int era_pat;
static unsigned int address = 0;

static ILE3 dviitms[] = { 4, DVI$_DEVCHAR, &devchar, NULL,
                          4, DVI$_DEVCLASS, &devclass, NULL,
                          4, DVI$_MOUNTCNT, &mountcnt, NULL,
                          0, 0, NULL, NULL };

static ILE3 mntitms[] = { 0, MNT$_DEVNAM, NULL, NULL,
                          8, MNT$_FLAGS, mnt_flags, NULL,
                          0, 0, NULL, NULL };

static char device[255+1];
static struct dsc$descriptor_s device_d = { sizeof (device) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            device };

static const $DESCRIPTOR (prompt1_d, "Device to erase: ");

static const $DESCRIPTOR (prompt2_d,
                   "Are you SURE you want to erase this device (Y/N) [N] ? ");

static char rusure[255+1];
static struct dsc$descriptor_s rusure_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            rusure };

static unsigned short int channel;

static char finished = FALSE;

    /*
    ** Get a device to erase from the operator.
    */
    do {
        device_d.dsc$w_length = sizeof (device) - 1;
        r0_status = lib$get_input (&device_d,
                                   &prompt1_d,
                                   &device_d.dsc$w_length);
        if (r0_status == RMS$_EOF) {
            exit (EXIT_SUCCESS);
        } else {
            errchk_sig (r0_status);
        }
    } while (device_d.dsc$w_length == 0);

    /*
    ** Get information about the device.
    */
    r0_status = sys$getdviw (EFN$C_ENF,
                             0,
                             &device_d,
                             dviitms,
                             &iosb,
                             0,
                             0,
                             0);
    if (r0_status == SS$_NOSUCHDEV) {
        (void)printf ("No such device %-.*s\n",
                      device_d.dsc$w_length,
                      device_d.dsc$a_pointer);
        exit (EXIT_SUCCESS);
    } else {
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$l_getxxi_status);
    }

    if (((devclass != DC$_DISK) &&
         (devclass != DC$_TAPE)) ||
        ((devchar & DEV$M_MNT) == 0)) {
        (void)printf ("Device %-.*s is not mounted or is not a disk or tape\n",
                      device_d.dsc$w_length,
                      device_d.dsc$a_pointer);
        exit (EXIT_SUCCESS);
    }

    if ((devchar & DEV$M_SWL) != 0) {
        (void)printf ("Device %-.*s is write locked\n",
                      device_d.dsc$w_length,
                      device_d.dsc$a_pointer);
        exit (EXIT_SUCCESS);
    }

    /*
    ** Confirm the operator really wants to erase this device.
    */
    while ((rusure[0] != 'Y') && (rusure[0] != 'N')) {
        rusure_d.dsc$w_length = sizeof (rusure) - 1;
        r0_status = lib$get_input (&rusure_d,
                                   &prompt2_d,
                                   &rusure_d.dsc$w_length);
        if (r0_status == RMS$_EOF) {
            exit (EXIT_SUCCESS);
        } else {
            errchk_sig (r0_status);
        }
        if (rusure_d.dsc$w_length == 0) {
            exit (EXIT_SUCCESS);
        }
    }
    if (rusure[0] == 'N') {
        exit (EXIT_SUCCESS);
    }
        
    /*
    ** Set up flags for the dismount and erapat calls.
    */
    if (devclass == DC$_DISK) {
        dmt_flags |= DMT$M_CLUSTER;
        era_type = ERA$K_DISK;
    } else {
        era_type = ERA$K_TAPE;
    }

    /*
    ** Dismount the device.  If it's a disk, ensure we dismount it cluster
    ** wide.  In either case, don't unload it.
    */
    r0_status = sys$dismou (&device_d,
                            dmt_flags);
    errchk_sig (r0_status);

    /*
    ** Set up the item list to remount the device.
    */
    assert (mntitms[0].ile3$w_code == MNT$_DEVNAM);
    mntitms[0].ile3$w_length = device_d.dsc$w_length;
    mntitms[0].ile3$ps_bufaddr = device_d.dsc$a_pointer;

    /*
    ** And mount it.
    */
    r0_status = sys$mount (mntitms);
    errchk_sig (r0_status);

    r0_status = sys$assign (&device_d,
                            &channel,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** What is coming up is on most VMS systems rather wasteful.  The
    ** call to sys$erapat will return zero in the era_pat variable unless
    ** you have installed a custom erase pattern generator (see the "System
    ** Security Services" chapter of the "OpenVMS Programming Concepts Manual"
    ** for instructions on installing a custom erase pattern generator.  On
    ** systems that don't have a generator installed, the sys$erapat routine
    ** will get called multiple times for no purpose.  However, as the idea
    ** of this code is to demonstrate a call to it, we will be wasteful...
    **
    ** Secondly, the call to sys$qiow is erasing a disk in 20 block chunks.
    ** This might or might not be what you want.  Also notice that P1 though
    ** P3 arguments for a tape erase are meaningless and are ignored for tape
    ** devices.  See the "OpenVMS I/O User's Reference Manual" for full
    ** details of the parameters being passed in P1 though P3 for the sys$qiow
    ** call.
    */
    era_count = 0;
    while (!finished) {
        r0_status = sys$erapat (era_type,
                                ++era_count,
                                &era_pat);
        if (r0_status == SS$_NOTRAN) {
            era_count = 0;
            continue;
        } else {
            errchk_sig (r0_status);
        }

        r0_status = sys$qiow (EFN$C_ENF,
                              channel,
                              IO$_WRITELBLK|IO$M_ERASE,
                              &iosb,
                              0,
                              0,
                              &era_pat,
                              (20*512),
                              address,
                              0,
                              0,
                              0);
        errchk_sig (r0_status);
        if (iosb.iosb$w_status == SS$_ILLBLKNUM) {
            finished = TRUE;
            continue;
        } else {
            errchk_sig (iosb.iosb$w_status);
        }
        if (devclass == DC$_TAPE) {
            /*
            ** We only need to issue one QIO for tapes.  The drive does the
            ** erase for us.
            */
            finished = TRUE;
            continue;
        }
        address += 20;
    }

    r0_status = sys$dassgn (channel);
    errchk_sig (r0_status);

    r0_status = sys$dismou (&device_d,
                            dmt_flags);
    errchk_sig (r0_status);
    (void)printf ("Device %-.*s successfully erased\n",
                  device_d.dsc$w_length,
                  device_d.dsc$a_pointer);
}
