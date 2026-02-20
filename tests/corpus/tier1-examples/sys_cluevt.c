
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <cluevtdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

struct ast_args {
    unsigned int add_ast_handle[2];
    unsigned int rem_ast_handle[2];
};

/*
** Forward declarations.
*/
static void node_add_ast (struct ast_args *);
static void node_rem_ast (struct ast_args *);


/******************************************************************************/
static void arm_add_ast (struct ast_args *args) {

static int r0_status;

    r0_status = sys$setcluevt (CLUEVT$C_ADD,
                               node_add_ast,
                               (int)args,
                               0,
                               args->add_ast_handle);
    errchk_sig (r0_status);
}


/******************************************************************************/
static void arm_rem_ast (struct ast_args *args) {

static int r0_status;

    r0_status = sys$setcluevt (CLUEVT$C_REMOVE,
                               node_rem_ast,
                               (int)args,
                               0,
                               args->rem_ast_handle);
    errchk_sig (r0_status);
}


/******************************************************************************/
static void node_add_ast (struct ast_args *args) {

    (void)printf ("A node joined the cluster\n");

    arm_add_ast (args);
}


/******************************************************************************/
static void node_rem_ast (struct ast_args *args) {

    (void)printf ("A node left the cluster\n");

    arm_rem_ast (args);
}


/******************************************************************************/
int main (void) {

static int r0_status;
static struct ast_args args;

    arm_add_ast (&args);
    arm_rem_ast (&args);

    r0_status = sys$tstcluevt (args.add_ast_handle,
                               0,
                               0);
    errchk_sig (r0_status);

    r0_status = sys$tstcluevt (0,
                               0,
                               CLUEVT$C_REMOVE);
    errchk_sig (r0_status);

    r0_status = sys$clrcluevt (args.add_ast_handle,
                               0,
                               0);
    errchk_sig (r0_status);

    r0_status = sys$clrcluevt (0,
                               0,
                               CLUEVT$C_REMOVE);
    errchk_sig (r0_status);

}
