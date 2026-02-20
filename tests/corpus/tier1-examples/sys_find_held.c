
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <kgbdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <string.h>
#include <iosbdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static GENERIC_64 holder;

static IOSB iosb;

static int r0_status;
static unsigned int context;
static unsigned int attribs;
static unsigned int id;
static unsigned int ident_cnt = 0;

static char ascid[31+1];
static char finished;

static ILE3 jpiitms[] = { 4, JPI$_UIC, &holder, NULL,
                          0, 0, NULL, NULL };

static struct dsc$descriptor ascid_d = { sizeof (ascid) - 1,
                                         DSC$K_DTYPE_T,
                                         DSC$K_CLASS_S,
                                         ascid };

    /*
    ** Get the UIC of this process.
    */
    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             &jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);
                             
    /*
    ** Context and flag for loop...
    */
    context = 0;
    finished = FALSE;

    while (!finished) {
        /*
        ** Find all identifiers for this UIC
        */
        r0_status = sys$find_held (&holder,
                                   &id,
                                   &attribs,
                                   &context);
        if (r0_status == SS$_NOSUCHID) {
            /*
            ** If the call returned no such id, then we have finished.
            ** Set the finished flag and continue
            */
            finished = TRUE;
            continue;
        } else {
            errchk_sig (r0_status);
            ident_cnt++;
        }

        /*
        ** Convert the identifier to ascii text
        */
        ascid_d.dsc$w_length = sizeof (ascid) - 1;
        r0_status = sys$idtoasc (id,
                                 &ascid_d.dsc$w_length,
                                 &ascid_d,
                                 0,
                                 &attribs,
                                 0);
        errchk_sig (r0_status);

        /*
        ** Write it out and write out the resource attribute string if the
        ** id has that attribute
        */
        ascid[ascid_d.dsc$w_length] = '\0';
        (void)printf ("%-31.31s", ascid);
        if (attribs & KGB$M_RESOURCE) {
            (void)printf ("        resource");
        }
        (void)printf ("\n");
    }

    /*
    ** Demonstrate a call to sys$find_holder ().  Find the first holder
    ** of the last identifier we got from the previous loop.  We must reset
    ** context to zero...
    */
    if (ident_cnt > 0) {
        context = 0;
        do {
            r0_status = sys$find_holder (id,
                                         &holder,
                                         &attribs,
                                         &context);
            if (r0_status != SS$_NOSUCHID) {
                errchk_sig (r0_status);
                r0_status = sys$finish_rdb (&context);
                errchk_sig (r0_status);
            }
        } while (!finished);
    }
}
