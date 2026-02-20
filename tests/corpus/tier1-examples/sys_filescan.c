
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <iledef.h>
#include <fscndef.h>
#include <assert.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int fldflags;

static int x;

static ILE2 fsitms[] = { 0, FSCN$_NODE, NULL,
                         0, FSCN$_NODE_ACS, NULL,
                         0, FSCN$_DEVICE, NULL,
                         0, FSCN$_DIRECTORY, NULL,
                         0, FSCN$_NAME, NULL,
                         0, FSCN$_TYPE, NULL,
                         0, FSCN$_VERSION, NULL,
                         0, FSCN$_FILESPEC, NULL,
                         0, 0, NULL };

static $DESCRIPTOR (srcstr_d,
                    "NODE\"USERNAME PASSWORD\"::DEV:[DIR]FILE.TYPE;-1");

    r0_status = sys$filescan (&srcstr_d,
                              fsitms,
                              &fldflags,
                              0,
                              0);
    errchk_sig (r0_status);

    x = 0;
    if ((fldflags & FSCN$M_NODE) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_NODE);
        (void)printf ("Node: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_NODE_ACS) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_NODE_ACS);
        (void)printf ("Node ACS: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_DEVICE) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_DEVICE);
        (void)printf ("Device: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_DIRECTORY) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_DIRECTORY);
        (void)printf ("Directory: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_NAME) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_NAME);
        (void)printf ("Name: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_TYPE) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_TYPE);
        (void)printf ("Type: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    if ((fldflags & FSCN$M_VERSION) != 0) {
        assert (fsitms[x].ile2$w_code == FSCN$_VERSION);
        (void)printf ("Version: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
    x++;
    assert (fsitms[x].ile2$w_code == FSCN$_FILESPEC);
    if (fsitms[x].ile2$w_length >= 1) {
        (void)printf ("Filespec: %-.*s\n",
                      fsitms[x].ile2$w_length,
                      (char *)fsitms[x].ile2$ps_bufaddr);
    }
}
