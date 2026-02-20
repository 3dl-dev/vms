
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <ots$routines.h>


/******************************************************************************/
int main (void) {

static unsigned char str1[] = "Thequickbrownfoxjumpsoverthelazydog";
static unsigned char str2[sizeof (str1)];
static unsigned char str3[sizeof (str1)+10];
static int i;


    /*
    ** Ensure output strings are null.
    */
    for (i = 0; i < sizeof (str2); i++) {
        str2[i] = str3[i] = 0;
    }

    for (; i < sizeof (str3); i++) {
        str3[i] = 0;
    }

    ots$move3 (sizeof (str1), str1, str2);

    (void)printf ("%s\n", str2);

    /*
    ** For move3, it didn't matter we included the trailing null of str1.
    ** This time we have to skip it or we printf will terminate on it.
    */
    ots$move5 (sizeof (str1) - 1, str1, '.', sizeof (str3), str3);

    (void)printf ("%s\n", str3);
}
