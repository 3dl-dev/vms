
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <assert.h>
#include <issdef.h>
#include <rms.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Requires the IMPERSONATE privilege.
*/

static int r0_status;
static unsigned int system_persona_id;
static unsigned int default_persona_id;
static unsigned int copy_persona_id;
static unsigned int prev_persona_id;
static unsigned int zero = 0;

static ILE3 itmlst[] = { 4, ISS$_NOAUDIT, &zero, NULL,
                         0, 0, NULL, NULL };

static struct {
    unsigned int length;
    void *addr;
} usrpro_d;

static $DESCRIPTOR (system_d, "SYSTEM");
static $DESCRIPTOR (default_d, "DEFAULT");
static char default_ok = TRUE;

    /*
    ** First we have to get the length that the usrpro data structure will
    ** be when returned.
    */
    r0_status = sys$create_user_profile (&system_d,
                                         0,
                                         0,
                                         0,
                                         &usrpro_d.length,
                                         0);
    errchk_sig (r0_status);

    usrpro_d.addr = malloc (usrpro_d.length);
    assert (usrpro_d.addr != NULL);

    /*
    ** Now call sys$create_user_profile again to actually get the user profile.
    */
    r0_status = sys$create_user_profile (&system_d,
                                         0,
                                         0,
                                         usrpro_d.addr,
                                         &usrpro_d.length,
                                         0);
    errchk_sig (r0_status);

    /*
    ** Create a persona for the SYSTEM username.  Note this call uses the
    ** user profile we just created.
    */
    r0_status = sys$persona_create (&system_persona_id,
                                    0,
                                    0,
                                    &usrpro_d,
                                    0);
    if (r0_status == SS$_NOIMPERSONATE) {
        (void)printf ("This program requires the IMPERSONATE privilege.\n");
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    /*
    ** Create a persona for the DEFAULT username.  Note this call users the
    ** username arg and not the usrpro arg.
    */
    r0_status = sys$persona_create (&default_persona_id,
                                    &default_d,
                                    0,
                                    0,
                                    0);
    if ((r0_status == RMS$_RNF) ||
        (r0_status == SS$_USERDISABLED)) {
        (void)printf ("The %-.*s account is either missing or disabled.\n"
                      "Some functionality won't be demonstrated.\n",
                      default_d.dsc$w_length,
                      default_d.dsc$a_pointer);
        default_ok = FALSE;
    } else {
        errchk_sig (r0_status);
    }

    /*
    ** Clone the system persona so we can...
    */
    r0_status = sys$persona_clone (&copy_persona_id,
                                   &system_persona_id);
    errchk_sig (r0_status);

    /*
    ** ...modify it.
    */
    r0_status = sys$persona_modify (&copy_persona_id,
                                    &itmlst);
    errchk_sig (r0_status);
    
    /*
    ** Assume the persona of SYSTEM.
    */
    r0_status = sys$persona_assume (&system_persona_id,
                                    0,
                                    &prev_persona_id,
                                    0);
    errchk_sig (r0_status);

    /*
    ** The previous persona ID (in this case our "natural" persona) is returned.
    */
    assert (prev_persona_id == ISS$C_ID_NATURAL);

    if (default_ok) {
        /*
        ** Assume the DEFAULT persona.
        */
        r0_status = sys$persona_assume (&default_persona_id,
                                        0,
                                        0,
                                        0);
        errchk_sig (r0_status);
    }

    /*
    ** Resume our "natural" persona.
    */
    r0_status = sys$persona_assume (&prev_persona_id,
                                    0,
                                    0,
                                    0);
    errchk_sig (r0_status);

    if (default_ok) {
        r0_status = sys$persona_delete (&default_persona_id);
        errchk_sig (r0_status);
    }

    r0_status = sys$persona_delete (&copy_persona_id);
    errchk_sig (r0_status);

    r0_status = sys$persona_delete (&system_persona_id);
    errchk_sig (r0_status);

    free (usrpro_d.addr);
}
