
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <rms.h>
#include <dvidef.h>
#include <jpidef.h>
#include <efndef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <dcdef.h>
#include <devdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** To use this program, you must mount a tape write-enabled on an allocated
** tape drive.
**
** Obviously, this program can be modified to initialize a disk drive too,
** however, I suspect that if you shoot yourself in the foot by initializing
** the wrong tape it will be much less pain than if you'd initialized the
** wrong disk.  Please be careful...
*/

static IOSB iosb;

static int r0_status;
static unsigned int devchar;
static unsigned int devclass;
static unsigned int dev_pid;
static unsigned int our_pid;

static ILE3 dviitms[] = { 4, DVI$_DEVCHAR, &devchar, NULL,
                          4, DVI$_DEVCLASS, &devclass, NULL,
                          4, DVI$_PID, &dev_pid, NULL,
                          0, 0, NULL, NULL };
static ILE3 jpiitms[] = { 4, JPI$_PID, &our_pid, NULL,
                          0, 0, NULL, NULL };

static char tape[64];
static struct dsc$descriptor_s tape_d = { sizeof (tape),
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          tape };

static $DESCRIPTOR (prompt_d, "Enter tape drive to init: ");
static $DESCRIPTOR (volnam_d, "label");

    (void)printf ("This program allows you to initialize a tape mounted in a\n"
                  "tape drive.  WARNING: This will (effectively) erase all\n"
                  "data on the tape.  You have been warned...\n");

    r0_status = lib$get_input (&tape_d,
                               &prompt_d,
                               &tape_d.dsc$w_length);
    if (r0_status == RMS$_EOF) {
        exit (EXIT_SUCCESS);
    } else {
        errchk_sig (r0_status);
    }

    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    r0_status = sys$getdviw (EFN$C_ENF,
                             0,
                             &tape_d,
                             dviitms,
                             &iosb,
                             0,
                             0,
                             0);
    switch (r0_status) {
        case SS$_NOSUCHDEV:
        case SS$_IVDEVNAM:
        case SS$_IVLOGNAM:
            exit (r0_status);
            break;
        default:
            errchk_sig (r0_status);
            errchk_sig (iosb.iosb$l_getxxi_status);
            break;
    }

    if (devclass != DC$_TAPE) {
        (void)printf ("%-.*s is not a tape device!\n",
                      tape_d.dsc$w_length,
                      tape_d.dsc$a_pointer);
        exit (EXIT_FAILURE);
    }

    if ((devchar & DEV$M_AVL) == 0) {
        (void)printf ("Device is unavailable!\n");
        exit (EXIT_FAILURE);
    }

    if (((devchar & DEV$M_MNT) != 0) ||
        ((devchar & DEV$M_FOR) != 0)) {
        (void)printf ("Device is already mounted!\n");
        exit (EXIT_FAILURE);
    }

    if ((devchar & DEV$M_SWL) != 0) {
        (void)printf ("Device is write locked!\n");
        exit (EXIT_FAILURE);
    }

    if (dev_pid != our_pid) {
        (void)printf ("The tape drive is not allocated to you.  Issue an\n\n"
                      "\t$ allocate %-.*s\n\ncommand and run this program "
                      "again.\n",
                      tape_d.dsc$w_length,
                      tape_d.dsc$a_pointer);
    }

    r0_status = sys$init_vol (&tape_d,
                              &volnam_d,
                              0);
    if (r0_status == SS$_NOPRIV) {
        exit (r0_status);
    } else {
        errchk_sig (r0_status);
        (void)printf ("Tape on drive %-.*s successfully initialized with label "
                      "\"%-.*s\"\n",
                      tape_d.dsc$w_length,
                      tape_d.dsc$a_pointer,
                      volnam_d.dsc$w_length,
                      volnam_d.dsc$a_pointer);
    }
}    
