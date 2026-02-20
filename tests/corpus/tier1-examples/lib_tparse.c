
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <descrip.h>
#include <tpadef.h>
#include <libdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
extern int VALIDATE_GRP (TPADEF *tparse_blk) {
/*
** UIC group numbers are 1 to 37776 octal.
*/

    if ((tparse_blk->tpa$l_number > 0) &&
        (tparse_blk->tpa$l_number < 037777)) {
        return SS$_NORMAL;
    } else {
        return LIB$_SYNTAXERR;
    }
}


/******************************************************************************/
extern int VALIDATE_MBR (TPADEF *tparse_blk) {
/*
** UIC member numbers are 0 to 177776 octal.
*/

    if (tparse_blk->tpa$l_number < 0177777) {
        return SS$_NORMAL;
    } else {
        return LIB$_SYNTAXERR;
    }
}


/******************************************************************************/
int main (int argc, char *argv[]) {
/*
** This example demonstrates LIB$TABLE_PARSE (), a finite state parser.  The
** difficulty with this example is that we need a MACRO-32 module to implement
** the state and key (symbol) tables...
**
** To compile and link this module, you need to extract the following code
** into a module called TPARSE.MAR, assemble it and link it with this code.
** Here is the MACRO-32 code.  Extract immediately after this line:

    $TPADEF

COMMA = 44

    $INIT_STATE UIC_STATE, UIC_KEY

    $STATE START
    $TRAN '['
    $STATE
    $TRAN TPA$_OCTAL,,VALIDATE_GRP
    $STATE
    $TRAN COMMA
    $STATE
    $TRAN TPA$_OCTAL,,VALIDATE_MBR
    $STATE
    $TRAN ']',TPA$_EXIT
    $TRAN TPA$_LAMBDA, TPA$_FAIL

    $END_STATE
    .END

** and finish extraction immediately before this line.
**
** Now to compile and link, you should do:
**
**        $ macro tparse.mar
**        $ cc lib_tparse.c
**        $ link lib_tparse.obj, tparse.obj
**
** The parser expects to be handed a string representing a UIC.  This means
** that the string should consist of two octal numbers surrounded by square
** brackets and separated by a comma.  For example, [301,1].  Ommission of the
** brackets or comma, or use of numbers outside the range 0-7 inclusive will
** result in a syntax error.  Here is an example run:
**
**        $ mc []lib_tparse [1,10]
**        [1,10] is valid syntax
**        $ mc []lib_tparse [9,10]
**        [9,10] is invalid syntax
**
** (Please realise I do know I could have used TPA$_UIC in the MACRO-32 code.
** This *is* a demo, remember?)
**
*/

extern unsigned int UIC_STATE, UIC_KEY;
static int r0_status;
static TPADEF tparse_blk = { TPA$K_COUNT0, TPA$M_BLANKS };
static struct dsc$descriptor_s uic_d = { 0,
                                         DSC$K_DTYPE_T,
                                         DSC$K_CLASS_S,
                                         NULL };


    if (argc < 2) {
        (void)printf ("Usage: mc []lib_tparse [grp,mbr]\n");
        exit (EXIT_FAILURE);
    }

    /*
    ** Let's save off the length and pointer as the parser screws around with
    ** the ones in the tparse block.
    */
    uic_d.dsc$w_length = strlen (argv[1]);
    uic_d.dsc$a_pointer = argv[1];

    tparse_blk.tpa$l_stringcnt = uic_d.dsc$w_length;
    tparse_blk.tpa$l_stringptr = uic_d.dsc$a_pointer;

    r0_status = lib$table_parse ((unsigned int *)&tparse_blk,
                                 &UIC_STATE,
                                 &UIC_KEY);
    (void)printf ("%-.*s is ",
                  uic_d.dsc$w_length,
                  uic_d.dsc$a_pointer);
    if (r0_status == LIB$_SYNTAXERR) {
        (void)printf ("in");
    } else {
        errchk_sig (r0_status);
    }
    (void)printf ("valid syntax\n");
}
