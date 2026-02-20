
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <libdef.h>
#include <descrip.h>
#include <rms.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

/*
** Define a macro to construct a "counted string", which consists of a
** byte containing the length of the string followed by the string.
*/
#define ascic(name,string) \
struct { \
    unsigned char count; \
    char s[sizeof (string)]; \
} name = { sizeof (string) - 1, string };


/******************************************************************************/
static int handler (unsigned int sigargs[],
                    unsigned int *mecargs) {
/*
** Condition handler.  The main code will signal errors, and this is here
** to display them and continue.
*/

    switch (sigargs[1]) {
        case LIB$_AMBKEY:
        case LIB$_UNRKEY:
            sigargs[0] -= 2;
            (void)sys$putmsg (sigargs,
                              0,
                              0,
                              0);
            return SS$_CONTINUE;
            break;
        default:
            return SS$_RESIGNAL;
            break;
    }
}


/******************************************************************************/
int main (void) {

/*
** This program demonstrates calling lib$lookup_table ().  This routine is
** meant to assist programmers doing command keywords etc.  Here, we will
** loop and prompt for one of five commands.  The response will be just
** to echo the command back.  EXIT will get you out.  It's worth typing
** shortened commands to demo the ambiguity handling.  "C", "D", and "E"
** are all non-ambiguous.  But "S" is ambiguous, and will generate an
** appropriate error.
*/

ascic (create, "CREATE");
ascic (delete, "DELETE");
ascic (show, "SHOW");
ascic (exit, "EXIT");
ascic (set, "SET");

unsigned int table[] = { 10,                            /* Vector count */
                         (unsigned int)&create, 1,
                         (unsigned int)&delete, 2,
                         (unsigned int)&show,   3,
                         (unsigned int)&set,    4,
                         (unsigned int)&exit,   5 };

static int r0_status;
static unsigned int key_value;
static int i;

static char finished = FALSE;

static char input[255+1];
static char keyword[255+1];

static struct dsc$descriptor_s input_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };
static struct dsc$descriptor_s keyword_d = { 0,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             keyword };

static const $DESCRIPTOR (prompt_d, "Command> ");


    /*
    ** Establish an error handler to intercept errors from lib$lookup_key.
    ** You could of course do this differently by testing r0_status after
    ** the call to the function, but this is more elegant.
    */
    lib$establish (handler);

    (void)printf ("You can type one of five commands at the prompt:\n"
                  "CREATE, DELETE, SHOW, SET, or EXIT.  Typing CTRL-Z\n"
                  "will also terminate the program.\n\n");

    while (!finished) {
        /*
        ** Get user's input.
        */
        input_d.dsc$w_length = sizeof (input) - 1;
        r0_status = lib$get_input (&input_d,
                                   &prompt_d,
                                   &input_d.dsc$w_length);
        if (r0_status == RMS$_EOF) {
            /*
            ** Handle CTRL-Z
            */
            finished = TRUE;
            continue;
        } else {
            errchk_sig (r0_status);
        }

        /*
        ** Ignore zero length input.
        */
        if (input_d.dsc$w_length == 0) {
            continue;
        }

        /*
        ** Uppercase the input.
        */
        for (i = 0; i < input_d.dsc$w_length; i++) {
            input[i] = toupper (input[i]);
        }

        /*
        ** Do the table lookup.
        */
        keyword_d.dsc$w_length = sizeof (keyword) - 1;
        r0_status = lib$lookup_key (&input_d,
                                    table,
                                    &key_value,
                                    &keyword_d,
                                    &keyword_d.dsc$w_length);
        if (!$VMS_STATUS_SUCCESS (r0_status)) {
            (void)lib$signal (r0_status, 1, &input_d);
        } else {
            (void)printf ("Keyword is %-.*s\n",
                          keyword_d.dsc$w_length,
                          keyword_d.dsc$a_pointer);
            if (key_value == 5) {
                finished = TRUE;
            }
        }
    }
}
