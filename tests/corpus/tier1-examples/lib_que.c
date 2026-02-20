
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"

#pragma environment save
#pragma nomember_alignment quadword 
struct elem {
    void *flink;
    void *blink;
    unsigned int data;
};
#pragma environment restore

#define MAX_ELEMENTS 20


/******************************************************************************/
int main (void) {

/*
** Demo calling lib$insqhi (), lib$insqti (), lib$remqti (), and lib$remqhi ().
** The "i" on the end of these functions means "interlocked".  In a system
** that is potentially accessing the queue from multiple proccesses, you
** would check for return status LIB$_SECINTFAI, but in this demo, it's
** not needed.
*/

static __int64 list_head = 0;
static struct elem *p;

static int r0_status;
static int i;
static unsigned int retries = 1;

static char finished = FALSE;

    for (i = 1; i <= MAX_ELEMENTS; i++) {
        p = malloc (sizeof (struct elem));
        if (p == NULL) {
            (void)printf ("Could not allocate element!\n");
            (void)lib$signal (SS$_INSFMEM);
        }
        p->data = i;
        if (i & 1) {
            r0_status = lib$insqhi ((unsigned int *)p,
                                    &list_head,
                                    &retries);
        } else {
            r0_status = lib$insqti ((unsigned int *)p,
                                    &list_head,
                                    &retries);
        }
        errchk_sig (r0_status);
    }

    i = MAX_ELEMENTS;
    while (!finished) {
        if (i-- & 1) {
            r0_status = lib$remqti (&list_head,
                                    &p,
                                    &retries);
        } else {
            r0_status = lib$remqhi (&list_head,
                                    &p,
                                    &retries);
        }
        if ((r0_status == SS$_NORMAL) ||
            (r0_status == LIB$_ONEENTQUE)) {
            (void)printf ("got element %u from list\n",
                          p->data);
            free (p);
        } else {
            if (r0_status == LIB$_QUEWASEMP) {
                finished = TRUE;
            } else {
                errchk_sig (r0_status);
            }
        }
    }
}
