
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <jpidef.h>
#include <descrip.h>
#include <errnodef.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void signal_handler (int signum) {

    (void)printf ("Caught signal %d\n", signum);

    exit (EXIT_SUCCESS);
}


/******************************************************************************/
int main (int argc, char *argv[]) {
/*
** Use undocumented system server SYS$SIGPRC () to send a SIGUSR1 to any PID in
** the cluster.  Note this requires GROUP priv to signal processes in your
** group but not owned by your UIC, or WORLD priv to signal processes not in
** your group.
**
** Normally, you'd deliver a signal to another process, but this demo
** just delivers a USR1 signal to the same process.
*/

extern unsigned int sys$sigprc (unsigned int *,             /* Target PID */
                                struct dsc$descriptor_s *,  /* Process name */
                                int);                       /* Signal number */

static int item_code = JPI$_PID;
static int r0_status;
static unsigned int pid;
  
    /*
    ** Register our signal handler.
    */
    (void)signal (SIGUSR1, signal_handler);

    /*
    ** Although we can just send a zero to have SYS$SIGPRC send a signal to us,
    ** let's get our pid anyway.
    */
    r0_status = lib$getjpi (&item_code,
                            0,
                            0,
                            &pid,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Send the signal to the specified PID.
    */
    r0_status = sys$sigprc (&pid, 0, C$_SIGUSR1);
    errchk_sig (r0_status);

    r0_status = sys$hiber ();
    errchk_sig (r0_status);

    (void)printf ("We'll never get here\n");
}
