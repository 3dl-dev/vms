
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

#define trigger_ast(arg) r0_status = sys$dclast (ast_routine, arg, 0); \
                         if (r0_status != SS$_NORMAL) \
                             (void)lib$signal (r0_status);


/******************************************************************************/
static void ast_routine (int arg) {

    (void)printf ("In ast_routine (): message %d\n", arg);
}


/******************************************************************************/
int main (void) {

static int r0_status;

    /*
    ** Enable ASTs (they are enabled by default of course).
    */
    r0_status = sys$setast (1);
    errchk_sig (r0_status);

    /*
    ** Trigger ast_routine as an AST.
    */
    trigger_ast (1);
    (void)printf ("Note this will appear after message 1\n");

    /*
    ** Disable ASTs.
    */
    r0_status = sys$setast (0);
    errchk_sig (r0_status);

    /*
    ** Trigger ast_routine as an AST, but because ASTs are disabled, it won't
    ** be delivered...
    */
    trigger_ast (2);
    (void)printf ("Note this will appear before message 2\n");

    /*
    ** ...until we reenable AST delivery here.
    */
    r0_status = sys$setast (1);
    errchk_sig (r0_status);
}    
