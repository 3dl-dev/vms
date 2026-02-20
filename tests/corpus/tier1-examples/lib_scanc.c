
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
static void display_offset (int offset) {

    if (offset == 0) {
        (void)printf ("No match\n");
    } else {
        offset--;
        for (int i = 0; i < offset; i++) {
            (void)printf ("-");
        }
        (void)printf ("^    (offset = %d)\n", offset);
    }
}


/******************************************************************************/
int main (void) {

static int r0_status;
static unsigned int offset;
static $DESCRIPTOR (str_d, "the quick brown fox jumps over the lazy dog");
static char stop_chars[] = { 'a', 'b' };
#define NUM_STOP_CHARS sizeof (stop_chars)
#define NUM_CHARS 256
static unsigned char table[NUM_CHARS];
static int i;
static unsigned char mask;

    memset (table, 0, NUM_CHARS);
    for (i = 0; i < NUM_STOP_CHARS; i++) {
       table[stop_chars[i]] = 1;
    }

    /*
    ** Set the mask value to 1 so that an AND of the values in the table (1)
    ** will produce a result of 1.
    */
    mask = 1;
    offset = lib$scanc (&str_d, table, &mask);
    (void)printf ("%-.*s\n", str_d.dsc$w_length, str_d.dsc$a_pointer);
    display_offset (offset);

    /*
    ** Note now we change the value in the mask to 2, so an AND will fail.
    ** You can use various values in the mask byte and the table to match
    ** different sets of characters.
    */
    mask = 2;
    offset = lib$scanc (&str_d, table, &mask);
    (void)printf ("%-.*s\n", str_d.dsc$w_length, str_d.dsc$a_pointer);
    display_offset (offset);
}
