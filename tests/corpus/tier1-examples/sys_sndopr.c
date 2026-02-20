
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <iodef.h>
#include <efndef.h>
#include <opcdef.h>
#include <rms.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

/*
** On another terminal, enable yourself as an operator with the
** $ REPLY/ENABLE=CENTRAL/TEMP command, then run this program.  If you
** elect to wait for an operator to respond, use the $ REPLY/TO=rqid "anything"
** command from the other terminal to complete the request.
*/

static IOSB iosb;

static int r0_status;

static unsigned short int chan;
static unsigned short int length;

static char input[255];
static char mbx_buffer[255];

static OPCDEF reply;

static struct dsc$descriptor_s reply_d;
static struct dsc$descriptor_s input_d = { sizeof (input),
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };
static const $DESCRIPTOR (prompt_d, "Wait for an operator to reply? [N] ");

static char wait = FALSE;

    r0_status = lib$get_input (&input_d.dsc$w_length,
                               &prompt_d,
                               &input_d);
    if (r0_status == RMS$_EOF) {
        exit (EXIT_SUCCESS);
    } else {
        errchk_sig (r0_status);
    }

    if (strchr ("YyTt1", input[0]) != NULL) {
        wait = TRUE;
    }

    reply.opc$b_ms_type = OPC$_RQ_RQST;
    reply.opc$b_ms_target = OPC$M_NM_CENTRL;
    reply.opc$l_ms_rqstid = 0;

    reply_d.dsc$a_pointer = (char *)&reply;

    length = sprintf ((char *)&reply.opc$l_ms_text,
                      "SYS_SNDOPR.C demo.%s",
                      wait ? "  Waiting for operator..." : "");

    /*
    ** Max length of message is 128 characters.
    */
    assert (length <= 128);

    reply_d.dsc$w_length = length + 8;

    if (!wait) {
        /*
        ** Send the message to the central operator and don't wait for a
        ** reply.
        */
        r0_status = sys$sndopr (&reply_d,
                                0);
    } else {
        /*
        ** Send the message to the central operator and wait for a
        ** reply.
        */
        r0_status = sys$crembx (0,
                                &chan,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0);
        errchk_sig (r0_status);

        r0_status = sys$sndopr (&reply_d,
                                chan);

        r0_status = sys$qiow (EFN$C_ENF,
                              chan,
                              IO$_READVBLK,
                              &iosb,
                              0,
                              0,
                              mbx_buffer,
                              sizeof (mbx_buffer),
                              0,
                              0,
                              0,
                              0);
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);

        r0_status = sys$dassgn (chan);
        errchk_sig (r0_status);
        (void)printf ("%s\n", &mbx_buffer[8]);
    }
    errchk_sig (r0_status);
}
