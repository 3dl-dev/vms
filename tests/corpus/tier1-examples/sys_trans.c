
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <efndef.h>
#include <rms.h>
#include <iosbdef.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#ifndef cc$rms_xabitm
#define cc$rms_xabitm XAB$C_ITM
#endif

#ifndef XAB$_XABTID
#define XAB$_XABTID 320
#endif

#ifndef DDTM$M_NONDEFAULT
#define DDTM$M_NONDEFAULT 2
#endif

#include "errchk.h"

/*
** We need something to rollback a transaction if we have started one.
** If I was coding this in a real application, I'd do something more... elegant
** than this, but... demo code.
*/
#define errchk_abort(arg) if (!($VMS_STATUS_SUCCESS(arg))) \
			{ \
			    int abort_status; \
			    IOSB abort_iosb; \
			    (void)printf ("Transaction aborted due to " \
					  "unexpected error!\n"); \
			    abort_status = sys$abort_transw (EFN$C_ENF, \
                                      0, \
                                      &abort_iosb, \
                                      0, \
                                      0, \
                                      &tid); \
			    errchk_sig (abort_status); \
			    errchk_sig (abort_iosb.iosb$w_status); \
                            (void)lib$signal(arg); \
			}

struct tid {
    char opaque[16];
};

struct record {
    unsigned int key;
    char data[60];
};


/******************************************************************************/
int main (void) {
/*
** Demo SYS$START_TRANSW, SYS$END_TRANSW, and SYS$ABORT_TRANSW.  Note that
** the demo uses RMS Journalling.  RMS Journalling requires a software license,
** and that you have DEC DTM running on your system, and that you have a valid
** transaction log file defined.
**
** You run this program multiple times.  Firstly, it creates an empty indexed
** file and askes you to mark the file for journalling.  Then you run it
** again and either commit or rollback the put to the file.  Then you can
** look in the file to see the results.
**
** The command for marking and unmarking a file for journalling is:
**
**       $ SET FILE/[NO]RU_JOURNAL filename
**
*/

static struct FAB fab;
static struct RAB rab;
static struct XABKEY xabkey;
static XABITMDEF xabitm;

static int r0_status;
static const unsigned int flags = DDTM$M_NONDEFAULT;

static char filename[] = "SYS_TRANS.IDX";

static struct tid tid;
static struct record record;
static IOSB iosb;

static ILE3 item_list[] = 
    { sizeof (struct tid), XAB$_XABTID, &tid, NULL,
      0, 0, NULL, NULL };

static char commit = FALSE;
static char finished = FALSE;
static char report = FALSE;

static char input[255+1];
static struct dsc$descriptor_s input_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           input };
static const $DESCRIPTOR (prompt_d, "Commit the record? (Y/N) ");

    /*
    ** Set up the File Access Block
    */
    fab = cc$rms_fab;
    fab.fab$b_fns = strlen (filename);
    fab.fab$l_fna = filename;
    fab.fab$b_org = FAB$C_IDX;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$w_mrs = sizeof (struct record);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$l_xab = &xabkey;

    /*
    ** Describe the primary key.
    */
    xabkey = cc$rms_xabkey;
    xabkey.xab$b_ref = 0;
    xabkey.xab$w_pos0 = 0;
    xabkey.xab$b_siz0 = 4;
    xabkey.xab$b_dtp = XAB$C_BN4;
    xabkey.xab$l_nxt = 0;

    /*
    ** Set up the Record Access Block.
    */
    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$w_rsz = sizeof (struct record);
    rab.rab$w_usz = sizeof (struct record);
    rab.rab$l_rbf = (char *)&record;
    rab.rab$l_ubf = (char *)&record;
    rab.rab$l_xab = &xabitm;

    /*
    ** Set up the Item eXtended Attributes Block to specify the transaction
    ** item.
    */
    xabitm.xab$b_cod = cc$rms_xabitm;
    xabitm.xab$b_bln = XAB$K_ITMLEN;
    xabitm.xab$l_itemlist = &item_list;
    xabitm.xab$b_mode = XAB$K_SETMODE;
    xabitm.xab$l_nxt = 0;

    /*
    ** Try to open the file.
    */
    r0_status = sys$open (&fab);
    switch (r0_status) {
        case RMS$_FNF :
            /*
            ** If the file doesn't exist, create it and tell the user how to
            ** mark the new file for run unit journalling.
            */
            r0_status = sys$create (&fab);
            errchk_sig (r0_status);
            (void)printf ("You have successfully created the test file %s\n",
                          filename);
            (void)printf ("Please issue the following DCL command and run the "
                          "code again:\n\n\t$ SET FILE/RU_JOURNAL %s\n\n",
                          filename);
            exit (EXIT_SUCCESS);
            break;
        case RMS$_JNLNOTAUTH :
            (void)printf ("No RMS Journal license found.  Unable to "
                          "continue.\n");
            exit (EXIT_FAILURE);
            break;
        default :
            /*
            ** The file existed.  Check for unexpected errors.
            */
            errchk_sig (r0_status);

    }

    /*
    ** Check that the file is marked for journalling.
    */
    if ((fab.fab$b_journal & FAB$M_RU) == 0) {
        (void)printf ("The file %s is not marked for journalling.\n"
                      "Please issue the following DCL command and run "
                      "the code again:\n\n\t$ SET FILE/RU_JOURNAL %s\n\n",
                      filename,
                      filename);
        exit (EXIT_FAILURE);
    }

    /*
    ** Suggest an order for the test.
    */
    (void)printf ("The program is now about to write a record to the file.  "
                  "A suggested order\nfor running the tests is to answer no "
                  "to the commit question first.  Then\nyou can look at the "
                  "file and see it's empty, then run the program again \n"
                  "and answer yes.  In this way, you won't get a message "
                  "about duplicate keys.\n\n");

    /*
    ** Connect a record stream to the file.
    */
    r0_status = sys$connect (&rab);
    errchk_sig (r0_status);

    /*
    ** Start a transaction.
    */
    (void)printf ("Starting a transaction\n");
    r0_status = sys$start_transw (EFN$C_ENF,
                                  flags,
                                  &iosb,
                                  0,
                                  0,
                                  &tid);
    if (r0_status == SS$_NOLOG) {
        (void)printf ("No log file found.  Please use MCR LMCP to create "
                      "one!\n");
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
        errchk_sig (iosb.iosb$w_status);
    }

    /*
    ** Set up a record...
    */
    record.key = 1;
    strcpy (record.data, "ONE");

    /*
    ** ...and write it to the file.
    */
    (void)printf ("Writing record to file\n");
    r0_status = sys$put (&rab);
    if (r0_status == RMS$_DUP) {
        (void)printf ("Duplicate key detected.  This means the program has "
                      "been run before and\nthe record commited to the file. "
                      " Please unmark the file for journaling\nand start "
                      "again.\n");
    } else if (r0_status == RMS$_ACC_RUJ && rab.rab$l_stv == RMS$_DNF) {
	(void)printf ("The [SYSJNL.nodename] directory does not exist!\n"
		      "The manual says RMS Journaling will create it, but\n"
		      "this doesn't happen.  Create a directory in the MFD\n"
		      "of this disk called [SYSJNL.nodename], where nodename\n"
		      "is the name of this node, and run the code again.\n");
    } else {
        errchk_abort (r0_status);

        report = TRUE;

        /*
        ** See what the user wants to do.
        */
        while (!finished) {
            input_d.dsc$w_length = sizeof (input) - 1;
            r0_status = lib$get_input (&input_d,
                                       &prompt_d,
                                       &input_d);
            if (r0_status != RMS$_EOF) {
                errchk_abort (r0_status);
            }
            switch (input[0]) {
                case 'Y':
                case 'y':
                    commit = TRUE;
                    finished = TRUE;
                    break;
                case 'N':
                case 'n':
                    finished = TRUE;
                    break;
                default:
                    (void)printf ("Please answer Y or N.\n");
                    break;
            }
        }
    }

    if (commit) {
        (void)printf ("Commit tranaction.\n");
        (void)printf ("You should be able to dump the test file and see the "
                      "record.\n");

        r0_status = sys$end_transw (EFN$C_ENF,
                                    0,
                                    &iosb,
                                    0,
                                    0,
                                    &tid);
    } else {
        (void)printf ("Abort transaction\n");
        if (report) {
            (void)printf ("You should be able to dump the test file and see "
                          "that it is empty.\n");
        }

        r0_status = sys$abort_transw (EFN$C_ENF,
                                      0,
                                      &iosb,
                                      0,
                                      0,
                                      &tid);
    }
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);

    /*
    ** Close the file.
    */
    r0_status = sys$close (&fab);
    errchk_sig (r0_status);
}
