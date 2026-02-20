
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <prcdef.h>
#include <uaidef.h>
#include <prvdef.h>
#include <gen64def.h>
#include <jpidef.h>
#include <iledef.h>
#include <pqldef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static PRVDEF def_priv;

static unsigned int r0_status;
static unsigned int pid;
static unsigned int prio;
static int i;
static int jpiitm = JPI$_USERNAME;

/*
** The order of the following struct is important to the upcoming code.
** Read on.
*/
static ILE3 uaiitms[] = {  4, UAI$_ASTLM, NULL, NULL,
                           4, UAI$_BIOLM, NULL, NULL,
                           4, UAI$_BYTLM, NULL, NULL,
                           4, UAI$_CPUTIM, NULL, NULL,
                           4, UAI$_DIOLM, NULL, NULL,
                           4, UAI$_ENQLM, NULL, NULL,
                           4, UAI$_FILLM, NULL, NULL,
                           4, UAI$_JTQUOTA, NULL, NULL,
                           4, UAI$_PGFLQUOTA, NULL, NULL,
                           4, UAI$_PRCCNT, NULL, NULL,
                           4, UAI$_TQCNT, NULL, NULL,
                           4, UAI$_DFWSCNT, NULL, NULL,
                           4, UAI$_WSEXTENT, NULL, NULL,
                           4, UAI$_WSQUOTA, NULL, NULL,
                           1, UAI$_PRI, &prio, NULL,
                          64, UAI$_DEF_PRIV, &def_priv, NULL,
                           0, 0, NULL, NULL };

static char username[12];

static struct dsc$descriptor_s username_d = { sizeof (username),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };

static $DESCRIPTOR (process_name_d, "ec_sys_creprc");
static $DESCRIPTOR (image_name_d, "SYS$SYSTEM:LOGINOUT.EXE");
static $DESCRIPTOR (command_proc_d, "SYS$MANAGER:SYLOGIN.COM");
static $DESCRIPTOR (null_d, "_NLA0:");

/*
** This structure contains the process quotas.  Note that it's nomember
** aligned.  Also note that the order of these quotas is important to the
** code that follows, as it assumes that the order is the same as the order in
** the uaiitms struct, and it uses this order to fill in the addresses of the
** value cells so the quota values get put in their correct locations.
**
** Engineering could have done a better job with this one.  The names of the
** symbols are in some cases radically different from each other (for example,
** UAI$_DFWSCNT vs PQL$_WSDEFAULT).  It would have been nice if they were the
** same name (and same value for that matter).
*/
#pragma member_alignment save
#pragma nomember_alignment
static struct {
    char quota;
    int value;
} quotas[15] = { PQL$_ASTLM, 0,
                 PQL$_BIOLM, 0,
                 PQL$_BYTLM, 0,
                 PQL$_CPULM, 0,
                 PQL$_DIOLM, 0,
                 PQL$_ENQLM, 0,
                 PQL$_FILLM, 0,
                 PQL$_JTQUOTA, 0,
                 PQL$_PGFLQUOTA, 0,
                 PQL$_PRCLM, 0,
                 PQL$_TQELM, 0,
                 PQL$_WSDEFAULT, 0,
                 PQL$_WSEXTENT, 0,
                 PQL$_WSQUOTA, 0,
                 PQL$_LISTEND, 0 };
#pragma member_alignment restore

    /*
    ** Get the username of this process.
    */
    r0_status = lib$getjpi (&jpiitm,
                            0,
                            0,
                            0,
                            &username_d,
                            &username_d.dsc$w_length);
    errchk_sig (r0_status);

    /*
    ** Set up the quota list.
    */
    i = 0;
    while (quotas[i].quota != PQL$_LISTEND) {
        uaiitms[i].ile3$ps_bufaddr = &quotas[i].value;
        i++;
    }

    /*
    ** Retrieve the base priority, default privilege mask, and quota values
    ** from the SYSUAF file.
    */
    r0_status = sys$getuai (0,
                            0,
                            &username_d,
                            uaiitms,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Create the process to run LOGININOUT.EXE and execute the SYSLOGIN.COM
    ** command procedure.
    */
    r0_status = sys$creprc (&pid,
                            &image_name_d,
                            &command_proc_d,
                            &null_d,
                            &null_d,
                            (struct _generic_64 *)&def_priv,
                            (unsigned int *)quotas,
                            &process_name_d,
                            prio,
                            0,
                            0,
                            PRC$M_INTER|PRC$M_NOPASSWORD,
                            0,
                            0);
    errchk_sig (r0_status);

    (void)printf ("PID = %08x\n",
                  pid);
}
