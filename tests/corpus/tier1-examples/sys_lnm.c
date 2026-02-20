
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lnmdef.h>
#include <string.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int attributes = LNM$M_CREATE_IF;

static unsigned short int promask = 0xff00;

static $DESCRIPTOR (process_directory_d, "LNM$PROCESS_DIRECTORY");
static $DESCRIPTOR (table_name_d, "SYS_LNM_DEMO_TABLE");
static $DESCRIPTOR (logical_name_d, "SYS_LNM_DEMO_NAME");

static char equiv_name[] = { "VALUE" };

static char resnam[31+1];
static struct dsc$descriptor_s resnam_d = { sizeof (resnam) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            resnam };

static ILE3 clnmitms[] = { 0, LNM$_STRING, equiv_name, NULL,
                           0, 0, NULL, NULL };

    /*
    ** Create a logical name table.  Make it a child of the process directory.
    */
    r0_status = sys$crelnt (&attributes,
                            &resnam_d,
                            &resnam_d.dsc$w_length,
                            0,
                            &promask,
                            &table_name_d,
                            &process_directory_d,
                            0);
    errchk_sig (r0_status);

    /*
    ** Fill in the length of the equivilence name.
    */
    assert (clnmitms[0].ile3$w_code == LNM$_STRING);
    clnmitms[0].ile3$w_length = strlen (equiv_name);

    /*
    ** Create a logical name in our new table.
    */
    r0_status = sys$crelnm (0,
                            &table_name_d,
                            &logical_name_d,
                            0,
                            clnmitms);
    errchk_sig (r0_status);

    /*
    ** Delete the logical name.
    */
    r0_status = sys$dellnm (&table_name_d,
                            &logical_name_d,
                            0);
    errchk_sig (r0_status);

    /*
    ** Because the logical name table is represented by a logical name in
    ** the process directory, we can just delete the table by deleting its
    ** logical name representation.
    */
    r0_status = sys$dellnm (&process_directory_d,
                            &table_name_d,
                            0);
    errchk_sig (r0_status);
}
