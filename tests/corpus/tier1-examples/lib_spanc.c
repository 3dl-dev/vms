
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <descrip.h>
#include <lib$routines.h>


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

static unsigned long int offset;

static char quote[] = "Outside a dog a book is man's best friend; "
                      "Inside a dog it's too dark to read.";

static struct dsc$descriptor_s quote_d = { sizeof (quote) - 1,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           quote };

static char span_chars[] = "Outsi";
#define NUM_SPAN_CHARS sizeof (span_chars) - 1
#define NUM_CHARS 256
static unsigned char table[NUM_CHARS];
static int i;
static unsigned char mask;

    (void)memset (table, 0, NUM_CHARS);
    for (i = 0; i < NUM_SPAN_CHARS; i++) {
       table[span_chars[i]] = 1;
    }

    mask = 1;
    offset = lib$spanc (&quote_d, table, &mask);
    (void)printf ("%-.*s\n", quote_d.dsc$w_length, quote_d.dsc$a_pointer);
    display_offset (offset);
}
