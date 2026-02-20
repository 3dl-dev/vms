
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <armdef.h>
#include <ctype.h>
#include <chpdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <acldef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb;

static int r0_status;
static unsigned int mask;
static unsigned int flags;
static unsigned int object = ACL$C_FILE;

static short int i;

static char username[12];

static ILE3 caitms[] = { 4, CHP$_ACCESS, &mask,  0,
                         4, CHP$_FLAGS, &flags, 0,
                         0, 0, NULL, NULL };

static ILE3 jpiitms[] = { 12, JPI$_USERNAME, &username, NULL,
                          0, 0, NULL, NULL };

static struct dsc$descriptor_s username_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

static $DESCRIPTOR (filename_d, "SYS$SYSDEVICE:[000000]000000.DIR");

    /*
    ** Get the username.
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
    ** The username is returned as a space filled 12 byte string.  Find the
    ** first non-space.
    */
    for (i = 0; i < sizeof (username); i++) {
        if (isspace (username[i])) {
            break;
        }
    }

    /*
    ** Set up the username string descriptor
    */
    username_d.dsc$w_length = i;

    mask = ARM$M_READ;
    flags = CHP$M_OBSERVE;
    r0_status = sys$check_access (&object,
                                  &filename_d,
                                  &username_d,
                                  &caitms,
				  0,
				  0,
				  0,
				  0);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("No read access to file %-.*s\n",
                      filename_d.dsc$w_length,
                      filename_d.dsc$a_pointer);
	exit (EXIT_FAILURE);
    } else if (r0_status == SS$_NOCALLPRIV) {
	(void)printf ("No access to SYSUAF - try running from a privileged "
		      "account\n");
	exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("Read access available\n");
}
