
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

static unsigned int pos;
static int i = -1;

static $DESCRIPTOR (sub1_d, "PLUGH");
static $DESCRIPTOR (sub2_d, "XYZZY");
static $DESCRIPTOR (str_d, "A note on the wall says MAGIC WORD XYZZY.");

static struct dsc$descriptor_s *subs[] = { &sub1_d, &sub2_d, NULL };

    while (subs[++i] != NULL) {  
        pos = lib$index (&str_d,
                         subs[i]);
        if (pos == 0) {
            (void)printf ("String %-.*s not found\n",
                          subs[i]->dsc$w_length,
                          subs[i]->dsc$a_pointer);
        } else {
            (void)printf ("String %-.*s found at position %u\n",
                          subs[i]->dsc$w_length,
                          subs[i]->dsc$a_pointer,
                          pos);
        }
    }
}
