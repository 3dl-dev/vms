
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <pscandef.h>
#include <jpidef.h>
#include <efndef.h>
#include <assert.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb;

static int r0_status;
static unsigned int pid;
static unsigned int ps_ctx = 0;
static int nodename_len;
static int prcnam_len;
static int grp;
static int mbr;

/*
** This item list will set up $process_scan to find all processes on the
** cluster with our UIC.
*/
static struct {
    unsigned short int length;
    unsigned short int code;
    unsigned int val;
    unsigned int flags;
} psitms[] = { 0, PSCAN$_NODE_CSID, 0, PSCAN$M_NEQ,
               0, PSCAN$_GRP, 0, PSCAN$M_EQL,
               0, PSCAN$_MEM, 0, PSCAN$M_EQL,
               0, 0, 0, 0 };

static char nodename[6];
static char prcnam[15];

static struct {
    unsigned short int length;
    unsigned short int code;
    void *bufadr;
    void *retlen;
} jpiitms[] = { 4, JPI$_PID, &pid, NULL,
                6, JPI$_NODENAME, &nodename, &nodename_len,
                15, JPI$_PRCNAM, &prcnam, &prcnam_len,
                0, 0, NULL, NULL };

    r0_status = lib$getjpi (&JPI$_GRP,
                            0,
                            0,
                            &grp,
                            0,
                            0);
    errchk_sig (r0_status);

    r0_status = lib$getjpi (&JPI$_MEM,
                            0,
                            0,
                            &mbr,
                            0,
                            0);
    errchk_sig (r0_status);

    assert (psitms[1].code == PSCAN$_GRP);
    psitms[1].val = grp;
    assert (psitms[2].code == PSCAN$_MEM);
    psitms[2].val = mbr;

    r0_status = sys$process_scan (&ps_ctx,
                                  &psitms);
    errchk_sig (r0_status);

    while (TRUE) {
        r0_status = sys$getjpiw (EFN$C_ENF,
                                 &ps_ctx,
                                 0,
                                 jpiitms,
                                 &iosb,
                                 0,
                                 0);
        if (iosb.iosb$l_getxxi_status == SS$_NOMOREPROC) {
            break;
        } else {
            errchk_sig (r0_status);
            errchk_sig (iosb.iosb$l_getxxi_status);
            (void)printf ("Process %-15.*s (%08x) on node %-.*s\n",
                          prcnam_len,
                          prcnam,
                          pid,
                          nodename_len,
                          nodename);
        }
    }
}
