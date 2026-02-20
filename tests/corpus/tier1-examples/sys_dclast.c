
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


/******************************************************************************/
static void ast_routine (void) {
/*
** ASTs, or asynchronous system traps, are VMSs way of doing "event"
** programming.  An AST can be delivered to a process at any time.
** It interrupts the normal flow of program execution, executes to
** completion, and then returns program control to where it  interrupted
** the program.
**
** ASTs can occur at all processor modes although only one AST can be
** active for a specific mode at any one time.  Inner modes can interrupt
** outer modes, but not vica versa.  In other words, if a user mode AST
** is executing, it can be interrupted by a supervisor, executive, or
** kernel mode AST.
**
** Don't read my inadequate writeup.  Read the "Programming Concepts"
** manual.  Go on!  Why are you still here?
**
** This routine demos a user mode AST being declared by sys$dclast (),
** which is particularly useful for executing an AST when you want it,
** rather than when the system wants it ;-)
*/

    (void)printf ("In the AST. Wasn't that exciting?\n");
}


/******************************************************************************/
int main (void) {

static int r0_status;

    r0_status = sys$dclast (ast_routine,
                            0,
                            0);
    errchk_sig (r0_status);
}
