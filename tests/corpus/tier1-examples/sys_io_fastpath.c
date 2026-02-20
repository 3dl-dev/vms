
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
#include <fpdef.h>
#include <string.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void usage (void) {

    (void)printf (
        "This program allows you to programattically control the CPUs that\n"
        "can participate in FastPath activities.\n\n"
        "You use the program by defining a foreign symbol and then specifying\n"
        "the CPUs you wish to use for FastPath.\n\n"
        "To enable all CPUs, you specify -1.  For example:\n\n"
        "\t$ FASTPATH :== $<device><dir>SYS_IO_FASTPATH.EXE\n"
        "\t$ FASTPATH 1   ! Prefer CPU 1\n"
        "\t$ FASTPATH 0,1 ! Prefer CPUs 0 and 1\n"
        "\t$ FASTPATH -1  ! Prefer all CPUs.\n\n"
        "Note that the resultant set of CPUs is the intersection of the CPUs\n"
        "specified by SYSGEN parameter IO_PREFER_CPUS, the CPUs currently in\n"
        "the active set (i.e., in the RUN state), and your parameter.\n\n"
        "For further information, see the documentation in the \"System\n"
        "Services Manual\".\n");
}


/******************************************************************************/
int main (int argc, char *argv[]) {
/*
** This program requires PHY_IO privilege.  Please don't run this program
** unless you understand the consequences of rebalancing FastPath activities
** on your hardware.
**
** You have to link this code against the LIB text library for C.  You can
** accomplish this by defining logical name DECC$TEXT_LIBRARY prior to
** compiling the program.
**
** For example:
**
**    $ define decc$text_library sys$share:sys$lib_c.tlb
**    $ cc sys_io_fastpath.c
**    $ link sys_io_fastpath
**
*/

static IOSB iosb;

static char *p;
static char *start;

static int r0_status;
static unsigned int active_cpus;
static unsigned int bitmap = 0;
static unsigned int fastpath;

static const unsigned int function = FP$K_BALANCE_PORTS;

static int cpu;
static int mask;

static ILE3 syiitms[] = { 4, SYI$_FAST_PATH, &fastpath, NULL,
                          4, SYI$_ACTIVE_CPU_MASK, &active_cpus, NULL,
                          0, 0, NULL, NULL };

    if (argc < 2) {
        usage ();
        exit (EXIT_SUCCESS);
    }

    /*
    ** Get the setting for sysgen param FAST_PATH, and the active CPU mask.
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

    if (!fastpath) {
        (void)fprintf (stderr,
                       "FastPath is not enabled on this system: abort\n");
        exit (EXIT_FAILURE);
    }

    start = argv[1];
    while (TRUE) {
        p = strtok (start, ",");
        if (p == NULL) {
            break;
        }
        if (start != NULL) {
            start = NULL;
        }
        cpu = atoi (p);
        if (cpu == -1) {
            bitmap = cpu;
            break;
        } else {
            if (cpu < 0) {
                (void)fprintf (stderr,
                               "%d is an illegal CPU number: abort\n",
                               cpu);
                exit (EXIT_FAILURE);
            } else {
                mask = 1 << cpu;
                if ((mask & active_cpus) == 0) {
                    (void)fprintf (stderr,
                                   "CPU %d is not in the active set: abort\n",
                                   cpu);
                    exit (EXIT_FAILURE);
                } else {
                    bitmap |= mask;
                }
            }
        }
    }

    r0_status = sys$io_fastpathw (EFN$C_ENF,
                                  (unsigned int)&bitmap,
                                  function,
                                  &iosb,
                                  0,
                                  0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);

    (void)printf ("Successfully rebalanced CPUs\n");
}
