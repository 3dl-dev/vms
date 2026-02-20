
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <utcblkdef.h>
#include <acmedef.h>
#include <jpidef.h>
#include <efndef.h>
#include <descrip.h>
#include <string.h>
#include <smgdef.h>
#include <acmemsgdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <smg$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static int read_pwd (struct dsc$descriptor_s *output_d) {
/*
** Bonus function: how to read a single character or single keypress at a
** time with no echo on VMS.
** This routine uses the SMG$ API, which allows you to do device independant
** terminal I/O on VMS very efficiently (similar to Curses on Un*x).
*/

static char *p;

static int r0_status;
static unsigned int kbid;
static int i;

static unsigned short int key;
static short int max_len;

static char done;
static const char valid[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890$_";

    /*
    ** Create a virtual keyboard to read input.
    */
    r0_status = smg$create_virtual_keyboard (&kbid,
                                             0,
                                             0,
                                             0);
    errchk_sig (r0_status);

    max_len = output_d->dsc$w_length;
    p = output_d->dsc$a_pointer;
    i = 0;
    done = FALSE;
    while (!done) {
        if (i == max_len) {
            done = TRUE;
            continue;
        }
        r0_status = smg$read_keystroke (&kbid,
                                        &key,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0);
        if (r0_status == SMG$_EOF) {
            /*
            ** Handle ctrl-z just like return.
            */
            output_d->dsc$w_length = i;
            done = TRUE;
            continue;
        } else {
            /*
            ** Return unexpected errors to our caller.
            */
            errchk_ret (r0_status);
        }

        if (key == '\r') {
            /*
            ** User hit return.  Update length of output and exit loop
            */
            output_d->dsc$w_length = i;
            done = TRUE;
            continue;
        }
            
        if (strchr (valid, (char)toupper (key)) == NULL) {
            if (key == SMG$K_TRM_DELETE) {
                /*
                ** If the key is delete, then backspace and overwrite our last
                ** star unless we are at position zero, in which case, just
                ** beep.
                */
                if (i > 0) {
                    (void)printf ("\b \b");
                    i--;
                    *p--;
                } else {
                    (void)printf ("\a");
                }
            } else {
                /*
                ** An invalid character for passwords, just beep.  Of course,
                ** in a production program, the code would accept the invalid
                ** character and eventually just return a status indicating
                ** the entire password was invalid (without bothering to call
                ** $hash_password), as telling the possible black hat that the
                ** individual character is invalid is giving them info about
                ** the system...
                */
                (void)printf ("\a");
                if (i > 0) {
                    i--;
                    *p--;
                }
            }
        } else {
            /*
            ** Add the valid character to the output string and print out a
            ** star so the user knows they hit a key.
            */
            (void)printf ("*");
            (void)fflush (stdout);
            *p++ = (char)key;
            i++;
        }
    }
    (void)printf ("\n");

    r0_status = smg$delete_virtual_keyboard (&kbid);
    errchk_sig (r0_status);
    return SS$_NORMAL;
}


/******************************************************************************/
int main (void) {
/*
** This code uses SYS$ACMW () to duplicate the SET PASSWORD command.
*/

static ILE3 acmitms[6];

static ACMESB acmesb;

static int r0_status;
static int jpi_code = JPI$_USERNAME;
static int i;
static int flags = ACMEPWDFLG$M_PASSWORD_1;
static int logon_type = ACME$K_LOCAL;

static char username[12];
static char old_pwd[31];
static char new_pwd1[31];
static char new_pwd2[31];

static struct dsc$descriptor_s username_d = { sizeof (username),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

static struct dsc$descriptor_s old_pwd_d = { sizeof (old_pwd),
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             old_pwd };

static struct dsc$descriptor_s new_pwd1_d = { sizeof (new_pwd1),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              new_pwd1 };

static struct dsc$descriptor_s new_pwd2_d = { sizeof (new_pwd2),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              new_pwd2 };

    /*
    ** Get the username of this process.
    */
    r0_status = lib$getjpi (&jpi_code,
                            0,
                            0,
                            0,
                            &username_d,
                            0);
    errchk_sig (r0_status);

    /*
    ** Fix the descriptor length,
    */
    for (i = 0; i < sizeof (username); i++) {
        if (isspace (username[i])) {
            username_d.dsc$w_length = i;
            break;
        }
    }

    /*
    ** Get old password.
    */
    (void)printf ("Old password: ");
    (void)fflush (stdout);
    r0_status = read_pwd (&old_pwd_d);
    errchk_sig (r0_status);

    /*
    ** Get new password.
    */
    (void)printf ("New password: ");
    (void)fflush (stdout);
    r0_status = read_pwd (&new_pwd1_d);
    errchk_sig (r0_status);

    /*
    ** Get new password again.
    */
    (void)printf ("Verification: ");
    (void)fflush (stdout);
    r0_status = read_pwd (&new_pwd2_d);
    errchk_sig (r0_status);

    /*
    ** Make sure the new password and the verification match.
    */
    if ((new_pwd1_d.dsc$w_length != new_pwd2_d.dsc$w_length) ||
        (memcmp (new_pwd1_d.dsc$a_pointer,
                 new_pwd2_d.dsc$a_pointer,
                 new_pwd1_d.dsc$w_length) != 0)) {
        (void)printf ("New password verification errror\n");
        exit (EXIT_FAILURE);
    }

    /*
    ** Set up the item list for the call to SYS$ACMW ()
    */
    acmitms[0].ile3$w_length = username_d.dsc$w_length;
    acmitms[0].ile3$w_code = ACME$_PRINCIPAL_NAME_IN;
    acmitms[0].ile3$ps_bufaddr = username;
    acmitms[0].ile3$ps_retlen_addr = NULL;
    acmitms[1].ile3$w_length = old_pwd_d.dsc$w_length;
    acmitms[1].ile3$w_code = ACME$_PASSWORD_1;
    acmitms[1].ile3$ps_bufaddr = old_pwd;
    acmitms[1].ile3$ps_retlen_addr = NULL;
    acmitms[2].ile3$w_length = new_pwd1_d.dsc$w_length;
    acmitms[2].ile3$w_code = ACME$_NEW_PASSWORD_1;
    acmitms[2].ile3$ps_bufaddr = new_pwd1;
    acmitms[2].ile3$ps_retlen_addr = NULL;
    acmitms[3].ile3$w_length = 4;
    acmitms[3].ile3$w_code = ACME$_NEW_PASSWORD_FLAGS;
    acmitms[3].ile3$ps_bufaddr = &flags;
    acmitms[3].ile3$ps_retlen_addr = NULL;
    acmitms[4].ile3$w_length = 4;
    acmitms[4].ile3$w_code = ACME$_LOGON_TYPE;
    acmitms[4].ile3$ps_bufaddr = &logon_type;
    acmitms[4].ile3$ps_retlen_addr = NULL;
    acmitms[5].ile3$w_length = 0;
    acmitms[5].ile3$w_code = 0;
    acmitms[5].ile3$ps_bufaddr = NULL;
    acmitms[5].ile3$ps_retlen_addr = NULL;

    /*
    ** Do the password change.
    */
    r0_status = sys$acmw (EFN$C_ENF,
                          ACME$_FC_CHANGE_PASSWORD,
                          0,
                          acmitms,
                          &acmesb,
                          0,
                          0);
    errchk_sig (r0_status);
    switch (acmesb.acmesb$l_status) {
        case SS$_INVPWDLEN :
        case SS$_PWDNOTDIF :
        case SS$_PWDINDIC :
        case SS$_PWDINHIS :
        case SS$_PWDWEAK :
        case ACME$_PWDINHISTORY :
        case ACME$_AUTHFAILURE :
        case ACME$_PWDTOOSHORT :
        case ACME$_INVNEWPWD :
            /*
            ** Possible error returns from change password function.
            */
            r0_status = acmesb.acmesb$l_status;
            break;
        default:
            errchk_sig (acmesb.acmesb$l_status);
            (void)printf ("Successfully changed password\n");
            r0_status = SS$_NORMAL;
            break;
    }
    exit (r0_status);
}
