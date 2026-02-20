
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <string.h>
#include <descrip.h>
#include <ots$routines.h>

#define KEN_OLSEN_QUOTE "Software comes from heaven when you have good hardware"
#define QUOTE_LEN 54


/******************************************************************************/
int main (void) {

static int bytes;

static $DESCRIPTOR (source_d, KEN_OLSEN_QUOTE);

static char exact[QUOTE_LEN] = { 0 };
static char more[QUOTE_LEN+9] = { 0 };
static char less[QUOTE_LEN-9] = { 0 };

static struct dsc$descriptor_s static_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            NULL };

#define MAX_VS 255
static struct {
    unsigned short length;
    char string[MAX_VS];
} vs_data;
static struct dsc$descriptor_vs vs_d = { MAX_VS,
                                         DSC$K_DTYPE_VT,
                                         DSC$K_CLASS_VS,
                                        (char *)&vs_data };


static struct dsc$descriptor_d dyn_d = { 0,
                                         DSC$K_DTYPE_D,
                                         DSC$K_CLASS_D,
                                         NULL };

    /*
    ** OTS$SCOPY_DXDX does different things with different classes of
    ** descriptors.
    */                    

    static_d.dsc$w_length = sizeof (exact);
    static_d.dsc$a_pointer = exact;
    bytes = ots$scopy_dxdx (&source_d, &static_d);
    (void)printf ("Bytes = %d\n", bytes);
    (void)printf ("\"%-.*s\"\n",
                  static_d.dsc$w_length,
                  static_d.dsc$a_pointer);

    static_d.dsc$w_length = sizeof (more);
    static_d.dsc$a_pointer = more;
    bytes = ots$scopy_dxdx (&source_d, &static_d);
    (void)printf ("Bytes = %d\n", bytes);
    (void)printf ("\"%-.*s\"\n",
                  static_d.dsc$w_length,
                  static_d.dsc$a_pointer);

    static_d.dsc$w_length = sizeof (less);
    static_d.dsc$a_pointer = less;
    bytes = ots$scopy_dxdx (&source_d, &static_d);
    (void)printf ("Bytes = %d\n", bytes);
    (void)printf ("\"%-.*s\"\n",
                  static_d.dsc$w_length,
                  static_d.dsc$a_pointer);

    bytes = ots$scopy_dxdx (&source_d, &vs_d);
    (void)printf ("Bytes = %d\n", bytes);
    (void)printf ("\"%-.*s\"\n",
                  vs_data.length,
                  vs_data.string);

    ots$sget1_dd (255,
                  (unsigned __int64 *)&dyn_d);

    bytes = ots$scopy_dxdx (&source_d, &dyn_d);
    (void)printf ("Bytes = %d\n", bytes);
    (void)printf ("\"%-.*s\"\n",
                  vs_data.length,
                  vs_data.string);

    ots$sfree1_dd ((unsigned __int64 *)&dyn_d);
}
