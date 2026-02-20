
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <lib$routines.h>

#define STR_LEN 3


/******************************************************************************/
int main (void) {

static char source[STR_LEN+1] = "ABC";
static char dest[STR_LEN+1] = "XXX";
static unsigned short int len = STR_LEN;

    lib$movc3 (&len,
               (unsigned int *)&source[0],
               (unsigned int *)&dest[0]);

    (void)printf ("%s",
                  dest);
}
