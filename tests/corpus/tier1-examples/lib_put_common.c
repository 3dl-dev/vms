
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** This program demos lib$put_common () and lib$get_common ().  These
** routines allow you to store and retrieve data that is persistant
** over image activations.  So to demo this program, you have to
** run it more than once.  The first time it runs, it stores a counter
** in the common area.  Each subsequent time, it gets the counter, prints
** it out, and increments it.
*/

static int r0_status;
static unsigned int counter = 0;

static char *p;

static char data[255+1];

static struct dsc$descriptor_s data_d = { sizeof (data) - 1,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          data };

static const char prefix[] = "lib_put_common: ";


    r0_status = lib$get_common (&data_d,
                                &data_d.dsc$w_length);
    errchk_sig (r0_status);

    if (data_d.dsc$w_length == 0) {
        data_d.dsc$w_length = sprintf (data,
                                       "%s%u",
                                       prefix,
                                       counter);

        r0_status = lib$put_common (&data_d);
        errchk_sig (r0_status);

        (void)printf ("Successfully stored data in the common area.\n"
                      "Run this program again to see the magic.\n");
    } else {
        if (memcmp (data_d.dsc$a_pointer,
                    prefix,
                    strlen (prefix)) == 0) {

            data[data_d.dsc$w_length] = '\0';
	    p = strtok (data, " ");
            p = strtok (NULL, " ");
            counter = atoi (p);

            (void)printf ("We've been here before, %u time%s I think.\n",
                          ++counter,
                          counter == 1 ? "" : "s");

            data_d.dsc$w_length = sprintf (data,
                                           "%s%u",
                                           prefix,
                                           counter);
            r0_status = lib$put_common (&data_d);
            errchk_sig (r0_status);
        } else {
            (void)printf ("You are already using common for something!\n");
            (void)printf ("The string in the command area is:\n%-.*s\n",
                          data_d.dsc$w_length,
                          data_d.dsc$a_pointer);
        }
    }
}
