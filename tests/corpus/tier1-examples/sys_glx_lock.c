
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <glockdef.h>
#include <syidef.h>
#include <efndef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <psldef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This code demos the galaxy lock services.  Naturally, you must run this code
** on a galaxy partition.  If you don't have galaxy-capable hardware, you can
** still test this code by creating a single-instance galaxy on any alpha.
** See the "OpenVMS Alpha Partitioning and Galaxy Guide" for further
** information on this.
**
** You need CMKRNL and SHMEM privileges to run this code.
**
*/

#ifndef __ALPHA
#  error "Alpha specific code"
#endif

static IOSB iosb;

static unsigned __int64 lock_handle;

static int r0_status;
static unsigned int galaxy;
static unsigned int page_size;
static unsigned int length;
static unsigned int protection = 0x0000ff00;
static unsigned int lock_max_size;
static unsigned int lock_min_size;
static unsigned int tbl_handle;
static unsigned int info_ipl;
static unsigned int info_rank;
static unsigned int info_timeout;
static unsigned int info_size;

static ILE3 syiitms[] = { 4, SYI$_GALAXY_MEMBER, &galaxy, NULL,
                          4, SYI$_PAGE_SIZE, &page_size, NULL,
                          0, 0, NULL, NULL }; 

static unsigned short int info_flags;

static $DESCRIPTOR (table_d, "GLX_DEMO_TABLE");
static $DESCRIPTOR (lock_d, "GLX_DEMO_LOCK");

static char info_name[16+1];

static struct dsc$descriptor_s info_name_d = { 0,
                                               DSC$K_DTYPE_T,
                                               DSC$K_CLASS_S,
                                               info_name };

    /*
    ** Find out if we are running on a galaxy partition and what our
    ** CPU-specific page size is.
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

    if (!galaxy) {
        (void)printf ("You must run this on a galaxy!\n");
        exit (EXIT_FAILURE);
    }

    r0_status = sys$get_galaxy_lock_size (&lock_min_size,
                                          &lock_max_size);
    errchk_sig (r0_status);

    /*
    ** Assign 10 pages of memory for the table.
    */
    length = page_size * 10;

    (void)printf ("Creating galaxy lock table %-.*s... ",
                  table_d.dsc$w_length,
                  table_d.dsc$a_pointer);
    (void)fflush (stdout);

    r0_status = sys$create_galaxy_lock_table (&table_d,
                                              PSL$C_EXEC,
                                              length,
                                              GLCKTBL$C_SYSTEM,
                                              protection,
                                              lock_max_size,
                                              &tbl_handle);
    if (r0_status == SS$_NOPRIV) {
        (void)printf ("You need CMKRNL and SHMEM to run this code!\n");
        exit (EXIT_FAILURE);
    } else {
        errchk_sig (r0_status);
    }

    (void)printf ("created.\n");

    (void)printf ("Creating galaxy lock %-.*s... ",
                  lock_d.dsc$w_length,
                  lock_d.dsc$a_pointer);
    (void)fflush (stdout);

    r0_status = sys$create_galaxy_lock (tbl_handle,
                                        &lock_d,
                                        100,
                                        lock_max_size,
                                        0,
                                        0,
                                        &lock_handle);
    errchk_sig (r0_status);

    (void)printf ("created.\n");

    (void)printf ("Acquiring galaxy lock... ");
    (void)fflush (stdout);

    r0_status = sys$acquire_galaxy_lock (lock_handle,
                                         100,
                                         0);
    errchk_sig (r0_status);

    (void)printf ("acquired.\n");

    r0_status = sys$get_galaxy_lock_info (lock_handle,
                                          info_name,
                                          &info_timeout,
                                          &info_size,
                                          &info_ipl,
                                          &info_rank,
                                          &info_flags,
                                          &info_name_d.dsc$w_length);
    errchk_sig (r0_status);

    (void)printf ("Lock information:\n"
                  "            Name: %-.*s\n"
                  "         Timeout: %u\n"
                  "            Size: %u\n"
                  "             IPL: %u\n"
                  "            Rank: %u\n"
                  "           Flags: %u\n",
                  info_name_d.dsc$w_length,
                  info_name_d.dsc$a_pointer,
                  info_timeout,
                  info_size,
                  info_ipl,
                  info_rank,
                  info_flags);

    (void)printf ("Releasing galaxy lock... ");
    (void)fflush (stdout);

    r0_status = sys$release_galaxy_lock (lock_handle);
    errchk_sig (r0_status);

    (void)printf ("released.\n");

    (void)printf ("Deleting galaxy lock... ");
    (void)fflush (stdout);

    r0_status = sys$delete_galaxy_lock (lock_handle);
    errchk_sig (r0_status);

    (void)printf ("deleted.\n");

    (void)printf ("Deleting galaxy lock table... ");
    (void)fflush (stdout);

    r0_status = sys$delete_galaxy_lock_table (tbl_handle);
    errchk_sig (r0_status);

    (void)printf ("deleted.\n");
}
