
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <descrip.h>
#include <ots$routines.h>


/******************************************************************************/
int main (void) {

#define DESC_CNT 8

static char *p;

static int bytes;
static int i;

static struct dsc$descriptor_d array[DESC_CNT];

static char source[] = "To really screw things up requires a computer";

    for (i = 0; i < DESC_CNT; i++) {
        ots$sget1_dd (255,
                      (unsigned __int64 *)&array[i]);
        array[i].dsc$b_dtype = DSC$K_DTYPE_T;
        array[i].dsc$b_class = DSC$K_CLASS_D;
        p = strtok (i == 0 ? source : NULL, " ");
        bytes = ots$scopy_r_dx (strlen (p),
                                p,
                                &array[i]);
        if (bytes != 0) {
            (void)fprintf (stderr, "Something went wrong!\n");
            exit (EXIT_FAILURE);
        }
        (void)printf ("\"%-.*s\"\n",
                      array[i].dsc$w_length,
                      array[i].dsc$a_pointer);
    }

    ots$sfreen_dd (DESC_CNT,
                   (unsigned __int64 *)&array[0]);
}
