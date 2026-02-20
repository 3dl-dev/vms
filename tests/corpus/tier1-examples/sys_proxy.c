
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <efndef.h>
#include <jpidef.h>
#include <syidef.h>
#include <prxdef.h>
#include <secsrvmsgdef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program will add a proxy record, display proxy information for
** the current user, demo a call to verify proxy info, and then delete the
** proxy record.
**
** You need SYSPRV privilege to run this program.
**
** One small caveat.  If for some reason you have a default proxy from
** the node you are currently executing on for the username you run this
** program with, this program will overwrite the default proxy.
**
** I can't see why you would have something like localnode::user user/default
** but if you do, it _WILL_BE_OVERWRITTEN_.
**
** If you don't know what I'm talking about, don't run the program.
**
** Standard disclaimers apply.  See http://www.eight-cubed.com/disclaimer.html
**
** By downloading this code, you explicitly agree to that license.
**
*/

static IOSB iosb;

static int r0_status;
static int i;

static unsigned short int buffer_sizes[4];

static char delete = TRUE;

static char username[12];
static char nodename[15];
static char proxy_node[15];
static char proxy_user[32];
static char default_user[32];
static char local_user[32];

static struct dsc$descriptor_s username_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };
static struct dsc$descriptor_s nodename_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              nodename };
static struct dsc$descriptor_s proxy_node_d = { sizeof (proxy_node),
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                proxy_node };
static struct dsc$descriptor_s proxy_user_d = { sizeof (proxy_user),
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                proxy_user };
static struct dsc$descriptor_s default_user_d = { sizeof (default_user),
                                                  DSC$K_DTYPE_T,
                                                  DSC$K_CLASS_S,
                                                  default_user };
static struct dsc$descriptor_s local_user_d = { sizeof (local_user),
                                                DSC$K_DTYPE_T,
                                                DSC$K_CLASS_S,
                                                local_user };

static struct {
    unsigned short int length;
    unsigned short int unused;
    char username[32];
} local_users[16];

static ILE3 jpiitms[] = { 12, JPI$_USERNAME, username, NULL,
                           0, 0, NULL, NULL };
static ILE3 syiitms[] = { 15, SYI$_NODENAME, nodename, &nodename_d.dsc$w_length,
                           0, 0, NULL, NULL };


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
    ** JPI$_USERNAME always returns 12 characters space filled.  Get
    ** the real length.
    */
    for (i = 0; i < 12; i++) {
        if (username[i] == 0x20) {
            break;
        }
    }
    username_d.dsc$w_length = i;

    /*
    ** Get the name of the node we are executing on.
    */
    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             syiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    /*
    ** Add a proxy record from our_node::our_username to our_username and
    ** mark it a default proxy.
    */
    r0_status = sys$add_proxy (&nodename_d,
                               &username_d,
                               &username_d,
                               PRX$M_DEFAULT);
    switch (r0_status) {
        case SS$_NOSYSPRV :
            (void)printf ("You need SYSPRV privilege to run this program!\n");
            exit (SS$_NOSYSPRV);
            break;
        case SECSRV$_DUPLICATEUSER :
            (void)printf ("Proxy for %-.*s::%-.*s to %-.*s already exists\n",
                          nodename_d.dsc$w_length,
                          nodename_d.dsc$a_pointer,
                          username_d.dsc$w_length,
                          username_d.dsc$a_pointer,
                          username_d.dsc$w_length,
                          username_d.dsc$a_pointer);
            delete = FALSE;
            break;
        default :
            errchk_sig (r0_status);
            (void)printf ("Proxy record added\n");
            break;
    }

    /*
    ** Display any proxy information for this node/user combo.
    */
    r0_status = sys$display_proxy (&nodename_d,
                                   &username_d,
                                   buffer_sizes,
                                   &proxy_node_d,
                                   &proxy_user_d,
                                   &default_user_d,
                                   (unsigned int *)&local_users[0],
                                   PRX$M_EXACT,
                                   0);
    errchk_sig (r0_status);

    (void)printf ("A proxy exists for remote user "
                  "%-.*s::%-.*s to local users:\n",
                  buffer_sizes[1],
                  proxy_node_d.dsc$a_pointer,
                  buffer_sizes[0],
                  proxy_user_d.dsc$a_pointer);

    for (i = 0; i < buffer_sizes[2]; i++) {
        if (local_users[i].length != 0) {
            (void)printf ("    %-.*s\n",
                          local_users[i].length,
                          local_users[i].username);
        }
    }

    if (buffer_sizes[3] != 0) {
        (void)printf ("    %-.*s (default)\n",
                      buffer_sizes[3],
                      default_user_d.dsc$a_pointer);
    }

    r0_status = sys$verify_proxy (&nodename_d,
                                  &username_d,
                                  0,
                                  &local_user_d,
                                  &local_user_d.dsc$w_length,
                                  0);
    errchk_sig (r0_status);

    (void)printf ("Confirming the default is %-.*s\n",
                  local_user_d.dsc$w_length,
                  local_user_d.dsc$a_pointer);

    if (delete) {
        /*
        ** Try deleting the remote_node::remote_user/local_user combo.
        ** This may not work if the local user is the only username for
        ** the remote...
        */
        r0_status = sys$delete_proxy (&nodename_d,
                                      &username_d,
                                      &username_d,
                                      PRX$M_EXACT);
        if (r0_status == SECSRV$_INVALIDDELETE) {
            /*
            ** ... and it was the last local.  Delete the entire proxy
            ** record.
            */
            r0_status = sys$delete_proxy (&nodename_d,
                                          &username_d,
                                          0,
                                          0);
            errchk_sig (r0_status);
        } else {
            errchk_sig (r0_status);
        }
        (void)printf ("Proxy record deleted\n");
    } else {
        (void)printf ("As the proxy already existed before this program ran, "
                      "it was not deleted\n");
    }
}
