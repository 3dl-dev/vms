
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <starlet.h>


/******************************************************************************/
int main (void) {

#ifdef __VAX
#  error "Alpha or IA64 only code"
#endif /* __VAX */

static unsigned long int float_enabled;
static unsigned long int flags;


    float_enabled = sys$check_fen (&flags);

    (void)printf ("Floating point is %s\n",
                  float_enabled ? "enabled" : "disabled");
#ifdef __ia64
    (void)printf ("The low floating point bank is %s\n",
                  (flags & 1) ? "enabled" : "disabled");
    (void)printf ("The high floating point bank is %s\n",
                  (flags & 2) ? "enabled" : "disabled");
#endif /* __ia64 */
}
