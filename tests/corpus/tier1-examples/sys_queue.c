
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <syidef.h>
#include <quidef.h>
#include <sjcdef.h>
#include <jbcmsgdef.h>
#include <descrip.h>
#include <efndef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** SYS$GETQUI and SYS$SNDJBC (and their synchronous counterparts) are two of
** the more erm, extensive APIs supplied by OpenVMS.  There is no way I can
** demo all the functionalty available here.  This program finds the first
** open batch queue on this node able to accept work, creates a NOOP command
** procedure, and submits it.
*/

static IOSB iosb;

static FILE *fp;

static int r0_status;
static unsigned int flags = QUI$M_SEARCH_BATCH;
static unsigned int queue_status;
static unsigned int entry_num;

static unsigned short int que_node_len;
static unsigned short int our_node_len;

static char comfile[] = "sys_queue.com";
static char wild[] = "*";
static char queue[31];
static char que_node[6];
static char our_node[6];

static struct dsc$descriptor_s queue_d = { sizeof (queue),
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           queue };
static ILE3 syiitms[] = 
    { sizeof (our_node), SYI$_NODENAME, our_node, &our_node_len,
      0, 0, NULL, NULL };

static ILE3 quiitms[] = 
    { sizeof (wild) - 1, QUI$_SEARCH_NAME, &wild, NULL,
      sizeof (flags), QUI$_SEARCH_FLAGS, &flags, NULL,
      sizeof (queue), QUI$_QUEUE_NAME, queue, &queue_d.dsc$w_length,
      sizeof (que_node), QUI$_SCSNODE_NAME, &que_node, &que_node_len,
      sizeof (queue_status), QUI$_QUEUE_STATUS, &queue_status, NULL,
      0, 0, NULL, NULL };

static ILE3 sjcitms[] =
    { 0, SJC$_QUEUE, queue, NULL,
      sizeof (comfile) - 1, SJC$_FILE_SPECIFICATION, comfile, NULL,
      sizeof (entry_num), SJC$_ENTRY_NUMBER_OUTPUT, &entry_num, NULL,
      0, 0, NULL, NULL };
          
static char found_queue = FALSE;
static char looking = TRUE;

    /*
    ** Get the name of the node we are running on.
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
    ** Cancel any outstanding queue operations.
    */
    r0_status = sys$getquiw (EFN$C_ENF,
                             QUI$_CANCEL_OPERATION,
                             0,
                             0,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);
                             
    /*
    ** Search for a batch queue.
    */
    while (looking) {
        r0_status = sys$getquiw (EFN$C_ENF,
                                 QUI$_DISPLAY_QUEUE,
                                 0,
                                 quiitms,
                                 &iosb,
                                 0,
                                 0);
        errchk_sig (r0_status);
        switch (iosb.iosb$l_getxxi_status) {
            case JBC$_NOMOREQUE :
                looking = FALSE;
                continue;
                break;
            default :
                errchk_sig (iosb.iosb$l_getxxi_status);
                if ((our_node_len == que_node_len) &&
                    (memcmp (our_node, que_node, our_node_len) == 0)) {
                    if (((queue_status & QUI$M_QUEUE_IDLE) ||
                        ((queue_status & QUI$M_QUEUE_AVAILABLE)) &&
                        !(queue_status & QUI$M_QUEUE_CLOSED))) {
                        looking = FALSE;
                        found_queue = TRUE;
                        continue;
                    }
                }
                break;
        }
    }

    if (found_queue) {
        (void)printf ("Found available batch queue %-.*s.  Creating "
                      "com file...\n",
                      queue_d.dsc$w_length,
                      queue_d.dsc$a_pointer);

        fp = fopen (comfile, "w");
        assert (fp != NULL);
        (void)fprintf (fp, "$ set verify\n");
        (void)fprintf (fp, "$ write sys$output \"Foo was here\"\n");
        (void)fprintf (fp, "$ exit\n");
        if (fclose (fp) != 0) {
            (void)printf ("Could not close file!\n");
            exit (EXIT_FAILURE);
        }

        assert (sjcitms[0].ile3$w_code == SJC$_QUEUE);
        sjcitms[0].ile3$w_length = queue_d.dsc$w_length;

        /*
        ** Create job to run in batch.  Note the defaults for the log file
        ** are to print it and delete it.
        */
        r0_status = sys$sndjbcw (EFN$C_ENF,
                                 SJC$_ENTER_FILE,
                                 0,
                                 sjcitms,
                                 &iosb,
                                 0,
                                 0);
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);
        printf ("Job %u submitted.\n",
                entry_num);
    } else {
        printf ("No available batch queues.  Job not submitted.\n");
    }
}
