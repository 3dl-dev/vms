
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <uaidef.h>
#include <jpidef.h>
#include <efndef.h>
#include <descrip.h>
#include <string.h>
#include <smgdef.h>
#include <iosbdef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <smg$routines.h>
#include <starlet.h>

#include "errchk.h"

struct itm3 {
    unsigned short int len;
    unsigned short int code;
    void *bufadr;
    void *buflen;
};


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
            
        key = toupper (key);
        if (strchr (valid, (char)key) == NULL) {
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

static IOSB iosb;

static GENERIC_64 pwd;
static GENERIC_64 hash_result;
static int r0_status;

static unsigned short int salt;

static char algorithm;
static char username[12];
static char password[31];

static struct itm3 jpiitms[] =
        { sizeof (username), JPI$_USERNAME, username, NULL,
          0, 0, NULL, NULL };

static struct itm3 uaiitms[] = 
        { sizeof (salt), UAI$_SALT, &salt, NULL,
          sizeof (algorithm), UAI$_ENCRYPT, &algorithm, NULL,
          sizeof (pwd), UAI$_PWD, &pwd, NULL,
          0, 0, NULL, NULL };

static struct dsc$descriptor_s username_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

static struct dsc$descriptor_s password_d = { sizeof (password),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              password };
    /*
    ** Get our username.
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
    ** Fix the descriptor.
    */
    for (int i = 0; i < sizeof (username); i++) {
        if (isspace (username[i])) {
            username_d.dsc$w_length = i;
            break;
        }
    }

    /*
    ** Get the salt, the encryption algorithm, and the hashed password for
    ** our account.  Note you don't need privilege to look up your own info,
    ** but if you wish to modify this code to check the password for other
    ** usernames, you need either grpprv or sysprv (or read access to SYSUAF).
    */
    r0_status = sys$getuai (0,
                            0,
                            &username_d,
                            uaiitms,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Enter password for %-.*s: ",
                  username_d.dsc$w_length,
                  username_d.dsc$a_pointer);
    (void)fflush (stdout);

    /*
    ** Read the password with no echo, validating the charaters as we go.
    */
    r0_status = read_pwd (&password_d);
    errchk_sig (r0_status);
    
    /*
    ** Hash the password the user just typed using the salt and encryption
    ** algorithm we got via $GETUAI previously.
    */
    r0_status = sys$hash_password (&password_d,
                                   algorithm,
                                   salt,
                                   &username_d,
                                   &hash_result);
    errchk_sig (r0_status);

    /*
    ** See if the new hash matches the one we got via $GETUAI
    */
    if (pwd.gen64$q_quadword == hash_result.gen64$q_quadword) {
        (void)printf ("Correct password entered\n");
    } else {
        (void)printf ("Incorrect password entered\n");
    }
}    
