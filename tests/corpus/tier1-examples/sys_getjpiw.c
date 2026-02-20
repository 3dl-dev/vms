
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <jpidef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static IOSB iosb;

static int r0_status;
static unsigned int pid;
static unsigned int group;
static unsigned int member;

static char username[12];
static unsigned short int username_len;

static ILE3 jpiitms[] = {  4, JPI$_PID, &pid, NULL,
                           4, JPI$_GRP, &group, NULL,
                           4, JPI$_MEM, &member, NULL,
                          12, JPI$_USERNAME, username, &username_len,
                           0, 0, NULL, NULL };

    r0_status = sys$getjpiw (EFN$C_ENF,
                             0,
                             0,
                             jpiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    for (; username_len - 1 >= 1; username_len--) {
        if (!isspace (username[username_len - 1])) {
            break;
        }
    }

    (void)printf ("Hello %-.*s, your process ID is %08x "
                  "and your UIC is [%o,%o]\n",
                  username_len,
                  username,
                  pid,
                  group,
                  member);
}

