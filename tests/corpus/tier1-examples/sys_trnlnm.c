
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
#include <psldef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int index;
static unsigned int max_index;
static unsigned int attributes = LNM$M_CASE_BLIND;

static $DESCRIPTOR (table_name_d, "LNM$SYSTEM_TABLE");
static $DESCRIPTOR (logical_name_d, "SYS$SYSROOT");

static char equiv_name[255+1];
static struct dsc$descriptor_s equiv_name_d = { 0,
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                equiv_name };

static ILE3 trnitms[] = {
    4, LNM$_INDEX, &index, NULL,
    0, LNM$_STRING, equiv_name, &equiv_name_d.dsc$w_length,
    4, LNM$_MAX_INDEX, &max_index, NULL,
    0, 0, NULL, NULL };

static unsigned char access_mode = PSL$C_EXEC;


    (void)printf ("Translation of system executive mode logical %-.*s:\n",
                  logical_name_d.dsc$w_length,
                  logical_name_d.dsc$a_pointer);

    index = 0;
    do {
        assert (trnitms[1].ile3$w_code == LNM$_STRING);
        trnitms[1].ile3$w_length = sizeof (equiv_name) - 1;

        r0_status = sys$trnlnm (&attributes,
                                &table_name_d,
                                &logical_name_d,
                                &access_mode,
                                trnitms);
        errchk_sig (r0_status);

        (void)printf ("    %-.*s\n",
                      equiv_name_d.dsc$w_length,
                      equiv_name_d.dsc$a_pointer);
    } while (index++ < max_index);
}
