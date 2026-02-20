
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <vadef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

#ifdef __VAX
#  error "Alpha/IPF specific code"
#endif /* __VAX */

static GENERIC_64 region_id;
static __int64 rsize;
static int r0_status;
static unsigned int retlen;
static REGSUM buffer;

    region_id.gen64$q_quadword = VA$C_P2;

    r0_status = sys$get_region_info (VA$_REGSUM_BY_ID,
                                     &region_id,
                                     0,
                                     0,
                                     sizeof (buffer),
                                     &buffer,
                                     &retlen);
    errchk_sig (r0_status);

    rsize = buffer.va$q_region_size;
    (void)printf ("Info for the P2 region:\n"
                  "        Flags: %08X\n"
                  "         Prot: %08X\n"
                  "     Start VA: %0LX\n"
                  "First Free VA: %0LX\n"
                  "  Region size: %Lu bytes\n",
                  buffer.va$l_flags,
                  buffer.va$l_region_protection,
                  buffer.va$pq_start_va,
                  buffer.va$pq_first_free_va,
                  rsize);
}
