
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <chfdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifndef SS$_UNLOCKED
#define SS$_UNLOCKED 4764
#endif


/******************************************************************************/
static int handler (struct chf$signal_array *sig_array,
                    struct chf$mech_array *mech_array) {
/*
** This is a condition handler.  Usually condition handlers return SS$_CONTINUE
** or SS$_RESIGNAL, depending on if they can or cannot handler the condition.
** A third method to return exists, and that is to call SYS$UNWIND.
*/

static int r0_status;
static unsigned int flags;

static int index;

static unsigned char outadr[4];
static char message[256];
static char outbuf[512];

static struct dsc$descriptor_s message_d = { 0,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             message };
static struct dsc$descriptor_s outbuf_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            outbuf };

static unsigned int ss$_unwind = SS$_UNWIND;
static unsigned int ss$_abort = SS$_ABORT;
static unsigned int ss$_unlocked = SS$_UNLOCKED;

    index = lib$match_cond ((unsigned int *)&sig_array->chf$is_sig_name,
                            &ss$_unwind,
                            &ss$_abort,
                            &ss$_unlocked);
    switch (index) {
        case 0 :
            /*
            ** We can't handle this exception.  Resignal.
            */
            return SS$_RESIGNAL;
            break;
        case 1 :
            /*
            ** Unwinding.  Usually just do nothing.  For this demo, say
            ** what's going on.
            */
            (void)printf ("Unwinding...\n");
            break;
        case 2 :
        case 3 :
            /*
            ** Display appropriate message.
            */
            message_d.dsc$w_length = sizeof (message);
            r0_status = sys$getmsg (sig_array->chf$is_sig_name,
                                    &message_d.dsc$w_length,
                                    &message_d,
                                    flags,
                                    outadr);
            errchk_sig (r0_status);

            if (sig_array->chf$is_sig_args > outadr[1]) {
                outbuf_d.dsc$w_length = sizeof (outbuf);
                r0_status = sys$faol (&message_d,
                                      &outbuf_d.dsc$w_length,
                                      &outbuf_d,
                                      &sig_array->chf$is_sig_arg1);
                errchk_sig (r0_status);

                (void)printf ("%-.*s\n",
                              outbuf_d.dsc$w_length,
                              outbuf_d.dsc$a_pointer);
            } else {
                (void)printf ("%-.*s\n",
                              message_d.dsc$w_length,
                              message_d.dsc$a_pointer);
            }

            if (index == 2) {
                /*
                ** For SS$_UNLOCKED, return to the module that established
                ** the handler, i.e., routine ()
                */
                r0_status = sys$unwind (0, 0);
            } else {
                /*
                ** For SS$_ABORT, return to the caller of the module that
                ** established the handler, i.e., main ()
                */
                r0_status = sys$unwind ((unsigned int *)&mech_array->chf$is_mch_depth,
                                        0);
            }
            errchk_sig (r0_status);
            break;
        default :
            (void)printf ("Unexpected index from lib$match_cond (): %d!\n",
                          index);
            (void)sys$exit (SS$_ABORT);
            break;
    }
    return SS$_CONTINUE;
}


/******************************************************************************/
static void routine (void) {

static $DESCRIPTOR (lock_d, "Pandora's box");

    /*
    ** Establish a condition handler.
    */
    (void)lib$establish (handler);

    /*
    ** Signal SS$_UNLOCKED.  Note you can pass parameters via the signal
    ** array to the condition handler by including optional arguments to
    ** LIB$SIGNAL.
    */
    (void)lib$signal (SS$_UNLOCKED, &lock_d);

    /*
    ** Because the condition handler called unwind with zero as the first
    ** parameter, control returns here.
    */
    (void)printf ("After unlocked\n");

    /*
    ** Signal SS$_ABORT.
    */
    (void)lib$signal (SS$_ABORT);

    /*
    ** Because the condition handler called unwind with an appropriate depth
    ** as the first parameter, control never returns here.  It returns to the
    ** caller of the routine that established the handler.  That is, the
    ** main () routine.  Therefore, this printf statement never executes.
    */
    (void)printf ("After abort\n");
}


/******************************************************************************/
int main (void) {

    routine ();

    /*
    ** Control returns here after the second call to unwind.
    */
    (void)printf ("After routine ()\n");
}
