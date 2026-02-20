
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

/*
** Must not optimize the code away, or no error will occur :-)
*/
#pragma optimize level=0

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <chfdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifndef __ALPHA
#  error "Alpha specific code"
#endif /* __ALPHA */


/******************************************************************************/
static int error_handler (struct chf$signal_array *sig,
                          struct chf$mech_array *mech) {

static __int64 vec[4];

static struct dsc$descriptor vec_d = { sizeof (vec),
                                       0,
                                       0,
                                       (char *)vec };
static int r0_status;
static int opc;
static int func;
static int ra;
static int rc;
static int i;
static __int64 mask;

static char *exsum[] = {
    "Software completion",
    "Invalid floating arithmetic, conversion, or comparison operation",
    "Invalid attempt to perform a floating divide operation with a divisor \n"
    "of zero. Note that integer divide-by-zero is not reported",
    "Floating arithmetic or conversion operation overflowed the destination \n"
    "exponent",
    "Floating arithmetic or conversion operation underflowed the destination \n"
    "exponent",
    "Floating arithmetic or conversion operation gave a result that differed \n"
    "from the mathematically exact result",
    "Integer arithmetic or conversion operation from floating point to \n"
    "integer overflowed the destination precision" };

    if (sig->chf$is_sig_name == SS$_HPARITH) {
        /*
        ** The signal we are expecting.
        */
        (void)printf ("Caught high performance arithmetic error\n");
        r0_status = sys$get_arith_exception (sig,
                                             mech,
                                             &vec_d);
        errchk_sig (r0_status);
        (void)printf ("The failing PC is 0x%llX\n",
                      vec[0]);

        /*
        ** Decode the instruction.  We know it's an operate instruction as
        ** these are the only ones that can generate the error.
        */
        opc = (vec[1] >> 26) & 0x3f;
        func = (vec[1] >> 5) & 0x7f;
        ra = (vec[1] >> 21) & 0x1F;
        rc = vec[1] & 0x1F;
        (void)printf ("The failing opcode is %x.%03x with ra = R%d and "
                      "rc = R%d\n",
                      opc,
                      func,
                      ra,
                      rc);

        (void)printf ("The exception summary indicates:\n%s\n",
                      exsum[vec[2]]);

        (void)printf ("The following registers were involved:\n");
        for (i = 0; i < 64; i++) {
            mask = 1;
            mask = mask << i;
            if ((vec[3] & mask) != 0) {
                (void)printf ("%s%d\n",
                              i < 32 ? "R" : "F",
                              i < 32 ? i : i - 32);
            }
        }
        return SS$_CONTINUE;
    }
    return SS$_RESIGNAL;
}


/******************************************************************************/
int main (void) {

static int x;
static int y = 0;

    (void)lib$establish (error_handler);

    x = 1 / y;
}
