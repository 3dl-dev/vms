
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned long int len;

static $DESCRIPTOR (test_d, "thequickbrownfoxjumpsoverthelazydog");

    len = lib$len (&test_d);

    (void)printf ("Length is %u\n",
                  len);
}
