
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <nsadef.h>
#include <rms.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static void usage (void) {
/*
** This program reads the audit log SYS$MANAGER:SECURITY.AUDIT$JOURNAL by
** default, although you can specify a different file by invoking the program
** via a foreign symbol and specifying the location.  The audit log file is
** by default owned by SYSTEM and protected from world read.  Therefore you
** need either SYSPRV, READALL, or BYPASS privilege to run this code.
*/

    (void)printf ("Usage: SYS_FORMAT_AUDIT [filename]\n\n"
                  "filename is the name of an audit log file.  If not\n"
                  "specified, defaults to\n"
                  "SYS$MANAGER:SECURITY.AUDIT$JOURNAL\n");
}


/******************************************************************************/
int main (int argc, char *argv[]) {

static struct FAB fab;
static struct RAB rab;

static int r0_status;

static char def_file[] = "SYS$MANAGER:SECURITY.AUDIT$JOURNAL";
static char buffer[32767];
static char output[32767];

static $DESCRIPTOR (term_d, "\n");
static struct dsc$descriptor_s output_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };
static char finished = FALSE;

    fab = cc$rms_fab;
    fab.fab$l_fop = FAB$M_GET;
    fab.fab$b_shr = FAB$M_SHRGET|FAB$M_SHRPUT|FAB$M_SHRUPD|FAB$M_SHRDEL;
    if (argc < 2) {
        fab.fab$b_fns = strlen (def_file);
        fab.fab$l_fna = def_file;
    } else {
        fab.fab$b_fns = strlen (argv[1]);
        fab.fab$l_fna = argv[1];
    }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$w_rsz = sizeof (buffer);
    rab.rab$l_rbf = buffer;
    rab.rab$w_usz = sizeof (buffer);
    rab.rab$l_ubf = buffer;

    r0_status = sys$open (&fab);    
    switch (r0_status) {
        case RMS$_FNF :
            (void)printf ("Can't find file %-.*s : abort\n",
                          fab.fab$b_fns,
                          fab.fab$l_fna);
            usage ();
            break;
        case RMS$_PRV :
            (void)printf ("No priv to open file %-.*s : abort\n",
                          fab.fab$b_fns,
                          fab.fab$l_fna);
            usage ();
            break;
        default :
            errchk_sig (r0_status);
    }

    r0_status = sys$connect (&rab);
    errchk_sig (r0_status);

    while (!finished) {
        r0_status = sys$get (&rab);
        if (r0_status == RMS$_EOF) {
            finished = TRUE;
        } else {
            errchk_sig (r0_status);
            buffer[rab.rab$w_rsz] = '\0';
            output_d.dsc$w_length = sizeof (output) - 1;
            r0_status = sys$format_audit (NSA$C_FORMAT_STYLE_FULL,
                                          (unsigned int *)&buffer[0],
                                          &output_d.dsc$w_length,
                                          &output_d,
                                          0,
                                          &term_d,
                                          0,
                                          1);
            errchk_sig (r0_status);
            (void)printf ("%-.*s\n\n",
                          output_d.dsc$w_length,
                          output_d.dsc$a_pointer);

        }
    }

    r0_status = sys$close (&fab);
    errchk_sig (r0_status);
}
