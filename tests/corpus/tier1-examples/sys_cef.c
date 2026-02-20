
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <efndef.h>
#include <time.h>
#include <descrip.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#define MAX_SECONDS 15


/******************************************************************************/
int main (void) {

static GENERIC_64 time_bin[3];

static int r0_status;

/*
** Using element zero to indicate EFN cluster.
*/
static unsigned int efn[3] = { 64, 64, 65 };

/*
** Using element zero to make sure that seconds are different.
*/
static unsigned int seconds[3] = { 0, 0, 0 };
static unsigned int mask = 0;
static unsigned int state;

static int i;

static $DESCRIPTOR (cluster_name_d, "Cluster Two");

static char time[23+1];
static struct dsc$descriptor_s time_d = { 0,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          time };


    /*
    ** Associate a common event flag cluster with the process.  We are using
    ** cluster 2 (efns 64 to 95).  The cluster is named "Cluster Two" and
    ** we are creating it on a temp basis and ensuring only our UIC can
    ** access it.
    **
    ** Normally, you use CEFs to do interprocess semaphores.  This code just
    ** demos the calls.
    */
    r0_status = sys$ascefc (64,
                            &cluster_name_d,
                            1,
                            0);
    errchk_sig (r0_status);
                            
    /*
    ** Immediately delete the cluster.  The cluster will still exist until
    ** its reference count drops to zero (i.e., when we call $dacefc at
    ** the end of the program).  As this is a temporary cluster, you don't
    ** really need this call.
    */
    r0_status = sys$dlcefc (&cluster_name_d);
    errchk_sig (r0_status);

    seconds[1] = 2;
    seconds[2] = 5;
    for (i = 1; i <= 2; i++) {
        time_d.dsc$w_length = sprintf (time,
                                       "0 00:00:%02u.00",
                                       seconds[i]);

        /*
        ** Convert seconds into binary.
        */
        r0_status = sys$bintim (&time_d,
                                &time_bin[i]);
        errchk_sig (r0_status);

        /*
        ** Set a timer to expire in the random seconds and set the event flag
        ** we have been given.  Note the 4th parameter is also the event flag
        ** so we can identify our timers.
        */
        r0_status = sys$setimr (efn[i],
                                &time_bin[i],
                                0,
                                efn[i],
                                0);
        errchk_sig (r0_status);

        (void)printf ("Set timer %u to expire in %u seconds\n",
                      i,
                      seconds[i]);

        /*
        ** We are playing with EFNs 64 and 65 in the cluster.  We are going to
        ** need a bit mask to specify them for the wait.  Construct one.
        */
        mask |= 1<<(efn[i] - 64);
    }

    /*
    ** Now wait for either efn to set from the expiration of a timer.
    */
    r0_status = sys$wflor (efn[0],
                           mask);
    errchk_sig (r0_status);

    /*
    ** An event flag was set.  Read the state of the cluster.
    */
    r0_status = sys$readef (efn[0],
                            &state);
    errchk_sig (r0_status);

    /*
    ** Report what happened.
    */
    for (i = 1; i <= 2; i++) {                                
        if ((state & (1<<(efn[i] - 64))) != 0) {
            (void)printf ("Timer %u expired first\n",
                          i);
        } else {
            r0_status = sys$cantim (efn[i],
                                    0);
            errchk_sig (r0_status);
            (void)printf ("Cancelled timer %u\n",
                          i);
        }
    }

    /*
    ** Disassociate from the common event flag cluster.
    */
    r0_status = sys$dacefc (efn[0]);
    errchk_sig (r0_status);
}
