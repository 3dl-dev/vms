
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This one requires that you have write access to the rights list.  So you
** need SYSPRV or BYPASS enabled.  The code adds an identifier to your system
** grants it to the username running this program, revokes it from the user,
** and deletes the identifier.  In addition, the program adds the identifier
** the the process identifier list for this process and removes it.
*/

#define ID_VALUE 0x6666

static IOSB iosb;

static GENERIC_64 id;
static GENERIC_64 uic;

static int r0_status;

static ILE3 jpiitms[] = { 4, JPI$_UIC, &uic, NULL,
                          0, 0, NULL, NULL };

static $DESCRIPTOR (id_d, "SYS_IDENT_TESTJFD");

    /*
    ** First check to see that the system doesn't have an identifier
    ** named the same as ours.
    */
    r0_status = sys$asctoid (&id_d,
                             &id.gen64$l_longword[0],
                             0);
    if (r0_status == SS$_NORMAL) {
        (void)printf ("You already have an identifier on your system "
                      "called %-.*s.\nChange this program's "
                      "id_d variable to a different value and recompile.\n",
                      id_d.dsc$w_length,
                      id_d.dsc$a_pointer);
        exit (EXIT_SUCCESS);
    } else {
        if (r0_status != SS$_NOSUCHID) {
            errchk_sig (r0_status);
        }
    }

    /*
    ** Add our identifier to the rights database.
    */
    r0_status = sys$add_ident (&id_d,
                               ID_VALUE,
                               0,
                               &id.gen64$l_longword[0]);
    if (r0_status == SS$_DUPIDENT) {
        (void)printf ("An identifier with value %X08 already exists\n"
              "To run, you can modify the code to set a value of the\n"
              "#define value ID_VALUE to an unused value and recompile.\n",
              ID_VALUE);
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("Identifier %-.*s added to the system with value %%X%08x.\n",
                  id_d.dsc$w_length,
                  id_d.dsc$a_pointer,
                  id.gen64$l_longword[0]);

    /*
    ** Find our UIC.
    */
    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Grant our username the newly created identifier.
    */
    r0_status = sys$add_holder (id.gen64$l_longword[0],
                                &uic,
                                0);
    errchk_sig (r0_status);

    (void)printf ("Identifier granted to UIC.\n");

    /*
    ** On the fly, grant the identifier to our process.
    */
    r0_status = sys$grantid (0,
                             0,
                             &id,
                             0,
                             0,
			     0);
    errchk_sig (r0_status);

    (void)printf ("Identifier granted to this process.\n");

    /*
    ** Remove identifier from our process.
    */
    r0_status = sys$revokid (0,
                             0,
                             &id,
                             0,
                             0,
			     0);
    errchk_sig (r0_status);

    (void)printf ("Identifier revoked from this process.\n");

    /*
    ** Delete our UIC as a holder of the identifier in the database.
    */
    r0_status = sys$rem_holder (id.gen64$l_longword[0],
                                &uic);
    errchk_sig (r0_status);

    (void)printf ("Identifier revoked from UIC.\n");

    /*
    ** Delete the identifier.
    */
    r0_status = sys$rem_ident (id.gen64$l_longword[0]);
    errchk_sig (r0_status);

    (void)printf ("Identifier deleted.\n");
}
