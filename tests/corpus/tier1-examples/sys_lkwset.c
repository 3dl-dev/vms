
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <va_rangedef.h>
#include <jpidef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#define ARRAY_SIZE 512 * 2000

static char array[ARRAY_SIZE];


/******************************************************************************/
static unsigned int get_faults (void) {

static int r0_status;
static unsigned int faults;

    r0_status = lib$getjpi (&JPI$_PAGEFLTS,
                            0,
                            0,
                            &faults,
                            0,
                            0);
    errchk_sig (r0_status);
    return faults;
}


/******************************************************************************/
static load_array (void) {

static int r0_status;
static VA_RANGE inadr;
static unsigned int start_faults;
static unsigned int i;
static unsigned int j;
static double f1;
static double f2;

    /*
    ** Purge our entire working set so we can't use the fact that a
    ** previous invokation of this routine faulted all the pages in.
    */
    inadr.va_range$ps_start_va = 0;
    inadr.va_range$ps_end_va = (void *)0x7FFFFFFF;
    r0_status = sys$purgws (&inadr);
    errchk_sig (r0_status);

    /*
    ** Get a "known" sequence of random numbers.  That is, generate the same
    ** sequence each time this routine is called.
    */
    srand (10);

    /*
    ** Get the page faults incurred by this process before run.
    */
    start_faults = get_faults ();

    /*
    ** Randomly fill the array with junk.  That is, select a random index into
    ** the array and stick a value in it.  Randomly selecting an array index
    ** will (should, if we don't lock that pages) cause lots of page faults.
    */
    for (i = 0; i < ARRAY_SIZE * 10; i++) {
        f1 = 1.0 * rand ();
        f2 = 1.0 * rand ();
        j = (int)(f1 / f2 * (double)ARRAY_SIZE);
        j %= ARRAY_SIZE;
        array[j] = (char)i;
    }

    /*
    ** Get page faults after run and print the difference.
    */
    (void)printf ("Faults: %d\n", get_faults() - start_faults);
}


/******************************************************************************/
int main (void) {
/*
** Demonstrate how to lock data into the working set.  This program was
** tested with the WSQUOTA for the account set to 2000.
*/
 
static int r0_status;
static VA_RANGE inadr;
static VA_RANGE outadr;

    /*
    ** Get the start and end address of the array.
    */
    inadr.va_range$ps_start_va = &array[0];
    inadr.va_range$ps_end_va = (char *)inadr.va_range$ps_start_va +
                               sizeof (array);

    /*
    ** Lock the array into the working set.
    */
    r0_status = sys$lkwset (&inadr,
                            &outadr,
                            0);
    if (r0_status == SS$_LKWSETFUL) {
        (void)printf ("You exceeded the number of pages you can lock in your\n"
                      "working set.  Try bumping your WSQUOTA up a bit and\n"
                      "logging out and back in, then running this program\n"
                      "again.\n");
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("Run with the array locked in the WS:\n");
    load_array ();

    /*
    ** Unlock the pages of the working set that map the array.
    */
    r0_status = sys$ulwset (&outadr,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Run with the array not locked:\n");
    load_array ();

    (void)printf ("Hopefully, there is a significant difference in the number"
                  " of pagefaults.\n");
}
