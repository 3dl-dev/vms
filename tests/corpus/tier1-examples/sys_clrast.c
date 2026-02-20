
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

/*
** Weird, the version of starlet.h I have on OpenVMS 8.2 does not seem to have
** a prototype for sys$clrast ().  Define one if not already defined.
*/
#ifndef sys$clrast
#define sys$clrast SYS$CLRAST
int sys$clrast (void);
#endif


/******************************************************************************/
static void ast_two (int efn) {

static int r0_status;

    (void)printf ("In ast_two\n");

    /*
    ** Set the event flag.
    */
    r0_status = sys$setef (efn);
    errchk_sig (r0_status);

    (void)printf ("Out ast_two\n");
}


/******************************************************************************/
static void ast_one (void) {

static int r0_status;

    /*
    ** Show where we are.
    */
    (void)printf ("In ast_one\n");

    /*
    ** Queue another user mode AST.  In normal circumstances, ast_two would not
    ** be delivered until after we return from ast_one.  However...
    */
    r0_status = sys$dclast (ast_two, 2, 0);
    errchk_sig (r0_status);

    /*
    ** Clear an event flag.
    */
    r0_status = sys$clref (2);
    errchk_sig (r0_status);

    /*
    ** Say that we are no longer interested in AST synchonization (!!!)
    */
    sys$clrast ();

    /*
    ** Wait for the event flag to set.  Note that under normal circumstances
    ** this would be a dumb idea.
    */
    r0_status = sys$waitfr (2);
    errchk_sig (r0_status);

    /*
    ** Say where we are.
    */
    (void)printf ("Out ast_one\n");
}


/******************************************************************************/
int main (void) {
/*
** Danger, Will Robinson!  This code employs SYS$CLRAST, which says to the
** operating system "I am no longer executing as an AST".
**
** In the normal course of events (ie, if you don't call SYS$CLRAST) the
** operating system queues ASTs of the same mode so that they occur one after
** the other.  This is an inherent synchronization method within the operating
** system, and usually should not be thwarted.  (Of course, inner mode
** ASTs can interrupt outer mode ones: that what it's all about.  But under
** normal circumstances, a user mode AST cannot interrupt a user mode AST
** already executing.  SYS$CLRAST allows this to happen.  HP strongly
** discourages the use of this system service, as it's *definately* easy
** to shoot yourself if the foot by calling it).
*/

static int r0_status;

    /*
    ** Get a user mode AST running...
    */
    r0_status = sys$dclast (ast_one, 0, 0);
    errchk_sig (r0_status);
}
