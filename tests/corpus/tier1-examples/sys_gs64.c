
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <syidef.h>
#include <secdef.h>
#include <vadef.h>
#include <psldef.h>
#include <efndef.h>
#include <iosbdef.h>
#include <seciddef.h>
#include <gen64def.h>
#include <iledef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** This program requires that you have PRMGBL privilege and hold identifier
** VMS$MEM_RESIDENT_USER.  You must compile the program with the /POINTER_SIZE
** qualifier (it doesn't matter if you choose 32 or 64).
*/

#ifdef __VAX
#  error "64 bit integers required"
#endif /* __VAX */

#pragma pointer_size save
#pragma pointer_size 64

static void *retadr_1;
static void *retadr_2;
static void * delretadr;
static GENERIC_64 retlen_1;
static GENERIC_64 retlen_2;
static GENERIC_64 delretlen;
static GENERIC_64 length;

static GENERIC_64 region = { VA$C_P2 };

static IOSB iosb;

static int r0_status;
static unsigned int page_size;
static unsigned int flags = SEC$M_PERM | SEC$M_EXPREG | SEC$M_WRT;

static SECID ident = { SEC$K_MATALL, 0 };
static const unsigned int prot = 0x0000FF00;
static const unsigned int acmode = PSL$C_USER;

static char *p;

static int i;

#pragma pointer_size 32
static ILE3 syiitms[] = { 4, SYI$_PAGE_SIZE, &page_size, NULL,
                          0, 0, NULL, NULL };

static $DESCRIPTOR (section_name_d, "TEST_SECTION");
#pragma pointer_size restore

    /*
    ** Get the CPU specific page size.
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
    ** Let's create a section 2 pages long.
    */
    length.gen64$q_quadword = page_size * 2;
    (void)printf ("Section will be %Ld bytes in length\n",
                  length.gen64$q_quadword);

    /*
    ** Map a demand zero global section in P2 space.  Note that this is a
    ** permanent section.
    */
    r0_status = sys$crmpsc_gdzro_64 (&section_name_d,
                                     &ident,
                                     prot,
                                     length.gen64$q_quadword,
                                     &region,
                                     0,
                                     acmode,
                                     flags,
                                     &retadr_1,
                                     &retlen_1.gen64$q_quadword,
                                     0,
                                     0,
                                     0,
                                     0);
    errchk_sig (r0_status);

    (void)printf ("Created group permanent global section at address %Lx\n",
                  retadr_1);

    /*
    ** Clear the perm flag as it only makes sense to specify this while
    ** creating the section.
    */
    flags &= ~SEC$M_PERM;

    /*
    ** "Double map" the section, giving us two linked copies of this in
    ** our virtual address space.  I can't think of a good reason to actually
    ** do this off hand.  Normally, cooperating processes would create
    ** and map a section for shared storage.  The first process to call
    ** SYS$CRMPSC would create the section, and subsequent callers would map
    ** it.  This is a demo, however.
    */
    r0_status = sys$mgblsc_64 (&section_name_d,
                               &ident,
                               &region,
                               0,
                               length.gen64$q_quadword,
                               acmode,
                               flags,
                               &retadr_2,
                               &retlen_2.gen64$q_quadword,
                               0);
    errchk_sig (r0_status);

    (void)printf ("Mapped group permanent global section at address %Lx\n",
                  retadr_2);

    /*
    ** Fill the global section with an appropriate pattern.
    */
    p = retadr_1;
    for (i = 0; i < retlen_1.gen64$q_quadword; i += 8) {
        (void)memcpy (&p[i], "DEADBEEF", 8);
    }

    /*
    ** Prove that we have double mapped the section.
    */
    p = retadr_2;
    for (i = 0; i < retlen_2.gen64$q_quadword; i += 8) {
        if (memcmp (&p[i], "DEADBEEF", 8) != 0) {
            (void)fprintf (stderr, "Not double mapped at all!\n");
        }
    }

    /*
    ** Mark the permanent global section for deletion.  Note that it is
    ** not deleted until all processes mapped to it delete the virtual
    ** address ranges associated with the section.
    */
    r0_status = sys$dgblsc (0,
                            &section_name_d,
                            &ident);
    errchk_sig (r0_status);

    (void)printf ("Marked section for deletion\n");

    /*
    ** Now delete the virtual address ranges mapped to the section.
    */
    r0_status = sys$deltva_64 (&region,
                               retadr_1,
                               retlen_1.gen64$q_quadword,
                               acmode,
                               &delretadr,
                               &delretlen.gen64$q_quadword);
    errchk_sig (r0_status);

    r0_status = sys$deltva_64 (&region,
                               retadr_2,
                               retlen_2.gen64$q_quadword,
                               acmode,
                               &delretadr,
                               &delretlen.gen64$q_quadword);
    errchk_sig (r0_status);

    (void)printf ("Deleted virtual address ranges\n");
}
