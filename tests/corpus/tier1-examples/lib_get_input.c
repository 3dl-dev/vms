
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>

#include <ssdef.h>
#include <stsdef.h>

/*
** This include defines RMS (record management services) return codes.
** RMS supplies different file types such as indexed, relative, and
** sequential files (also stream for you un*x types), with a myriad
** of options on how the file is processed.  It is being included here
** because some lib$ routines use RMS to perform underlying functions, and
** if something goes wrong, they propagate the error up.
*/
#include <rms.h>

/*
** This include defines string descriptors, a rather important concept on
** OpenVMS.
*/
#include <descrip.h>

#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static int r0_status;

/*
** A buffer to contain the user's input.
*/
static char input[255+1];

/*
** A "static string descriptor".  Descriptors are structures that contain
** the length of the object, the type and class of the object, and a
** pointer to the object.  In this case, we have the length of the string,
** the type is text and the class is static, and a pointer to our buffer.
*/
static struct dsc$descriptor_s input_d = { sizeof (input) - 1,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };

/*
** Another string descriptor.  We are using the $DESCRIPTOR macro supplied
** by the descrip.h header to construct a const variable called prompt_d,
** containing the string "Enter your input: ".
*/
static const $DESCRIPTOR (prompt_d, "Enter your input: ");

    /*
    ** Read input from the user.  The input will end if the length of the
    ** string is reached, or the user hits a terminator character, such
    ** as carriage return or ctrl-z.
    */
    r0_status = lib$get_input (&input_d,
                               &prompt_d,
                               &input_d.dsc$w_length);
    if (r0_status == RMS$_EOF) {
        /*
        ** If the user typed ctrl-z, this error is returned.
        */
        (void)printf ("End of file detected\n");
    } else {
        /*
        ** Use the $VMS_STATUS_SUCCESS macro supplied by the stsdef.h
        ** header to test the status of the call.  OpenVMS success
        ** status is indicated by the low bit being set, so all odd
        ** numbered statuses are success, and all even numbers are
        ** error.
        */
        if (!$VMS_STATUS_SUCCESS (r0_status)) {
            /*
            ** An error.  Call lib$signal to produce a stack dump.
            */
            (void)lib$signal (r0_status);
        }
        (void)printf ("Input was: \"%-.*s\"\n", 
                      input_d.dsc$w_length,
                      input_d.dsc$a_pointer);
    }
}
