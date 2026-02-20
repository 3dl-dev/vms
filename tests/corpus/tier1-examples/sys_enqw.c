
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lckdef.h>
#include <lkidef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <syidef.h>
#include <jpidef.h>
#include <descrip.h>
#include <efndef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#define MAX_FAC_LEN 8


/******************************************************************************/
static int process_limit (unsigned int limit,
                          struct dsc$descriptor_s *fac_d,
                          char cluster) {
/*
** Passed a limit and a "facility name", this function prevents the facility
** from being used more than limit times by a user.  The normal purpose of
** this is to stop multiple invocations of the main program by a single
** user.  The cluster parameter indicates if the facility is node or cluster
** wide.  If cluster is FALSE, the facility is node wide.
*/

static IOSB iosb;

static unsigned int limit_lksb[2];
static unsigned int lock_lksb[2];
static unsigned int grantcnt;
static unsigned int return_status;
static int r0_status;
static int jpi_item = JPI$_USERNAME;
static int syi_item = SYI$_SCSNODE;

static ILE3 lkiitms[] = { 4, LKI$_GRANTCOUNT, &grantcnt, NULL,
                          0, 0, NULL, NULL };

static char limit_res[31];
static char lock_res[31];
static char node[15];
static char username[12];

static struct dsc$descriptor_s limit_res_d = { 0,
                                               DSC$K_DTYPE_T,
                                               DSC$K_CLASS_S,
                                               limit_res };
static struct dsc$descriptor_s lock_res_d = { 0,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              lock_res };
static struct dsc$descriptor_s node_d = { sizeof (node),
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          node };
static struct dsc$descriptor_s username_d = { sizeof (username),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

    if (fac_d->dsc$w_length > MAX_FAC_LEN) {
        (void)printf ("Facility name limited to %d characters!\n",
            MAX_FAC_LEN);
        return SS$_ABORT;
    }

    /*
    ** Get our username.
    */
    r0_status = lib$getjpi (&jpi_item,
                            0,
                            0,
                            0,
                            &username_d,
                            &username_d.dsc$w_length);
    errchk_sig (r0_status);
    for (int i = 0; i < sizeof (username); i++) {
        if (username[i] == ' ') {
            username[i] = '\0';
            username_d.dsc$w_length = i + 1;
            break;
        }
    }

    if (!cluster) {
        /*
        ** This will be a node-wide limit.  Get the node name.
        */
        r0_status = lib$getsyi (&syi_item,
                                0,
                                &node_d,
                                &node_d.dsc$w_length,
                                0,
                                0);
        errchk_sig (r0_status);

        /*
        ** Format the resource names.
        */
        lock_res_d.dsc$w_length = sprintf (lock_res,
                                           "L_%-.*s_%-.*s_%-.*s",
                                           fac_d->dsc$w_length,
                                           fac_d->dsc$a_pointer,
                                           node_d.dsc$w_length,
                                           node_d.dsc$a_pointer,
                                           username_d.dsc$w_length,
                                           username_d.dsc$a_pointer);
        limit_res_d.dsc$w_length = sprintf (limit_res,
                                            "Q_%-.*s_%-.*s_%-.*s",
                                            fac_d->dsc$w_length,
                                            fac_d->dsc$a_pointer,
                                            node_d.dsc$w_length,
                                            node_d.dsc$a_pointer,
                                            username_d.dsc$w_length,
                                            username_d.dsc$a_pointer);
    } else {
        /*
        ** Format the resource names for a cluster wide limit.
        */
        lock_res_d.dsc$w_length = sprintf (lock_res,
                                           "L_%-.*s_%-.*s",
                                           fac_d->dsc$w_length,
                                           fac_d->dsc$a_pointer,
                                           username_d.dsc$w_length,
                                           username_d.dsc$a_pointer);
        limit_res_d.dsc$w_length = sprintf (limit_res,
                                            "Q_%-.*s_%-.*s",
                                            fac_d->dsc$w_length,
                                            fac_d->dsc$a_pointer,
                                            username_d.dsc$w_length,
                                            username_d.dsc$a_pointer);
    }

    /*
    ** Get an exclusive lock on the "lock" lock to prevent others from taking
    ** out new locks on the limit lock while we check if we are over the limit.
    */
    r0_status = sys$enqw (EFN$C_ENF,
                          LCK$K_EXMODE,
                          lock_lksb,
                          0,
                          &lock_res_d,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0);
    errchk_sig (r0_status);
    errchk_sig (lock_lksb[0]);

    /*
    ** Now take out a null lock on the limit lock.
    */
    r0_status = sys$enqw (EFN$C_ENF,
                          LCK$K_NLMODE,
                          limit_lksb,
                          0,
                          &limit_res_d,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0,
                          0);
    errchk_sig (r0_status);
    errchk_sig (limit_lksb[0]);

    /*
    ** Now count the number of holders of the limit lock.
    */
    r0_status = sys$getlkiw (EFN$C_ENF,
                             &limit_lksb[1],
                             &lkiitms,
                             &iosb,
                             0,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    if (grantcnt > limit) {
        /*
        ** If we are over the limit, drop our null lock on the limit lock.
        */
        r0_status = sys$deq (limit_lksb[1],
                             0,
                             0,
                             0);
        errchk_sig (r0_status);
        return_status = SS$_EXQUOTA;
    } else {
        return_status = SS$_NORMAL;
    }

    /*
    ** All done.  Drop the exclusive lock on the "lock" lock.
    */
    r0_status = sys$deq (lock_lksb[1],
                         0,
                         0,
                         0);
    errchk_sig (r0_status);
    return return_status;
}


/******************************************************************************/
int main (void) {

static int r0_status;
static $DESCRIPTOR (fac_d, "TEST");

    r0_status = process_limit (3, &fac_d, TRUE);
    if (r0_status == SS$_EXQUOTA) {
        (void)printf ("Process limit reached - exiting\n");
    } else {
        errchk_sig (r0_status);
        (void)printf ("Process sleeping forever\n");
        (void)sys$hiber ();
    }
}
