
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <lib$routines.h>

#include "errchk.h"

#ifdef __VAX
#  error "Alpha or IA64 only code"
#endif


/******************************************************************************/
int main (void) {

/*
** This program demonstrates lib$subx () which will subtract multiple precision
** binary integers.  The program uses a 64 bit int to do the demo, as it's
** easy to show that the results are the same.  Note that the arrays
** input to lib$subx can be any size.
*/

static int r0_status;
static unsigned int array1[2];
static unsigned int array2[2];
static unsigned int array3[2];

static int array_length = 2;

static __int64 check1;
static __int64 check2;
static __int64 check3;

    check1 = 0x0000444400003333;
    check2 = 0x0000222200001111;

    array1[0] = 0x00003333;
    array1[1] = 0x00004444;
    array2[0] = 0x00001111;
    array2[1] = 0x00002222;

    check3 = check1 - check2;

    r0_status = lib$subx (array1,
                          array2,
                          array3,
                          &array_length);
    errchk_sig (r0_status);

    if (memcmp (array3, &check3, 8) == 0) {
        (void)printf ("Results are equal\n");
    } else {
	(void)printf ("Results are not equal\n");
    }
}
