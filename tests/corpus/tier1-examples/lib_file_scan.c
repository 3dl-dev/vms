
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <rms.h>
#include <string.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
static int success_routine (struct FAB *fab) {
/*
** Print out the name of the file.
*/

static struct NAM *nam;

    nam = fab->fab$l_nam;

    (void)printf ("%-.*s\n",
                  nam->nam$b_rsl,
                  nam->nam$l_rsa);

    return SS$_NORMAL;
}


/******************************************************************************/
static int error_routine (struct FAB *fab) {
/*
** Print out the name of the file that caused an error and signal the
** error.
*/

static struct NAM *nam;

    nam = fab->fab$l_nam;

    (void)fprintf (stderr,
                   "Error encountered for file %-.*s\n",
                   nam->nam$b_rsl,
                   nam->nam$l_rsa);
    (void)lib$signal (fab->fab$l_stv);

    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int context = 0;

static char wild[] = "SYS$SYSTEM:ANALYZR*.EXE;";

static char rss[NAM$C_MAXRSS];
static char ess[NAM$C_MAXRSS];

static char finished = FALSE;

static struct FAB fab;
static struct NAM nam;

    /*
    ** Initialize the File Access Block.
    */
    fab = cc$rms_fab;
    fab.fab$l_fna = wild;
    fab.fab$b_fns = strlen (wild);
    fab.fab$l_nam = &nam;

    /*
    ** Initialize the NAMe block.
    */
    nam = cc$rms_nam;
    nam.nam$l_rsa = rss;
    nam.nam$b_rss = NAM$C_MAXRSS;
    nam.nam$l_rsa = rss;
    nam.nam$b_ess = NAM$C_MAXRSS;

    while (!finished) {
        /*
        ** The horrible casts on the second and third parameters
        ** are there because new starlet defines them as
        ** pointer to function with no parameters returning int.
        ** This is patently false, as the documentation and the
        ** running code demonstrate.
        */
        r0_status = lib$file_scan ((unsigned int *)&fab,
                                   (int (*)())&success_routine,
                                   (int (*)())&error_routine,
                                   &context);
        if ((r0_status == RMS$_NMF) ||
            (r0_status == RMS$_FNF)) {
            finished = TRUE;
        } else {
            errchk_sig (r0_status);
        }
    }

    r0_status = lib$file_scan_end (&fab,
                                   &context);
    errchk_sig (r0_status);
}
