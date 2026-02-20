
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lib$routines.h>

#include "errchk.h"


/******************************************************************************/
int main (void) {

static int crc_table[16];
static unsigned int mask = 0120001L;    /* Note! Octal */
static int crc;

static $DESCRIPTOR (test_d, "calculate the CRC for this string");

    lib$crc_table (&mask, crc_table);

    crc = lib$crc (crc_table,
                   &crc,
                   &test_d);

}



