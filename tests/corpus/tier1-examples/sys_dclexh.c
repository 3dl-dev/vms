
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

struct eh {
    void *forward_link;
    void *exit_handler_addr;
    unsigned int arg_count;
    unsigned int *cond_val_addr;
    unsigned int user_arg;
    unsigned int cond_value;
};


/******************************************************************************/
static int exit_handler_1 (unsigned int *condition_value,
                           unsigned int user_value) {
/*
** This is an exit handler.  It gets control after the main program exits.
** For further information, see the "OpenVMS Programming Concepts Manual".
*/

    (void)printf ("In exit_handler_1 ()\n");
    (void)printf ("condition_value = %d\n"
                  "user_value = %d\n",
                  *condition_value,
                  user_value);
    return *condition_value;
}


/******************************************************************************/
static int exit_handler_2 (unsigned int *condition_value,
                           unsigned int user_value) {
/*
** This is another exit handler.
*/

    (void)printf ("In exit_handler_2 ()\n");
    (void)printf ("condition_value = %d\n"
                  "user_value = %d\n",
                  *condition_value,
                  user_value);
    return *condition_value;
}


/******************************************************************************/
int main (void) {

static int r0_status;

static struct eh exit_handler_block_1 = { NULL,
                                          exit_handler_1,
                                          2,
                                          &exit_handler_block_1.cond_value,
                                          111,
                                          0 };

static struct eh exit_handler_block_2 = { NULL,
                                          exit_handler_2,
                                          2,
                                          &exit_handler_block_2.cond_value,
                                          222,
                                          0 };

    /*
    ** Declare an exit handler.
    */
    r0_status = sys$dclexh (&exit_handler_block_2);
    errchk_sig (r0_status);

    /*
    ** Declare another exit handler.
    */
    r0_status = sys$dclexh (&exit_handler_block_1);
    errchk_sig (r0_status);

    /*
    ** If the program was to exit at this point, both exit handlers would run,
    ** one after the other.  First 1, then 2.
    */

    /*
    ** Remove exit handler 2
    */
    r0_status = sys$canexh (&exit_handler_block_2);
    errchk_sig (r0_status);

    /*
    ** Exit the program.
    */
    r0_status = sys$exit (SS$_NORMAL);
}
