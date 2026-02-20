
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <syidef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <efndef.h>
#include <capdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** You need ALTPRI to run this code.
*/

#ifdef __VAX
#  error "Alpha/IPF specific code"
#endif /* __VAX */

static IOSB iosb;

static GENERIC_64 state = { CAP$M_IMPLICIT_AFFINITY_SET };
static GENERIC_64 prev_mask;

static int r0_status;
static unsigned int active_cpus;

static int cpu_id;

static ILE3 syiitms[] = { 4, SYI$_ACTIVE_CPU_MASK, &active_cpus, NULL,
                          0, 0, NULL, NULL };

    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             syiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Find the first active CPU in the set.
    */
    for (cpu_id = 0; cpu_id < sizeof (active_cpus) * 8 - 1; cpu_id++) {
        if ((active_cpus & (1<<cpu_id)) != 0) {
            break;
        }
    }

    (void)printf ("Setting this thread's affinity to CPU %u\n", cpu_id);
    r0_status = sys$set_implicit_affinity (0,
                                           0,
                                           &state,
                                           cpu_id,
                                           &prev_mask);
    errchk_sig (r0_status);

    (void)printf ("Clearing this thread's implicit affinity\n");
    state.gen64$q_quadword = CAP$M_IMPLICIT_AFFINITY_CLEAR;
    r0_status = sys$set_implicit_affinity (0,
                                           0,
                                           &state,
                                           cpu_id,
                                           &prev_mask);
    errchk_sig (r0_status);
}
