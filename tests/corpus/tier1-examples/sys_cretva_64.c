
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <vadef.h>
#include <psldef.h>
#include <syidef.h>
#include <efndef.h>
#include <prtdef.h>
#include <iosbdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifdef __VAX
#  error "64 bit architecture required"
#endif

#define NUM_PAGES 10
#if (NUM_PAGES & 1 != 0)
#  error "NUM_PAGES must be an even number"
#endif

#pragma pointer_size save
#pragma pointer_size 32
struct itm3 {
    unsigned short int len;
    unsigned short int fun;
    void *addr;
    void *retlen;
};
#pragma pointer_size restore


/******************************************************************************/
int main (void) {
/*
** You have to compile this with $ CC/POINTER_SIZE.  It doesn't matter if you
** pick 32 or 64.  This code explicitly sets what it needs.
*/

static int r0_status;
static unsigned int page_size;
static unsigned int prevprot;

static IOSB iosb;

static ILE3 syiitms[] = { 4, SYI$_PAGE_SIZE, &page_size, NULL,
                          0, 0, NULL, NULL };

static GENERIC_64 region_id;
static unsigned __int64 region_len;
static unsigned __int64 req_byte_cnt;
static unsigned __int64 ret_byte_cnt;
static unsigned __int64 deleted_len;
static unsigned __int64 retadr;
static unsigned __int64 retlen;

#pragma pointer_size save
#pragma pointer_size 64
static void *start_addr;
static void *return_addr;
static void *deleted_addr;
#pragma pointer_size restore

    (void)printf ("Get CPU specific page size... ");
    (void)fflush (stdout);

    r0_status = sys$getsyiw (EFN$C_ENF,
                             0,
                             0,
                             &syiitms,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);
    errchk_sig (iosb.iosb$l_getxxi_status);

    (void)printf ("%u bytes per page\n", page_size);

    req_byte_cnt = page_size * NUM_PAGES;

    (void)printf ("Create a %u page region... ", NUM_PAGES);
    (void)fflush (stdout);

    r0_status = sys$create_region_64 (req_byte_cnt,
                                      VA$C_REGION_UCREATE_UOWN,
                                      0,
                                      &region_id,
                                      (void *)&start_addr,
                                      &region_len,
                                      0);
    errchk_sig (r0_status);
                                      
    (void)printf ("created.  Start address is %Lx\n",
                  start_addr);

    (void)printf ("Create some VA in the region... ");
    (void)fflush (stdout);
    r0_status = sys$cretva_64 (&region_id,
                               start_addr,
                               req_byte_cnt / 2,
                               PSL$C_USER,
                               VA$M_NO_OVERMAP,
                               (void *)&return_addr,
                               &ret_byte_cnt);
    errchk_sig (r0_status);

    (void)printf ("created.  Start address is %Lx\n",
                  start_addr);

    (void)printf ("Expanding the VA to fill region... ");
    (void)fflush (stdout);
    r0_status = sys$expreg_64 (&region_id,
                               req_byte_cnt / 2,
                               0,
                               0,
                               (void *)&start_addr,
                               &region_len);
    errchk_sig (r0_status);
    (void)printf ("expanded.  Start address is %Lx\n",
                  start_addr);

    (void)printf ("Changing the protection on the pages... ");
    (void)fflush (stdout);
    r0_status = sys$setprt_64 (start_addr,
                               region_len,
                               PSL$C_USER,
                               PRT$C_EW,
                               (void *)&retadr,
                               &retlen,
                               &prevprot);
    errchk_sig (r0_status);
    (void)printf ("changed.\n");

    (void)printf ("Deleting the VA... ");
    (void)fflush (stdout);
    r0_status = sys$deltva_64 (&region_id,
                               return_addr,
                               ret_byte_cnt,
                               0,
                               (void *)&deleted_addr,
                               &deleted_len);
    errchk_sig (r0_status);

    (void)printf ("deleted.  Start address was %Lx\n",
                  deleted_addr);

    (void)printf ("Deleting VA region... ");
    (void)fflush (stdout);
    r0_status = sys$delete_region_64 (&region_id,
                                      0,
                                      (void *)&deleted_addr,
                                      &deleted_len);
    errchk_sig (r0_status);

    (void)printf ("deleted.\n");
}
