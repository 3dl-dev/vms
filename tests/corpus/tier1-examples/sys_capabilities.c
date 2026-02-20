
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <syidef.h>
#include <capdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** You need ALTPRI and WORLD privileges to run this code.  Please be aware that
** this code will affect the scheduling of this process (and possibly other
** processes) on this machine.  Please do not run this code unless you
** understand the consequences of changing CPU capabilities.
*/

#ifdef __VAX
#  error "Code only runs on 64 bit platforms"
#endif /* __VAX */

static IOSB iosb;

static GENERIC_64 cpu_prev_mask;
static GENERIC_64 prc_prev_mask;
static GENERIC_64 mod_mask;
static GENERIC_64 sel_mask;

static int capability = 16;
static int r0_status;
static unsigned int cpu_id;
static unsigned int active_cpus;

static ILE3 syiitms[] = { 4, SYI$_ACTIVE_CPU_MASK, &active_cpus, NULL,
                          0, 0, NULL, NULL };

    /*
    ** Set up our mask values.
    */
    mod_mask.gen64$q_quadword = CAP$M_USER16;
    sel_mask.gen64$q_quadword = CAP$M_USER16;

    /*
    ** Get the bitmap of active cpus.
    */
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

    /*
    ** Read the current CPU capabilities.
    */
    r0_status = sys$cpu_capabilities (cpu_id,
                                      0,
                                      0,
                                      &cpu_prev_mask,
                                      0);
    errchk_sig (r0_status);

    /*
    ** Set user capability 16 on for this CPU
    */
    r0_status = sys$cpu_capabilities (cpu_id,
                                      &sel_mask,
                                      &mod_mask,
                                      0,
                                      0);
    switch (r0_status) {
        case SS$_NOPRIV :
            (void)fprintf (stderr, "You need ALTPRI and WORLD privs\n");
            exit (r0_status);
            break;
        case SS$_CPUCAP :
            (void)fprintf (stderr,
                           "Attempted operation would place a thread in an "
                           "unrunnable state\n");
            exit (r0_status);
            break;
        default :
            errchk_sig (r0_status);
            break;
    }                          

    /*
    ** Reserve user capability 16 for this thread of execution.
    */
    r0_status = sys$get_user_capability (&capability,
                                         0,
                                         &sel_mask,
                                         0,
                                         0);
    errchk_sig (r0_status);

    /*
    ** Switch on user capability 16 for this thread of execution.
    */
    mod_mask = sel_mask;
    r0_status = sys$process_capabilities (0,
                                          0,
                                          &sel_mask,
                                          &mod_mask,
                                          &prc_prev_mask,
                                          0);
    errchk_sig (r0_status);

    /*
    ** Switch off user capability for this thread of execution if it was off
    ** previously.
    */
    mod_mask.gen64$q_quadword &= ~CAP$M_USER16;
    if ((prc_prev_mask.gen64$q_quadword & CAP$M_USER16) == 0) {
        r0_status = sys$process_capabilities (0,
                                              0,
                                              &sel_mask,
                                              &mod_mask,
                                              0,
                                              0);
        errchk_sig (r0_status);
    }
    
    /*
    ** Release the capability reservation.
    */
    r0_status = sys$free_user_capability (&capability,
                                          0,
                                          0);
    errchk_sig (r0_status);

    /*
    ** Set user capability 16 off for the CPU if it was previously off
    */
    if ((cpu_prev_mask.gen64$q_quadword & CAP$M_USER16) == 0) {
        r0_status = sys$cpu_capabilities (cpu_id,
                                          &sel_mask,
                                          &mod_mask,
                                          0,
                                          0);
        errchk_sig (r0_status);
    }
}
