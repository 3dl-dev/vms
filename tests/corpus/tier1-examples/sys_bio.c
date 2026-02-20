
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ssdef.h>
#include <stsdef.h>
#include <rms.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void rename_success (struct FAB *fab) {

    (void)printf ("Successfully renamed %-.*s%-.*s\n",
                  fab->fab$l_nam->nam$b_name,
                  fab->fab$l_nam->nam$l_name,
                  fab->fab$l_nam->nam$b_type,
                  fab->fab$l_nam->nam$l_type);

}


/******************************************************************************/
int main (void) {

static struct FAB fab1;
static struct FAB fab2;
static struct FAB fab3;
static struct RAB rab;
static struct NAM nam1;
static struct NAM nam3;

static int r0_status;
static int i;

#define RECORD_LEN 512
static char in_record[RECORD_LEN];
static char out_record[RECORD_LEN];
static char ess1[NAM$C_MAXRSS];
static char rss1[NAM$C_MAXRSS];
static char ess3[NAM$C_MAXRSS];
static char rss3[NAM$C_MAXRSS];

static char file1[] = "DEMO_SYS_BIO.ONE";
static char file2[] = "DEMO_SYS_BIO.TWO";
static char file3[] = "DEMO_SYS_BIO.THREE";

    fab1 = cc$rms_fab;
    fab1.fab$b_org = FAB$C_SEQ;
    fab1.fab$l_alq = 100;
    fab1.fab$b_fns = strlen (file1);
    fab1.fab$l_fna = file1;
    fab1.fab$b_fac = FAB$M_BIO|FAB$M_GET|FAB$M_PUT;
    fab1.fab$l_fop = FAB$M_ASY;
    fab1.fab$l_nam = &nam1;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab1;
    rab.rab$w_rsz = RECORD_LEN;
    rab.rab$l_rbf = in_record;
    rab.rab$w_usz = RECORD_LEN;
    rab.rab$l_ubf = out_record;

    fab2 = cc$rms_fab;
    fab2.fab$b_fns = strlen (file2);
    fab2.fab$l_fna = file2;

    fab3 = cc$rms_fab;
    fab3.fab$b_fns = strlen (file3);
    fab3.fab$l_fna = file3;
    fab3.fab$l_nam = &nam3;

    nam1 = cc$rms_nam;
    nam1.nam$b_ess = sizeof (ess1);
    nam1.nam$l_esa = ess1;
    nam1.nam$b_rss = sizeof (rss1);
    nam1.nam$l_rsa = rss1;

    nam3 = cc$rms_nam;
    nam3.nam$b_ess = sizeof (ess3);
    nam3.nam$l_esa = ess3;
    nam3.nam$b_rss = sizeof (rss3);
    nam3.nam$l_rsa = rss3;

    /*
    ** Create a file (asynchronously, to demo $wait)
    */
    r0_status = sys$create (&fab1);
    errchk_sig (r0_status);

    r0_status = sys$wait (&fab1);
    errchk_sig (r0_status);

    (void)printf ("Created file %s\n", file1);

    /*
    ** Back to synchronous operations...
    */
    fab1.fab$l_fop &= ~FAB$M_ASY;

    r0_status = sys$connect (&rab);
    errchk_sig (r0_status);

    (void)printf ("Populating file... ");
    (void)fflush (stdout);
    for (i = 1; i <= 50; i++) {
        rab.rab$l_bkt = i;
        in_record[0] = i;
        
        r0_status = sys$write (&rab);
        errchk_sig (r0_status);
    }
    (void)printf ("done.\n");

    (void)printf ("Verifying contents... ");
    (void)fflush (stdout);
    for (i = 50; i >= 1; i--) {
        rab.rab$l_bkt = i;
        r0_status = sys$read (&rab);
        errchk_sig (r0_status);

        if (out_record[0] != i) {
            (void)printf ("Oops, file is corrupt!\n");
            (void)lib$stop (SS$_ABORT);
        }
    }
    (void)printf ("done.\n");

    r0_status = sys$close (&fab1);
    errchk_sig (r0_status);

    (void)printf ("Renaming file...\n");
    r0_status = sys$rename (&fab1,
                            0,
                            rename_success,
                            &fab2);
    errchk_sig (r0_status);

    /*
    ** Complete setup of fab3...
    */
    r0_status = sys$parse (&fab3);
    errchk_sig (r0_status);

    /*
    ** Copy the file ID of the file we created to the nam block for fab3.
    ** Now we have fab1 and fab3 pointing at the same DID/FID, but with
    ** different names in their respective nam blocks...
    */
    nam3.nam$w_fid[0] = nam1.nam$w_fid[0];
    nam3.nam$w_fid[1] = nam1.nam$w_fid[1];
    nam3.nam$w_fid[2] = nam1.nam$w_fid[2];

    /*
    ** Create a directory entry for fab3.  This is equivelent to doing a
    ** $ SET FILE/ENTER from DCL.
    */
    (void)printf ("Creating directory entry... ");
    (void)fflush (stdout);
    r0_status = sys$enter (&fab3);
    errchk_sig (r0_status);
    (void)printf ("done.\n");

    /*
    ** Now remove the directory entry.
    */
    (void)printf ("Removing directory entry... ");
    (void)fflush (stdout);
    r0_status = sys$remove (&fab3);
    errchk_sig (r0_status);
    (void)printf ("done.\n");

    /*
    ** And delete the file.
    */
    (void)printf ("Deleting file... ");
    (void)fflush (stdout);
    r0_status = sys$erase (&fab2);
    errchk_sig (r0_status);
    (void)printf ("done.\n");
}
