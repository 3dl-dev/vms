
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {
/*
** Demo $FAO () and $FAOL ().  Results for the two calls will be exactly the
** same.  Note that I have only touched on the number of directives that
** are supported by these system services.  See the documentation for the
** entire list.
*/

#ifdef __VAX
#  error "64 bit integers required"
#endif /* __VAX */

static unsigned __int64 blkq = 923598753923258587;

static int r0_status;
static unsigned int blkl = 1894784930;

static unsigned short int blkw = 18983;

static unsigned char blkb = 155;

static struct {
    unsigned char length;
    char string[15];
} ascic = { 14, "Counted string" };

static $DESCRIPTOR (ascid, "Descriptor");

static char output[255];

static struct dsc$descriptor_s output_d = { sizeof (output),
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            output };
static $DESCRIPTOR (ctrstr_d,
        "!3*> !%D!/!AC!/!AS!/!@XQ!/!OL (octal)!/!UW (decimal)!/!ZB (decimal)");

/*
** Note, the last three values in the list will be filled in at run time as
** they are not constant at compile time.
*/
static unsigned int list[7] = {
        0,
        (unsigned int)&ascic,
        (unsigned int)&ascid,
        (unsigned int)&blkq,
        0, 0, 0 };

    /*
    ** First, $FAO ().
    */
    r0_status = sys$fao (&ctrstr_d,
                         &output_d.dsc$w_length,
                         &output_d,
                         0,
                         &ascic,
                         &ascid,
                         &blkq,
                         blkl,
                         blkw,
                         blkb);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);

    /*
    ** Now $FAOL ().  First fill in the last three values in the input list
    ** and reset the max length of the output string.
    */
    list[4] = blkl;
    list[5] = blkw;
    list[6] = blkb;
    output_d.dsc$w_length = sizeof (output);

    r0_status = sys$faol (&ctrstr_d,
                          &output_d.dsc$w_length,
                          &output_d,
                          list);
    errchk_sig (r0_status);

    (void)printf ("%-.*s\n",
                  output_d.dsc$w_length,
                  output_d.dsc$a_pointer);
}
