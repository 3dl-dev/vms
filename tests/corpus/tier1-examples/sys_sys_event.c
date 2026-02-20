
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ssdef.h>
#include <stsdef.h>
#include <sysevtdef.h>
#include <psldef.h>
#include <syidef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <efndef.h>
#include <cstdef.h>
#include <assert.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void add_active_cpu_ast (void) {

    (void)printf ("A CPU was added to the active set\n");
}


/******************************************************************************/
static void del_active_cpu_ast (void) {

    (void)printf ("A CPU was deleted from the active set\n");
}


/******************************************************************************/
static void stop_cpu (unsigned int target) {

static IOSB iosb;
static int r0_status;

    (void)printf ("Stopping CPU #%d\n", target);
    r0_status = sys$cpu_transitionw (CST$K_CPU_STOP,
                                     target,
                                     0,
                                     0,
                                     0,
                                     EFN$C_ENF,
                                     &iosb,
                                     0,
                                     0,
                                     0);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need CMKRNL priv to run this!\n");
	exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);
    }
}

/******************************************************************************/
static void start_cpu (unsigned int target) {

static IOSB iosb;
static int r0_status;

    (void)printf ("Starting CPU #%d\n", target);
    r0_status = sys$cpu_transitionw (CST$K_CPU_START,
                                     target,
                                     0,
                                     0,
                                     0,
                                     EFN$C_ENF,
                                     &iosb,
                                     0,
                                     0,
                                     0);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need CMKRNL priv to run this!\n");
	exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);
    }
}


/******************************************************************************/
int main (void) {

#ifdef __VAX
#  error "Alpha/IA64 specific code"
#endif /* __VAX */

static unsigned __int64 add_act_cpu_handle;
static unsigned __int64 del_act_cpu_handle;
static IOSB iosb;

static int r0_status;
static int cpu_cnt;
static int map_size;
static int remainder;
static unsigned int map_idx;
static unsigned int test_avail;
static unsigned int test_active;
static unsigned int target_cpu;

static int *avail_cpus;
static int *active_cpus;

static ILE3 syiitm_cnt[] = { 4, SYI$_MAX_CPUS, &cpu_cnt, 0,
                             0, 0, 0, 0 };

static ILE3 syiitm_map[] = { 0, SYI$_AVAIL_CPU_BITMAP, 0, 0,
                             0, SYI$_ACTIVE_CPU_BITMAP, 0, 0,
                             0, 0, 0, 0 };


    /*
    ** Let's find the available CPUs on this system.  This code is going to
    ** stop and start (or start and stop) the highest numbered non-primary
    ** CPU it finds.
    ** This might be a Bad Thing(tm) in your situation, particularly if you
    ** have a diligent systems manager that is manually load balancing device
    ** interrupts across CPUs.
    **
    ** Please read the disclaimer linked at the top of this code.
    ** You need CMKRNL privilege to run this code.
    */
    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             &syiitm_cnt,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);
    if (cpu_cnt < 2) {
	(void)printf ("You need a machine with more than one CPU "
                      "to run this code\n");
        exit (EXIT_FAILURE);
    } 

    (void)printf ("This system supports %d CPUs.\n", cpu_cnt);

    /*
    ** Calculate the size of the bitmap needed to represent all CPUs
    */
    remainder = cpu_cnt % 64;
    map_size = (cpu_cnt + 64 - remainder) / 8;

    avail_cpus = calloc (map_size, 1);
    if (avail_cpus == NULL) {
        (void)printf ("Cannot allocate available cpu map!\n");
        exit (EXIT_FAILURE);
    }
    active_cpus = calloc (map_size, 1);
    if (active_cpus == NULL) {
        (void)printf ("Cannot allocate active cpu map!\n");
        exit (EXIT_FAILURE);
    }

    /*
    ** Fill in the item list...
    */
    syiitm_map[0].ile3$w_length = map_size;
    syiitm_map[0].ile3$ps_bufaddr = avail_cpus;
    syiitm_map[1].ile3$w_length = map_size;
    syiitm_map[1].ile3$ps_bufaddr = active_cpus;

    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             &syiitm_map,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    r0_status = sys$set_system_event (SYSEVT$C_ADD_ACTIVE_CPU,
                                      add_active_cpu_ast,
                                      0,
                                      PSL$C_USER,
                                      SYSEVT$M_REPEAT_NOTIFY,
                                      &add_act_cpu_handle);
    errchk_sig (r0_status);

    r0_status = sys$set_system_event (SYSEVT$C_DEL_ACTIVE_CPU,
                                      del_active_cpu_ast,
                                      0,
                                      PSL$C_USER,
                                      SYSEVT$M_REPEAT_NOTIFY,
                                      &del_act_cpu_handle);
    errchk_sig (r0_status);

    /*
    ** Once the event ASTs are established, you would normally wait here with
    ** some form of wait call (e.g., $HIBER or $WAITFR) for events to occur.
    ** In this case, we will select a CPU and transition it in this code.
    */
    assert (sizeof (int) == 4);

    /*
    ** Find the highest non-zero integer in the available CPU map.  This
    ** integer contains the hightest numbered CPU.  Copy the same integer from
    ** the active set so we can find out if the CPU is on or off.
    ** Tell the compiler that programmers often want to compare an unsigned
    ** integer to zero ;)
    */
#pragma message save
#pragma message disable QUESTCOMPARE
    for (map_idx = map_size / 4 - 1; map_idx >= 0; map_idx--) {
#pragma message restore
        memcpy (&test_avail, avail_cpus+(map_idx*4), 4);
	if (test_avail != 0) {
            memcpy (&test_active, active_cpus+(map_idx*4), 4);
	    break;
	}
    }

    /*
    ** Finally find the highest bit number in our active set...
    */
#pragma message save
#pragma message disable QUESTCOMPARE
    for (target_cpu = 31; target_cpu >= 0; target_cpu--) {
#pragma message restore
        if ((test_avail & (1<<target_cpu)) != 0) {
            break;
        }
    }
    (void)printf ("Target cpu is %d\n", target_cpu);

    if ((test_active & (1<<target_cpu)) != 0) {
        /*
        ** The target CPU is running.
        */
        stop_cpu (target_cpu);
        start_cpu (target_cpu);
    } else {
        /*
        ** It's not running.
        */
        start_cpu (target_cpu);
        stop_cpu (target_cpu);
    }

    r0_status = sys$clear_system_event (&add_act_cpu_handle,
                                        PSL$C_USER,
                                        0);
    errchk_sig (r0_status);

    r0_status = sys$clear_system_event (&del_act_cpu_handle,
                                        PSL$C_USER,
                                        0);
    errchk_sig (r0_status);

    free (avail_cpus);
    free (active_cpus);
}
