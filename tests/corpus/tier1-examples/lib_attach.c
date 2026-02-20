
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ctype.h>
#include <descrip.h>
#include <rmsdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <libclidef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb;

static unsigned int our_pid;
static unsigned int sub_pid;
static unsigned int uic;
static unsigned int prio;
static unsigned int completion;
static int finished = FALSE;
static int r0_status;
static int sym_flag = LIB$K_CLI_GLOBAL_SYM;

static ILE3 jpiitms[] = { 4, JPI$_PID, &our_pid, NULL,
                          4, JPI$_UIC, &uic, NULL,
                          4, JPI$_PRIB, &prio, NULL,
                          0, 0, NULL, NULL };

static char input[255+1];
static char command[255+1];

static struct dsc$descriptor_s input_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };
static struct dsc$descriptor_s command_d = { 0,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             command };

static $DESCRIPTOR (sym_name_d, "LIB_ATTACH_RET");
static const $DESCRIPTOR (main_prompt_d, "Main process> ");
static const $DESCRIPTOR (sub_prompt_d, "Sub process> ");


    /*
    ** Get our PID.
    */
    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Say what command will get them back here.
    */
    command_d.dsc$w_length = sprintf (command, "attach/id=%08x", our_pid);
    (void)printf ("Attaching to subprocess.  Type "
                  "\"%-.*s\" (or \"LIB_ATTACH_RET\") to return here...\n",
                  command_d.dsc$w_length,
                  command_d.dsc$a_pointer);

    r0_status = lib$set_symbol (&sym_name_d,
                                &command_d,
                                &sym_flag);
    errchk_sig (r0_status);

    /*
    ** And spawn a subprocess.
    */
    r0_status = lib$spawn (0,
                           0,
                           0,
                           0,
                           0,
                           &sub_pid,
                           &completion,
                           0,
                           0,
                           0,
                           &sub_prompt_d,
                           0,
                           0);
    errchk_sig (r0_status);

    if (completion == 0) {
        /*
        ** If completion is zero, then they followed instructions and
        ** eventually used ATTACH.  Say what's going on.
        */
        (void)printf ("Returned from subprocess %08x.\n", sub_pid);
        (void)printf ("Type \"jump\" to reattach to the subprocess.\n");
        (void)printf ("Type \"exit\" or ctrl-z to exit the program.\n");
        while (!finished) {
            /*
            ** Now allow them to attach to the subprocess or exit.
            */
            input_d.dsc$w_length = sizeof (input) - 1;
            r0_status = lib$get_input (&input_d,
                                       &main_prompt_d,
                                       &input_d.dsc$w_length);
            if (r0_status == RMS$_EOF) {
                finished = TRUE;
                continue;
            } else {
                errchk_sig (r0_status);
                if (toupper (input[0]) == 'J') {
                    /*
                    ** Finally get to demo lib$attach.
                    */
                    (void)printf ("Attaching to subprocess.  Type "
                                  "\"%-.*s\" (or \"LIB_ATTACH_RET\") to "
                                  "return here...\n",
                                  command_d.dsc$w_length,
                                  command_d.dsc$a_pointer);
                    r0_status = lib$attach (&sub_pid);
                    errchk_sig (r0_status);
                    if (completion == SS$_EXITFORCED) {
                        finished = TRUE;
                        continue;
                    }
                } else {
                    if (toupper (input[0]) == 'E') {
                        (void)printf ("Exiting...\n");
                        finished = TRUE;
                    } else {
                        (void)printf ("Options are \"Jump\" and \"Exit\"\n");
                    }
                }
            }
        }
    } else {
        (void)printf ("You did it wrong!  You weren't supposed to log out!\n");
    }

    if (completion != SS$_EXITFORCED) {
        /*
        ** If the subprocess never exited, kill it.
        */
        r0_status = sys$delprc (&sub_pid,
                                0,
                                0);
        if (r0_status != SS$_NONEXPR) {
            errchk_sig (r0_status);
        }
    }
} 
