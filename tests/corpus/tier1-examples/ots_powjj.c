
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <ots$routines.h>


/******************************************************************************/
int main (void) {
/*
** I'm not going to write an example for every one of the OTS$POWxxx funtions
** as they are all the same with the exception of what data types they
** operate on.  It should be easy to figure out based on this example.
*/

static int r;
static int b = 8;
static int e = 3;

    r = ots$powjj (b, e);

    (void)printf ("eight cubed = %d\n",
                  r);

}
