
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <rms.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#define RECORD_LEN 10


/******************************************************************************/
int main (void) {
/*
** RMS (Record Management Services) provide a comprehensive API for
** manipulating files and records within those files.  RMS supports multiple
** file formats, including sequential, relative, and indexed, and many
** record formats including fixed, variable, fixed variable, and stream.
** Additionally, RMS will allow all record handling to be bypassed to allow
** direct block manipulation.  It supports transparent network file access
** (via DECnet), tape handling, even terminal I/O.
**
**  See the "OpenVMS Record Management Services Reference Manual" and the
** "Guide to OpenVMS File Applications" for extensive documentation on this
** API.
**
** This program will demonstrate some simple calls to the API.  There is
** a _lot_ of functionality in RMS, and I have no way to demo it all.
**
** Read The Fine Manuals.
*/

static struct FAB fab;
static struct RAB rab;

static char filename[] = "RMS_DEMO.SEQ";

static char record[RECORD_LEN];
static int r0_status;

static int i;
static int j;

    /*
    ** Set up the file and record access blocks for a sequential file.
    */
    fab = cc$rms_fab;
    fab.fab$l_fna = filename;
    fab.fab$b_fns = strlen (filename);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_fac = FAB$M_GET|FAB$M_PUT;
    fab.fab$w_mrs = RECORD_LEN;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$b_shr = FAB$M_NIL;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$b_rac = RAB$C_SEQ;
    rab.rab$l_rop = RAB$M_WBH;
    rab.rab$l_rbf = record;
    rab.rab$w_rsz = RECORD_LEN;
    rab.rab$l_ubf = record;
    rab.rab$w_usz = RECORD_LEN;

    /*
    ** Create an empty file.
    */
    (void)printf ("Creating file...\n");
    r0_status = sys$create (&fab,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Connect a record stream so we may perform record operations.
    */
    (void)printf ("Connecting record stream...\n");
    r0_status = sys$connect (&rab,
                             0,
                             0);
    errchk_sig (r0_status);

    /*
    ** Oops, we forgot to preallocate the file.  Let's do it with an
    ** explicit extend operation.
    */
    (void)printf ("Extending file...\n");
    fab.fab$l_alq = 100;
    r0_status = sys$extend (&fab,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Write RECORD_LEN number of records to the file.
    */
    (void)printf ("Populating file...\n");
    for (i = 0; i < RECORD_LEN; i++) {
        for (j = 0; j < RECORD_LEN; j++) {
            record[j] = i + '0';
        }
        r0_status = sys$put (&rab,
                             0,
                             0);
        errchk_sig (r0_status);

    }

    /*
    ** Because we are doing "write behind", let's ensure all the data hits
    ** the disk before we start reading.
    */
    (void)printf ("Flushing data to disk...\n");
    r0_status = sys$flush (&rab,
                           0,
                           0);
    errchk_sig (r0_status);

    /*
    ** Get to the beginning of the file.
    */
    (void)printf ("Rewinding to beginning of file...\n");
    r0_status = sys$rewind (&rab,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Read and print the entire file.
    */
    (void)printf ("Dump of file contents...\n");
    while (r0_status != RMS$_EOF) {
        r0_status = sys$get (&rab,
                             0,
                             0);
        if (r0_status != RMS$_EOF) {
            errchk_sig (r0_status);
            (void)printf ("%-.*s\n",
                          rab.rab$w_rsz,
                          record);
        }
    }

    /*
    ** Explicitly disconnect the record stream.  Unless you are doing multiple
    ** record streams and want to disconnect just one, you can usually just
    ** skip this call.  sys$close will disconnect all streams.
    */
    (void)printf ("Disconnecting record stream...\n");
    r0_status = sys$disconnect (&rab,
                                0,
                                0);
    errchk_sig (r0_status);

    /*
    ** Close the file.
    */
    (void)printf ("Closing file...\n");
    r0_status = sys$close (&fab,
                           0,
                           0);
    errchk_sig (r0_status);

    (void)printf ("Deleting file...\n");
    r0_status = sys$erase (&fab,
                           0,
                           0);
    errchk_sig (r0_status);

    (void)printf ("Success\n");
}
