
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <lib$routines.h>

#include "errchk.h"

/*
** Set up some macros to make the declaration of a variable string easy.
*/
#define MAX_LEN 250

#define VS_STRING(name, len) \
struct { \
    unsigned short length; \
    char string[len]; \
    struct dsc$descriptor_vs desc; \
} name = {0, "", {len, DSC$K_DTYPE_VT, DSC$K_CLASS_VS, (char*)&name.length}}

#define STRING(name) VS_STRING(name, MAX_LEN)


/******************************************************************************/
int main (void) {
/*
** OK, this is an extremely contrived example.  The idea it to demo
** LIB$FIND_IMAGE_SYMBOL, which is basically a method of doing dynamic
** linking on OpenVMS.
**
** LIB$FIND_IMAGE_SYMBOL allows you to find the address of a universal
** symbol in a shareable image, then dynamically map the shareable image
** into process address space, so you can call the routine whose address
** you just found.
**
** As usual, all this is discussed in the "Programming Concepts Manual".
** It's worthwile to take the time to understand shareable images. They're
** extremely powerful.
**
** Back to the reason this example is contrived.  To demonstrate
** LIB$FIND_IMAGE_SYMBOL, we need a shareable image to load.  I could write
** two bits of code, and give you instructions on how to create a shareable
** image from one, and blah, blah, blah.  But there are plenty of shareable
** images delivered with the operating system.  So we will call one of them.
** One that I know will be on your system: the BASIC run time library.
**
** First reason why it's a contrived example: I could call upcase () without
** having to resort to the variable string descriptor stuff I need to call
** a BASIC routine.  Second reason: I could just link the BASIC run time
** library in at link time and avoid calling LIB$FIND_IMAGE_SYMBOL at all ;-)
**
*/

static int edit_routine;

static int r0_status;
static unsigned int arg_list[4];

static $DESCRIPTOR (filename_d, "DEC$BASRTL");
static $DESCRIPTOR (symbol_d, "DBASIC$EDIT");

static STRING (instring);
static STRING (outstring);

    /*
    ** Find the address of DBASIC$EDIT, a routine that allows BASIC programmers
    ** to do things like uppercase strings.  Note that the name of the
    ** shareable image and symbol may vary on older versions of OpenVMS.  This
    ** certainly works on an Alpha running 7.3.  If this fails, you should
    ** be easily able to figure out the name of the shareable image: it will
    ** contain "BAS" and "RTL" in some combination, and live in SYS$SHARE:
    ** Once you have found it, you can do an $ ANALYSE/IMAGE to find the name
    ** of the universal symbol.
    */
    r0_status = lib$find_image_symbol (&filename_d,
                                       &symbol_d,
                                       &edit_routine,
                                       0,
                                       0);
    errchk_sig (r0_status);

    /*
    ** This will be the input string.  Note "test" is in lower case.
    */
    instring.length = sprintf (instring.string, "test 1 2 3");

    /*
    ** Set up the argument list for lib$callg ().  32 means "uppercase the
    ** input string".  For the BASIC programmers, I'm desperate to code it
    ** as "32%", but I'll resist ;-)
    */
    arg_list[0] = 3;
    arg_list[1] = (unsigned int)&outstring.desc;
    arg_list[2] = (unsigned int)&instring.desc;
    arg_list[3] = 32;

    /*
    ** Call the routine whose address we found.
    */
    r0_status = lib$callg (arg_list,
                           (int (*)())edit_routine);
    errchk_sig (r0_status);

    /*
    ** Show we really did the business.  Note "test" should now be upper
    ** case.
    */
    (void)printf ("%-.*s\n",
                  outstring.length,
                  outstring.desc.dsc$a_pointer+2);
}
