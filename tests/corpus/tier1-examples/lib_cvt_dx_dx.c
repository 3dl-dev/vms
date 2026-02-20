
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>

#include "errchk.h"

#ifdef __VAX
#error "Requires 64 bit integers"
#endif


/******************************************************************************/
int main (void) {
/*
** This demos just one of the many possible input/output combinations for
** lib$cvt_dx_dx ().  See the documentation for all the possibilities.
*/

static unsigned long long int quadword = 289575739823571;

static int r0_status;

static struct dsc$descriptor_s number_d = { sizeof (quadword),
                                            DSC$K_DTYPE_Q,
                                            DSC$K_CLASS_S,
                                            (char *)&quadword };
static struct dsc$descriptor_d output_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_D,
                                            NULL };

    /*
    ** The casts are present because new starlet defines the
    ** type of the first two parameters as pointer to unsigned
    ** int.  HP should probably fix this so it's pointer to void.
    */
    r0_status = lib$cvt_dx_dx ((unsigned int *)&number_d,
                               (unsigned int *)&output_d,
                               &output_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("\"%-.*s\"\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    /*
    ** And even worse, new starlet on the alpha defines the
    ** paramter type here as pointer to int64.  This is really
    ** bad, and HP should definately fix this.
    */
    r0_status = lib$sfree1_dd ((unsigned __int64 *)&output_d);
    errchk_sig (r0_status);
}
