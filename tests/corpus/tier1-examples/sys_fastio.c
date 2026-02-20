
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
#include <fibdef.h>
#include <efndef.h>
#include <unixio.h>
#include <fcntl.h>
#include <iodef.h>
#include <iosadef.h>
#include <iosbdef.h>
#include <va_rangedef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#define NUM_RECORDS 1000
#define RECORD_LENGTH 512


/******************************************************************************/
static void create_file (char *name) {

static int fd;
static int record_count = 0;

static char record[RECORD_LENGTH];
static char mrs[10];

    (void)printf ("Creating file %s... ", name);
    (void)fflush (stdout);

    (void)sprintf (mrs, "mrs=%d", RECORD_LENGTH);
    fd = creat (name, O_CREAT, "rat=cr", "rfm=fix", mrs);
    if (fd < 0) {
        (void)printf ("Can't open file %s\n", name);
        exit (EXIT_FAILURE);
    }

    while (record_count < NUM_RECORDS) {
        for (int j = 0; j < RECORD_LENGTH; j++) {
            record[j] = (record_count % 256);
        }
        (void) write (fd, record, RECORD_LENGTH);
        record_count++;
    }

    (void)close (fd);
    (void)printf ("done.\n");
}


/******************************************************************************/
int main (void) {
/*
** You must hold an identifier called VMS$BUFFER_OBJECT_USER to run this
** program.
*/

static IOSB iosb;

static VA_RANGE buffer_ia;
static VA_RANGE iosa_ia;
static VA_RANGE buffer_oa;
static VA_RANGE iosa_oa;
static GENERIC_64 buffer_handle;
static GENERIC_64 iosa_handle;

static FANDLE fan;

static IOSA *iostat;

static int r0_status;

static struct FAB fab;
static struct NAM nam;

static FIBDEF fib;
static struct {
    unsigned int length;
    FIBDEF *pointer;
} fib_d = { 22, &fib };

static unsigned short int channel;

static char file[] = "SYS$SCRATCH:SYS_FASTIO.DEMO";
static char ess[NAM$C_MAXRSS];
static char rss[NAM$C_MAXRSS];

static $DESCRIPTOR (file_d, file);

    /*
    ** Create a file for us to play with.
    */
    create_file (file);

    /*
    ** Set up a file access block...
    */
    fab = cc$rms_fab;
    fab.fab$b_fns = file_d.dsc$w_length;
    fab.fab$l_fna = file_d.dsc$a_pointer;
    fab.fab$l_nam = &nam;

    /*
    ** ... and a name block.
    */
    nam = cc$rms_nam;
    nam.nam$b_ess = NAM$C_MAXRSS;
    nam.nam$l_esa = ess;
    nam.nam$b_rss = NAM$C_MAXRSS;
    nam.nam$l_rsa = rss;

    /*
    ** Opening the file causes a SYS$DISPLAY to be performed, filling in
    ** information in the fab and nam, including the file ID.
    */
    r0_status = sys$open (&fab);
    errchk_sig (r0_status);

    r0_status = sys$close (&fab);
    errchk_sig (r0_status);

    /*
    ** Assign a channel to the disk that contains the file.
    */
    r0_status = sys$assign (&file_d,
                            &channel,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Copy the file id to the file information block and access the file.
    */
    (void)memcpy (&fib.fib$w_fid, &nam.nam$w_fid, 6);
    fib.fib$l_acctl = 0;
    r0_status = sys$qiow (EFN$C_ENF,
                          channel,
                          IO$_ACCESS|IO$M_ACCESS,
                          &iosb,
                          0,
                          0,
                          &fib_d,
                          0,
                          0,
                          0,
                          0,
                          0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);

    /*
    ** Get some page aligned memory to create a buffered object
    */
    r0_status = sys$expreg (RECORD_LENGTH / 512,
                            &buffer_ia,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Create a buffered object for fast I/O
    */
    r0_status = sys$create_bufobj (&buffer_ia,
                                   &buffer_oa,
                                   0,
                                   0,
                                   &buffer_handle);
    errchk_sig (r0_status);

    /*
    ** Do the same thing for the i/o status area.  One page is more
    ** than enough.
    */
    r0_status = sys$expreg (1,
                            &iosa_ia,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Create a buffered object for it.
    */
    r0_status = sys$create_bufobj (&iosa_ia,
                                   &iosa_oa,
                                   0,
                                   0,
                                   &iosa_handle);
    errchk_sig (r0_status);

    /*
    ** Set up a variable to make it a little easier to get the iosa status.
    */
    iostat = (IOSA *)iosa_oa.va_range$ps_start_va;

    /*
    ** Set up the I/O
    */
    r0_status = sys$io_setup (IO$_READVBLK,
                              &buffer_handle,
                              &iosa_handle,
                              0,
                              0,
                              (unsigned __int64 *)&fan);
    errchk_sig (r0_status);

    (void)printf ("Reading file with fast I/O... ");
    (void)fflush (stdout);

    for (int i = 1; i <= NUM_RECORDS; i++) {
        /*
        ** Read using Fast I/O.  Note the data ends up at buffer_oa.start_addr
        ** and the following RECORD_LENGTH bytes.
        */
        r0_status = sys$io_performw (fan,
                                     channel,
                                     iostat,
                                     buffer_oa.va_range$ps_start_va,
                                     RECORD_LENGTH,
                                     i);
        errchk_sig (r0_status);
        errchk_sig (iostat->iosa$l_status);
    } 

    (void)printf ("done.\n");

    /*
    ** Deaccess the file.
    */
    r0_status = sys$qiow (EFN$C_ENF,
                          channel,
                          IO$_DEACCESS,
                          &iosb,
                          0,
                          0,
                          &fib_d,
                          0,
                          0,
                          0,
                          0,
                          0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$w_status);

    /*
    ** Deassign the channel from the disk.
    */
    r0_status = sys$dassgn (channel);
    errchk_sig (r0_status);

    /*
    ** Delete the file we created.
    */
    (void)remove (file);

    /*
    ** Clean up the fast I/O setup stuff
    */
    r0_status = sys$io_cleanup (fan);
    errchk_sig (r0_status);

    /*
    ** Delete the two buffered objects.
    */
    r0_status = sys$delete_bufobj (&buffer_handle);
    errchk_sig (r0_status);

    r0_status = sys$delete_bufobj (&iosa_handle);
    errchk_sig (r0_status);

    /*
    ** Free the memory used by the buffered objects.
    */
    r0_status = sys$deltva (&buffer_oa, 0, 0);
    errchk_sig (r0_status);

    r0_status = sys$deltva (&iosa_oa, 0, 0);
    errchk_sig (r0_status);
}
