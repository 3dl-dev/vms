
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <syidef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static int csid = -1;
static int code = SYI$_NODENAME;

static short int node_cnt;

static char nodename[255+1];
static struct dsc$descriptor_s nodename_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              nodename };
static char finished = FALSE;

    (void)printf ("Nodes in the cluster: ");
    while (!finished) {
        nodename_d.dsc$w_length = sizeof (nodename) - 1;
        r0_status = lib$getsyi (&code,
                                0,
                                &nodename_d,
                                &nodename_d.dsc$w_length,
                                &csid,
                                0);
        if (r0_status == SS$_NOMORENODE) {
            finished = TRUE;
            continue;
        } else {
            errchk_sig (r0_status);
        }
        if (node_cnt++ > 0) {
            (void)printf (", ");
        }
        (void)printf ("%-.*s",
                      nodename_d.dsc$w_length,
                      nodename_d.dsc$a_pointer);
    }
}
