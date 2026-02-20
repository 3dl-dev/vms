
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <chpdef.h>
#include <armdef.h>
#include <jpidef.h>
#include <assert.h>
#include <descrip.h>
#include <ossdef.h>
#include <ctype.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Demo $chkpro () against SYS$SYSTEM:TYPE.EXE.  Note that your results may
** vary if you have privileges enabled.  For a normal unprivileged user,
** they should be able to read but not write the file.
*/

static int r0_status;
static unsigned int context;
static unsigned int access;
static unsigned int flags;
static unsigned int acl_length;
static unsigned int protection;
static unsigned int owner;

static ILE3 chpitms[] = { 4, CHP$_ACCESS, &access, NULL, 
                          4, CHP$_FLAGS, &flags, NULL,
                          4, CHP$_OWNER, &owner, NULL,
                          4, CHP$_PROT, &protection, NULL,
                          0, CHP$_ACL, NULL, NULL,
                          0, 0, NULL, NULL };
static ILE3 gsitms1[] = { 4, OSS$_ACL_LENGTH, &acl_length, NULL,
                          4, OSS$_OWNER, &owner, NULL,
                          4, OSS$_PROTECTION, &protection, NULL,
                          0, 0, NULL, NULL };
static ILE3 gsitms2[] = { 0, OSS$_ACL_READ, NULL, NULL,
                          0, 0, NULL, NULL };

static struct {
    unsigned int length;
    void *addr;
} usrpro_d;

static char myusername[12];
static struct dsc$descriptor_s myusername_d = { sizeof (myusername),
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                myusername };

static $DESCRIPTOR (file_d, "SYS$SYSTEM:TYPE.EXE");
static $DESCRIPTOR (class_d, "FILE");


    /*
    ** Get my username.
    */
    r0_status = lib$getjpi (&JPI$_USERNAME,
                            0,
                            0,
                            0,
                            &myusername_d,
                            &myusername_d.dsc$w_length);
    errchk_sig (r0_status);

    for (int i = myusername_d.dsc$w_length - 1; i >= 0; i--) {
        if (!isspace (myusername[i])) {
            myusername_d.dsc$w_length = i + 1;
            break;
        }
    }

    /*
    ** First we have to get the length that the usrpro data structure will
    ** be when returned.
    */
    r0_status = sys$create_user_profile (&myusername_d,
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
    r0_status = sys$create_user_profile (&myusername_d,
                                         0,
                                         0,
                                         usrpro_d.addr,
                                         &usrpro_d.length,
                                         0);
    errchk_sig (r0_status);

    /*
    ** Next, we have to retrieve the security information for the file.
    ** The first call will retrieve the length of the ACL (if any), the
    ** owner, and the protection.
    */
    r0_status = sys$get_security (&class_d,
                                  &file_d,
                                  0,
                                  0,
                                  gsitms1,
                                  &context,
                                  0);
    errchk_sig (r0_status);

    if (acl_length != 0) {
        /*
        ** The file had an ACL, retrieve it and add it's address and length
        ** to the itemlist for $chkpro ().
        */
        assert (gsitms2[0].ile3$w_code == OSS$_ACL_READ);
        gsitms2[0].ile3$w_length = acl_length;
        gsitms2[0].ile3$ps_bufaddr = malloc (acl_length);
        r0_status = sys$get_security (0,
                                      0,
                                      0,
                                      OSS$M_RELCTX,
                                      gsitms2,
                                      &context,
                                      0);
        errchk_sig (r0_status);
        assert (chpitms[4].ile3$w_code == CHP$_ACL);
        chpitms[4].ile3$w_length = acl_length;
        chpitms[4].ile3$ps_bufaddr = gsitms2[0].ile3$ps_bufaddr;
    } else {
        /*
        ** There was no ACL for this file.  We must call $get_security to
        ** release the context, and we need to make sure we wipe out the
        ** ACL item in the itemlist for $chkpro ().
        */
        r0_status = sys$get_security (0,
                                      0,
                                      0,
                                      OSS$M_RELCTX,
                                      0,
                                      &context,
                                      0);
        errchk_sig (r0_status);
        assert (chpitms[4].ile3$w_code == CHP$_ACL);
        chpitms[4].ile3$w_code = 0;
    }

    /*
    ** Test file for read access.
    */
    access = ARM$M_READ;
    flags = CHP$M_OBSERVE;
    r0_status = sys$chkpro (chpitms,
                            0,
                            &usrpro_d);
    if (r0_status == SS$_NOCALLPRIV) {
        (void)printf ("No access to SYSUAF - "
                      "try running from a privileged account\n");
	exit (EXIT_FAILURE);
    }
    (void)printf ("Read access to %-.*s ",
                  file_d.dsc$w_length,
                  file_d.dsc$a_pointer);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("NOT");
    } else {
        errchk_sig (r0_status);
        (void)printf ("IS");
    }
    (void)printf (" permitted\n");

    /*
    ** Test file for write access.
    */
    access = ARM$M_WRITE;
    flags = CHP$M_ALTER;
    r0_status = sys$chkpro (chpitms,
                            0,
                            &usrpro_d);
    if (r0_status == SS$_NOCALLPRIV) {
        (void)printf ("No access to SYSUAF - "
                      "try running from a privileged account\n");
	exit (EXIT_FAILURE);
    }
    (void)printf ("Write access to %-.*s ",
                  file_d.dsc$w_length,
                  file_d.dsc$a_pointer);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("NOT");
    } else {
        errchk_sig (r0_status);
        (void)printf ("IS");
    }
    (void)printf (" permitted\n");

    (void)free (usrpro_d.addr);
    if (acl_length != 0) {
        (void)free (gsitms2[0].ile3$ps_bufaddr);
    }
}
