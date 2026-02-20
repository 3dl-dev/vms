
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>


/******************************************************************************/
int main (void) {

/*
** lib$stop () is a little more terminal than lib$signal ().
** Demo by signalling a warning that lib$stop () will convert
** to a fatal.
*/

    (void)lib$signal (SS$_NOMORENODE);

    (void)printf ("Notice we continued on, even though we produced "
                  "a traceback\n");

    (void)lib$stop (SS$_NOMORENODE);

    (void)printf ("But there's no way we're getting here\n");
}
