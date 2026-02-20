
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <lib$routines.h>

#define SRC_STR_LEN 3
#define DST_STR_LEN 5


/******************************************************************************/
int main (void) {

static char source[SRC_STR_LEN+1] = "ABC";
static char dest[DST_STR_LEN+1] = "XXXXX";
static unsigned short int src_len = SRC_STR_LEN;
static unsigned short int dst_len = DST_STR_LEN;
static char fill = '*';

    lib$movc5 (&src_len,
               (unsigned int *)&source[0],
               &fill,
               &dst_len,
               (unsigned int *)&dest[0]);

    (void)printf ("%s",
                  dest);
}
