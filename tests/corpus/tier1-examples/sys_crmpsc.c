
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <assert.h>
#include <string.h>
#include <descrip.h>
#include <secdef.h>
#include <efndef.h>
#include <rms.h>
#include <va_rangedef.h>
#include <iosbdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

/*
** Note this is pages on a VAX and pagelets on an Alpha.
*/
#define SECTION_SIZE 16


/******************************************************************************/
int main (void) {
/*
** This program demonstrates creating and mapping a file into memory.  This
** is a fairly simplistic example: it just maps the file, puts or changes
** data in the file, and writes it out to disk.
**
** You can create a named section (as we are doing below), and share the
** section between co-operating processes.  There is a stripped down example
** of this in the "OpenVMS Programming Concepts Manual".
**
*/

static struct FAB fab;

static IOSB iosb;

static VA_RANGE in_addr;
static VA_RANGE out_addr;
static VA_RANGE del_addr;
static int r0_status;
static const unsigned int prot = 0x00FF;

static char *p;

static int i;
static int j;

static char filename[] = "SYS_CRMPSC.DEMO";
static struct dsc$descriptor_s filename_d = { sizeof (filename) - 1,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              filename };

static $DESCRIPTOR (section_name_d, "SYS_CRMPSC_DEMO");

static char junk[80];
static char add;
static char created;

    /*
    ** Set up a File Access Block to create or open a file that will back
    ** our section.  If the file already exists we will open it and attempt
    ** to use it.
    **
    ** Note we need to specify user file open.  While non-contiguous files
    ** will work, specifying contiguous best try is a good idea for
    ** performance.  Note the file access specifies put, so the file will
    ** be writeable.
    */
    fab = cc$rms_fab;
    fab.fab$b_fns = strlen (filename);
    fab.fab$l_fna = filename;
    fab.fab$l_alq = SECTION_SIZE;
    fab.fab$l_fop = FAB$M_CIF|FAB$M_UFO|FAB$M_CBT;
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$b_rtv = -1;

    /*
    ** Open or create the file.  If the call succeeds, we have a channel
    ** number assigned to it available in the first word of the fab$l_stv
    ** field of the fab.
    */
    r0_status = sys$create (&fab);
    switch (r0_status) {
        case RMS$_NORMAL:
            /*
            ** We successfully opened an existing file.  Say what's happening.
            */
            (void)printf ("You appear to have a file called %-.*s already.\n"
                          "Trying to use it... ",
                          filename_d.dsc$w_length,
                          filename_d.dsc$a_pointer);
            (void)fflush (stdout);

            /*
            ** Obviously, to use the file to back some memory, it has to be
            ** large enough.  If the $create call opened an existing file, it
            ** performed an implicit $display to get the file's attributes,
            ** including the file size.  Check that it's large enough and
            ** abort if it's not.
            */
            if (fab.fab$l_alq < SECTION_SIZE) {
                (void)printf ("can't.  It's not large enough.  Abort.\n");
                exit (EXIT_FAILURE);
            } else {
                /*
                ** The file is large enough.  Make sure that the user knows
                ** We are about to write junk all over his file and give him
                ** a chance to abort the program with a control-y or something.
                */
                (void)printf ("OK\n");
                (void)printf ("I'm assuming this file is from a previous run "
                              "of this program.\nIf it's not, the file is "
                              "about to be overwritten!  Abort now if so!\n");
                (void)printf ("Hit <RETURN> to overwrite: ");
                (void)fflush (stdout);
                (void)fgets (junk, sizeof (junk), stdin);
            }
            created = FALSE;
            break;
        case RMS$_CREATED:
            (void)printf ("Created file %-.*s to back the section.\n",
                          filename_d.dsc$w_length,
                          filename_d.dsc$a_pointer);
            created = TRUE;
            break;
        default:
            errchk_sig (r0_status);
            break;
    }

    /*
    ** Map the file input P0 space.  We specify that the section we are
    ** creating is writeable.  The EXPREG flag asks the routine to allocate
    ** memory for us.
    */
    r0_status = sys$crmpsc (&in_addr,
                            &out_addr,
                            0,
                            SEC$M_EXPREG|SEC$M_WRT,
                            &section_name_d,
                            0,
                            0,
                            fab.fab$l_stv,
                            SECTION_SIZE,
                            0,
                            prot,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Section mapped from address %08x to %08x\n",
                  out_addr.va_range$ps_start_va,
                  out_addr.va_range$ps_end_va);

    /*
    ** Now screw around with the data in the section.
    */
    p = (char *)out_addr.va_range$ps_start_va;

    add = 'A';
    if (!created) {
        add = (*p == 'A' ? 'a' : 'A');
    }

    for (i = 0; i < SECTION_SIZE; i++) {
        for (j = 0; j < 512; j++) {
            *p++ = i + add;
        }
    }

    /*
    ** Write the contents of memory to the file that backs our section.
    ** the forth parameter specifies that if we didn't update the page in
    ** memory, we don't need to update it on disk.
    */
    r0_status = sys$updsecw (&out_addr,
                             0,
                             0,
                             1,
                             EFN$C_ENF,
                             &iosb,
                             0,
                             0);
    errchk_sig (r0_status);

    /*
    ** Check we didn't get an error writing the file.
    */
    if ((iosb.iosb$w_bcnt & 1) != 0 ||
        !$VMS_STATUS_SUCCESS (iosb.iosb$w_status)) {
        (void)printf ("An error occured updating the section!\n"
                      "The first address not written was %08x\n",
                      iosb.iosb$l_dev_depend);
    }

    /*
    ** Delete the memory associated with the section.
    */
    r0_status = sys$deltva (&out_addr,
                            &del_addr,
                            0);
    errchk_sig (r0_status);

    (void)printf ("Deleted memory from address %08x to %08x\n",
                  del_addr.va_range$ps_start_va,
                  del_addr.va_range$ps_end_va);

    /*
    ** Deassign the channel that RMS opened for us in the $create call.
    */
    r0_status = sys$dassgn (fab.fab$l_stv);
    errchk_sig (r0_status);

    (void)printf ("You could now use the DCL command\n\n$ DUMP %-.*s\n\nto "
                  "display the contents of the file.\n",
                  filename_d.dsc$w_length,
                  filename_d.dsc$a_pointer);
}
