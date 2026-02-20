
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <syidef.h>
#include <psldef.h>
#include <secdef.h>
#include <hwrpbdef.h>
#include <vadef.h>
#include <seciddef.h>
#include <gen64def.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** To build:
**
**      $ define decc$text_library sys$share:sys$lib_c.tlb
**      $ cc/pointer=32 filename
**      $ link/sysexe filename
**
** You need PFNMAP, SYSGBL, and PRMGBL to run this program.
**
** Note that mapping physical memory is a Very Bad Idea(tm) unless you are a
** device driver.  This program is intended to demonstrate the system calls
** and nothing else.
**
** I'd like to thank the guys in the #vms IRC channel on 2600.net (particularly
** Burley and Hoff) for letting me bounce ideas off them.  This interaction
** let me correct a poor assumption that was preventing the code from working.
** Thanks for being there, guys!
**
*/

#ifndef __ALPHA
#  error "Alpha specific code"
#endif /* __ALPHA */

#pragma environment save
#pragma pointer_size 64
extern HWRPB *EXE$GPL_HWRPB_L;
static HWRPB *vir_hwrpb;
static HWRPB *map_hwrpb;
#pragma environment restore

static unsigned __int64 pfn;
static SECID section_id = { SEC$K_MATALL };
static GENERIC_64 region = { VA$C_P2 };
static unsigned __int64 del_address;
static unsigned __int64 byte_cnt;
static unsigned __int64 del_byte_cnt;

static int r0_status;
static unsigned int protection = 0xff00;
static unsigned int pfn_len = 1;
static unsigned int page_size;

static $DESCRIPTOR (name_d, "GPFN_DEMO_NAME");

    /*
    ** The HardWare Restart Parameter Block is loaded into a physical page
    ** (usually at physical address 2000(hex) on alphas) by the console
    ** subsystem at boot time.  The system also maps this page into virtual
    ** memory...
    **
    ** Get the virtual address of the HWRPB.
    */
    vir_hwrpb = EXE$GPL_HWRPB_L;

    /*
    ** Print out the ident, which should be "HWRPB" as a asciz string.
    */
    (void)printf (" System: %s\n", (char *)&vir_hwrpb->hwrpb$iq_ident);

    /*
    ** Get the size of a page of memory.
    */
    r0_status = lib$getsyi (&SYI$_PAGE_SIZE,
                            &page_size,
                            0,
                            0,
                            0,
                            0);
    errchk_sig (r0_status);

    /*
    ** Create a PFN mapped global section specifying the physical address
    ** found in the HWRPB.  The value at vir_hwrpb->hwrpb$pq_base is a
    ** physical address, so to obtain a PFN we just divide by the page size.
    ** Make the section one page long.
    */
    pfn = (__int64)vir_hwrpb->hwrpb$pq_base / page_size;
    r0_status = sys$create_gpfn (&name_d,
                                 &section_id,
                                 protection,
                                 pfn,
                                 pfn_len,
                                 PSL$C_USER,
                                 SEC$M_SYSGBL);
    if (r0_status != SS$_DUPLNAM) {
        /*
        ** Check for unexpected errors.  No problems if it already existed.
        */
        errchk_sig (r0_status);
    }

    /*
    ** Now map the section into P2 space so our process can access it.
    */
    r0_status = sys$mgblsc_gpfn_64 (&name_d,
                                    &section_id,
                                    &region,
                                    0,
                                    0,
                                    PSL$C_USER,
                                    SEC$M_SYSGBL|SEC$M_EXPREG,
                                    (void *)&map_hwrpb,
                                    &byte_cnt,
                                    0);
    errchk_sig (r0_status);

    /*
    ** Now demonstrate that we really mapped the physical page.
    ** We expect this to print "HWRPB".  Note we are referencing the page
    ** mapped to our process (map_hwrpb) verses the one mapped by the system
    ** (vir_hwrpb).
    */
    (void)printf ("Process: %s\n", (char *)&map_hwrpb->hwrpb$iq_ident);

    /*
    ** Delete the virtual address range from P2.
    */
    r0_status = sys$deltva_64 (&region,
                               map_hwrpb,
                               byte_cnt,
                               PSL$C_USER,
                               (void *)&del_address,
                               &del_byte_cnt);
    errchk_sig (r0_status);

    /*
    ** Delete the section.
    */
    r0_status = sys$dgblsc (SEC$M_SYSGBL,
                            &name_d,
                            &section_id);
    errchk_sig (r0_status);
}
