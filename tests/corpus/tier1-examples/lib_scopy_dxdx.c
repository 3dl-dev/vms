
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
static $DESCRIPTOR (static_d, "test string");
static struct dsc$descriptor_d dynamic_d = { 0,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_D,
                                             NULL };
/*
** This is a variable string.
*/
#define MAXSTRLEN 4
static struct {
    unsigned short int curlen;
    char body [MAXSTRLEN];
} varstr = { 0, 0 };

/*
** And a variable string descriptor.
*/
static struct dsc$descriptor_vs varstr_d = { MAXSTRLEN,
                                             DSC$K_DTYPE_VT,
                                             DSC$K_CLASS_VS,
                                             (char *)&varstr };

    r0_status = lib$scopy_dxdx (&static_d,
                                &dynamic_d);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  dynamic_d.dsc$w_length,
                  dynamic_d.dsc$a_pointer);

    r0_status = lib$scopy_dxdx (&dynamic_d,
                                &varstr_d);
    if (r0_status == LIB$_STRTRU) {
        (void)printf ("Expected string truncation\n");
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("%-.*s",
                  varstr.curlen,
                  varstr.body);

    r0_status = lib$sfree1_dd ((unsigned __int64 *)&dynamic_d);
    errchk_sig (r0_status);
}
