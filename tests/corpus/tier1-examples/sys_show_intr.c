
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ciadef.h>
#include <secsrvmsgdef.h>
#include <descrip.h>
#include <rms.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int context = 0;
static unsigned int count = 0;

static struct {
    unsigned short int type;
    unsigned short int flags;
    unsigned int count;
    GENERIC_64 time;
} breakin_blk;

static char username[1058];
static char time[23];
static char input[1];

static struct dsc$descriptor_s time_d = { sizeof (time),
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          time };
static struct dsc$descriptor_s username_d = { sizeof (username),
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              username };
static struct dsc$descriptor_s input_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };

static char finished = FALSE;
static char first_time = TRUE;

static $DESCRIPTOR (wild_d, "*");
static $DESCRIPTOR (prompt_d, "Delete? <y/N> ");

    while (!finished) {
        username_d.dsc$w_length = sizeof (username);
        r0_status = sys$show_intrusion (&wild_d,
                                        &username_d,
                                        &username_d.dsc$w_length,
                                        &breakin_blk,
                                        0,
                                        &context);
        switch (r0_status) {
            case SS$_NOSECURITY :
                (void)printf ("This program requires SECURITY privilege.\n");
                finished = TRUE;
                break;
            case SECSRV$_NOSUCHINTRUDER :
            case SECSRV$_CIADBEMPTY :
                if (count == 0) {
                    (void)printf ("No intruder records found\n");
                }
                finished = TRUE;
                break;
            case SS$_NOMOREITEMS :
                finished = TRUE;
                break;
            default :
                errchk_sig (r0_status);
                count++;
                time_d.dsc$w_length = sizeof (time);
                r0_status = sys$asctim (&time_d.dsc$w_length,
                                        &time_d,
                                        &breakin_blk.time,
                                        0);
                errchk_sig (r0_status);
                if (first_time) {
                    first_time = FALSE;
                    (void)printf ("Intrusion       Type       Count       "
                                  "Expiration          Source\n");
                    (void)printf ("---------       ----       -----       "
                                  "----------          ------\n");
                }
                (void)printf ("   ");
                if (breakin_blk.type & CIA$M_NETWORK) {
                    (void)printf ("NETWORK      ");
                } else if (breakin_blk.type & CIA$M_TERM_USER) {
                    (void)printf ("TERM_USER    ");
                } else if (breakin_blk.type & CIA$M_TERMINAL) {
                    (void)printf ("TERMINAL     ");
                } else if (breakin_blk.type & CIA$M_USERNAME) {
                    (void)printf ("USERNAME     ");
                } else {
                    assert (0);
                }
                if (breakin_blk.type & CIA$M_INTRUDER) {
                    (void)printf ("INTRUDER     ");
                } else if (breakin_blk.type & CIA$M_SUSPECT) {
                    (void)printf ("SUSPECT      ");
                } else {
                    assert (0);
                }
                (void)printf ("%2u   %-.*s  %-.*s\n",
                              breakin_blk.count,
                              time_d.dsc$w_length,
                              time_d.dsc$a_pointer,
                              username_d.dsc$w_length,
                              username_d.dsc$a_pointer);
                (void)memset (input, 0, sizeof (input));
                while (input[0] != 'Y' && input[0] != 'N') {
                    input_d.dsc$w_length = sizeof (input);
                    r0_status = lib$get_input (&input_d,
                                               &prompt_d,
                                               &input_d.dsc$w_length);
                    if (r0_status == RMS$_EOF) {
                        break;
                    } else {
                        errchk_sig (r0_status);
                    }
                    input[0] = toupper (input[0]);
                    if (input[0] == ' ') {
                        input[0] = 'N';
                    }
                    if (input[0] == 'Y') {
                        r0_status = sys$delete_intrusion (&username_d,
                                                          0);
                        errchk_sig (r0_status);
                        (void)printf ("Intrusion record deleted\n");
                    }
                }
                break;
        }
    }
}
