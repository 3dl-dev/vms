
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>

/*
** Include information and item codes for logical name translation.
*/
#include <lnmdef.h>

/*
** Include information about the processor status longword.
*/
#include <psldef.h>

#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static const unsigned int flags = LNM$M_CASE_BLIND;
static int max_index;
static int index = 0; 

static const unsigned char mode = PSL$C_EXEC;

static char equiv[255+1];
static struct dsc$descriptor_s equiv_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           equiv };

static $DESCRIPTOR (logical_d, "SYS$SYSROOT");
static const $DESCRIPTOR (table_d, "LNM$SYSTEM_TABLE");

    do {
        equiv_d.dsc$w_length = sizeof (equiv) - 1;
        r0_status = lib$get_logical (&logical_d,
                                     &equiv_d,
                                     &equiv_d.dsc$w_length,
                                     &table_d,
                                     &max_index,
                                     &index,
                                     &mode,
                                     &flags);
        errchk_sig (r0_status);
        if (index == 0) {
            (void)printf ("\"%-.*s\" = \"%-.*s\" (%-.*s)\n",
                          logical_d.dsc$w_length,
                          logical_d.dsc$a_pointer,
                          equiv_d.dsc$w_length,
                          equiv_d.dsc$a_pointer,
                          table_d.dsc$w_length,
                          table_d.dsc$a_pointer);
        } else {
            (void)printf ("        = \"%-.*s\"\n",
                          equiv_d.dsc$w_length,
                          equiv_d.dsc$a_pointer);
        } 
    } while (index++ < max_index);
}
