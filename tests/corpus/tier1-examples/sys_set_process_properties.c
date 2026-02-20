
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ppropdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

#ifdef __VAX
#  error "This service is not available on VAX"
#endif /* __VAX */

static int r0_status;
static unsigned __int64 prev_value;

    r0_status = sys$set_process_propertiesw (0,
                                             0,
                                             0,
                                             PPROP$C_PARSE_STYLE_PERM,
                                             1,
                                             &prev_value,
                                             0,
                                             0,
                                             0);
    errchk_sig (r0_status);

    (void)printf ("Process parse style was %s\n",
                  (prev_value == 0) ? "Traditional" : "Extended");
    (void)printf ("And is now Extended\n");

    r0_status = sys$set_process_propertiesw (0,
                                             0,
                                             0,
                                             PPROP$C_PARSE_STYLE_PERM,
                                             prev_value,
                                             0,
                                             0,
                                             0,
                                             0);
    errchk_sig (r0_status);

    (void)printf ("And is now reset to %s\n",
                  (prev_value == 0) ? "Traditional" : "Extended");

}
