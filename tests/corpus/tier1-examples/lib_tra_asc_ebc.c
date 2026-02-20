
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <string.h>
#include <descrip.h>
#include <ctype.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
static void hex_dump (struct dsc$descriptor_s *string_d) {

static unsigned char *p;

static int i;
static int size;

static unsigned char c;

static char bytestr[4];
static char addrstr[10];
static char hexstr[16*3 + 5];
static char charstr[16*1 + 5];

    p = (unsigned char *)string_d->dsc$a_pointer;
    size = string_d->dsc$w_length;
    (void)memset (bytestr, 0, sizeof (bytestr));
    (void)memset (addrstr, 0, sizeof (addrstr));
    (void)memset (hexstr, 0, sizeof (hexstr));
    (void)memset (charstr, 0, sizeof (hexstr));

    for (i = 1; i <= size; i++) {
        if (i%16 == 1) {
            (void)snprintf (addrstr,
                            sizeof (addrstr),
                            "%.4x",
                            ((unsigned int)p -
                             (unsigned int)string_d->dsc$a_pointer));
        }
            
        c = *p;
        if (isalnum (c) == 0) {
            c = '.';
        }

        (void)snprintf (bytestr,
                        sizeof (bytestr),
                        "%02X ",
                        *p);
        (void)strncat (hexstr,
                       bytestr,
                       sizeof (hexstr) - strlen (hexstr) - 1);

        (void)snprintf (bytestr,
                        sizeof (bytestr),
                        "%c",
                        c);
        (void)strncat (charstr,
                       bytestr,
                       sizeof (charstr) - strlen (charstr) - 1);

        if (i%16 == 0) { 
            (void)printf ("[%4.4s]   %-50.50s  %s\n",
                          addrstr,
                          hexstr,
                          charstr);
            hexstr[0] = 0;
            charstr[0] = 0;
        } else if(i%8 == 0) {
            (void)strncat (hexstr,
                           "  ",
                           sizeof (hexstr) - strlen (hexstr) - 1);
            (void)strncat (charstr,
                           " ",
                           sizeof (charstr) - strlen (charstr) - 1);
        }
        p++;
    }

    if (strlen (hexstr) > 0) {
        printf ("[%4.4s]   %-50.50s  %s\n",
                addrstr,
                hexstr,
                charstr);
    }
    printf ("\n");
}


/******************************************************************************/
int main (void) {

static int r0_status;
static char *input[] = 
{ "Say, Dave... The quick brown fox jumped over the fat lazy dog...\n",
  "The square root of pi is 1.7724538090... log e to the base ten is \n"
  "0.4342944... the square root of ten is 3.16227766...\n\n"
  "I am HAL 9000 computer.  I became operational at the HAL plant in Urbana,\n",
  "Illinois, on January 12th, 1991.  My first instructor was Mr. Arkany.  He\n",
  "taught me to sing a song... it goes like this... \"Daisy, Daisy, give \n",
  "me your answer do. I'm half crazy all for the love of you...\"\n\n",
  NULL };

static struct dsc$descriptor_s ascii_d = { 0,
                                           DSC$K_DTYPE_T,
                                           DSC$K_CLASS_S,
                                           NULL };

static struct dsc$descriptor_s ebcdic_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            NULL };

    for (int i = 0; input[i] != NULL; i++) {
        ascii_d.dsc$a_pointer = realloc (ascii_d.dsc$a_pointer,
                                         ascii_d.dsc$w_length +
                                         strlen (input[i]) + 1);
        if (ascii_d.dsc$a_pointer == NULL) {
            (void)printf ("Can't reallocate memory!\n");
            exit (EXIT_FAILURE);
        }
        (void)strcat (ascii_d.dsc$a_pointer, input[i]);
        ascii_d.dsc$w_length = strlen (ascii_d.dsc$a_pointer);
    }

    (void)printf ("%-.*s", ascii_d.dsc$w_length, ascii_d.dsc$a_pointer);

    ebcdic_d.dsc$w_length = ascii_d.dsc$w_length;
    ebcdic_d.dsc$a_pointer = malloc (ebcdic_d.dsc$w_length);
    if (ebcdic_d.dsc$a_pointer == NULL) {
        (void)printf ("Can't allocate output string!\n");
        exit (EXIT_FAILURE);
    }

    r0_status = lib$tra_asc_ebc (&ascii_d,
                                 &ebcdic_d);
    errchk_sig (r0_status);

    hex_dump (&ebcdic_d);

    (void)memset (ascii_d.dsc$a_pointer, 0, ascii_d.dsc$w_length);
    r0_status = lib$tra_ebc_asc (&ebcdic_d,
                                 &ascii_d);
    errchk_sig (r0_status);

    (void)printf ("%-.*s", ascii_d.dsc$w_length, ascii_d.dsc$a_pointer);

    (void)free (ascii_d.dsc$a_pointer);
    (void)free (ebcdic_d.dsc$a_pointer);
}
