
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int i;

static char lang[255+1];

static struct dsc$descriptor_s lang_d = { sizeof (lang) - 1,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          lang };


    r0_status = lib$get_users_language (&lang_d);
    if (r0_status == LIB$_ENGLUSED) {
        (void)printf ("Defaulted to English\n");
    } else {
        errchk_sig (r0_status);
        for (i = 0; i < sizeof (lang) - 1; i++) {
            if (lang[i] == ' ') {
                lang_d.dsc$w_length = i;
                break;
            }
        }

        (void)printf ("Language is %-.*s\n",
                      lang_d.dsc$w_length,
                      lang_d.dsc$a_pointer);
    }
}
