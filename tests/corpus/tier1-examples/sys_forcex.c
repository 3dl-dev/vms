
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <lckdef.h>
#include <descrip.h>
#include <clidef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program demos SYS$FORCEX (), which causes a SYS$EXIT () to be called by
** the targetted process.  Of course, first we need a target process, so
** the code runs itself as a subprocess.  To do this, we can also demo a use
** of the lock management system - $ENQ () and $DEQ ().
*/

static FILE *fp;

static unsigned int lksb[2];
static int r0_status;
static int jpiitm = JPI$_IMAGNAME;
static unsigned int spawn_flags = CLI$M_NOWAIT|CLI$M_NOTIFY;
static unsigned int sub_pid;
static unsigned int comp_status = 0;
static unsigned int state;
static unsigned int efn;

static char image[255+1];
static char file[] = "sys$scratch:demo_forcex.com;";

static struct dsc$descriptor_s image_d = { sizeof (image) - 1,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           image };

static $DESCRIPTOR (resource_d, "DEMO_FORCEX");
static $DESCRIPTOR (file_d, file);
static $DESCRIPTOR (process_d, "FORCEX_SUB");

    /*
    ** Attempt to take out a lock in exclusive mode.  If we get it, we are
    ** the parent process and we will spawn a subprocess to run this same
    ** image.  The subprocess will also attempt to get this lock in exclusive
    ** mode and fail, and so take the appropriate code path.
    **
    ** $ENQ () is an extensive system service capable of a huge range of
    ** functions.  This is a very simple use, where the lock is only job
    ** wide (i.e., not available system or cluster wide).  There is an entire
    ** chapter in the "Programming Concepts Manual" devoted to lock management.
    */
    r0_status = sys$enqw (EFN$C_ENF,
                          LCK$K_EXMODE,
                          lksb,
                          LCK$M_NOQUEUE,
                          &resource_d,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0);
    if (r0_status == SS$_NOTQUEUED) {
        /*
        ** Here we are the subprocess.  Output a message so the op knows we
        ** are up and running, then just sleep forever.
        */
        (void)printf ("Subprocess output: sleeping...\n");
        r0_status = sys$hiber ();
    } else {
        /*
        ** Here we are the "parent" process.  Check for unexpected errors
        ** from the sys$enqw ().
        */
        errchk_sig (r0_status);
        errchk_sig (lksb[0]);

        /*
        ** Get the name of this image.
        */
        r0_status = lib$getjpi (&jpiitm,
                                0,
                                0,
                                0,
                                &image_d,
                                &image_d.dsc$w_length);
        errchk_sig (r0_status);

        /*
        ** Write a temporary command procedure to run this image, and then
        ** wait for a minute before exiting.
        */
        fp = fopen (file, "w");
        (void)fprintf (fp,
                       "$ set noon\n");
        (void)fprintf (fp,
                       "$ run %-.*s\n",
                       image_d.dsc$w_length,
                       image_d.dsc$a_pointer);
        (void)fprintf (fp,
                       "$ write sys$output \"Subprocess output: waiting...\n");
        (void)fprintf (fp,
                       "$ wait ::15\n");
        (void)fprintf (fp,
                       "$ write sys$output \"Subprocess output: exiting...\n");
        (void)fprintf (fp,
                       "$ exit");
        if (fclose (fp) != 0) {
            (void)printf ("Could not close file!\n");
            exit (EXIT_FAILURE);
        }

        /*
        ** Get an event flag to be set when the subprocess exits.
        */
        r0_status = lib$get_ef (&efn);
        errchk_sig (r0_status);

        /*
        ** Spawn the subprocess.
        */
        r0_status = lib$spawn (0,
                               &file_d,
                               0,
                               &spawn_flags,
                               &process_d,
                               &sub_pid,
                               &comp_status,
                               &efn,
                               0,
                               0);
        errchk_sig (r0_status);

        /*
        ** Give the subprocess some image activation time ;-)
        */
        (void)sleep (5);

        /*
        ** Finally demo the call.  Note the SS$_ABORT.  This is the condition
        ** that the sys$exit () call will use in the context of the target
        ** process.
        */
        r0_status = sys$forcex (&sub_pid,
                                0,
                                SS$_ABORT);
        errchk_sig (r0_status);

        /*
        ** Give the subprocess some image rundown time ;-)
        */
        (void)sleep (5);

        (void)printf ("Note at this stage the subprocess is still there,\n"
                      "as demonstrated by the event flag being clear, and\n"
                      "the completion status still being zero.  This shows\n"
                      "that the image running in the subprocess was exited\n"
                      "rather than the entire subprocess exiting.  Compare\n"
                      "this behaviour to $DELPRC ()\n\n");

        (void)printf ("         efn: "); 
        r0_status = sys$readef (efn, &state);
        if (r0_status == SS$_WASSET) {
            (void)printf ("set\n");
        } else {
            if (r0_status == SS$_WASCLR) {
                (void)printf ("clear\n");
            } else {
                errchk_sig (r0_status);
            }
        }
        (void)printf (" comp_status: %08x\n\n", comp_status);
        (void)printf ("Now let's wait for the subprocess to complete...\n");

        /*
        ** Wait for the event flag to be set when the subprocess exits.
        */
        r0_status = sys$waitfr (efn);
        errchk_sig (r0_status);

        /*
        ** Free the event flag we used.
        */
        r0_status = lib$free_ef (&efn);
        errchk_sig (r0_status);

        /*
        ** Dequeue (release) the lock we took out.  Note that the subprocess
        ** never managed to aquire the lock, so that code stream doesn't need
        ** to do this.  The argument, which is the second longword in the lock
        ** status block, is the lock ID, which was written by the successful
        ** call to sys$enqw ().
        */
        r0_status = sys$deq (lksb[1],
                             0,
                             0,
                             0);
        errchk_sig (r0_status);

        /*
        ** Delete the temporary command procedure.
        */
        (void)remove (file);
    }
}
