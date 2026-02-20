
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <afrdef.h>
#include <chfdef.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"


/******************************************************************************/
static int handler (struct chf$signal_array *sig_array,
                    struct chf$mech_array *mech_array) {

static unsigned int test;
static struct chf64$signal_array *sig64_array;


    /*
    ** SS$_ALIGN is an error.  We need to turn in into an informational...
    */
    test = SS$_ALIGN;
    test &= ~$VMS_STATUS_SEVERITY (test);
    test |= 0x00000003;

    if (sig_array->chf$l_sig_name == test) {
        /*
        ** Is this error an alignment fault?  If so, demonstrate how to use
        ** the 64 bit version of the signal array.  Use the action routine
        ** argument of $putmsg to write the data to a file.
        */
        sig64_array = 
            (struct chf64$signal_array *)mech_array->chf$ph_mch_sig64_addr;
        (void)sys$putmsg (sig64_array, 0, 0, 0);                      
        return SS$_CONTINUE;
    } else {
        if (sig_array->chf$l_sig_name == SS$_UNWIND) {
            return SS$_UNWIND;
        } else {
            return SS$_RESIGNAL;
        }
    }
}


/******************************************************************************/
static void cause_fault (void) {

static char first_time = TRUE;
static int addr;
static int *x_p;
static int x[2];

    if (first_time) {
        /*
        ** Increment the address in x_p by 1 byte so it's off the longword
        ** boundary...
        */
        first_time = FALSE;
        addr = (int)&x[0];
        x_p = (int *)++addr;
    }

    /*
    ** Now referencing this address will cause an alignment fault.
    */
    *x_p = 1;
}


/******************************************************************************/
int main (void) {
#ifdef __VAX
#  error "64 bit integers required"
#endif /* __VAX */

#define BUFFER_ITEMS 10

static AFRDEF *data;

static int r0_status;
static int return_len;
static unsigned int offset;

static char af_buffer[1024];
static char get_buffer[BUFFER_ITEMS*AFR$K_USER_LENGTH];

    /*
    ** Establish a condition handler.
    */
    (void)lib$establish (&handler);

    /*
    ** Normally, the operating system silently handles alignment faults...
    */
    cause_fault ();

    /*
    ** But while developing software, you should consider enabling fault
    ** reporting, either as informational exceptions (which could be trapped
    ** by a condition handler and written out to a file)...
    */
    r0_status = sys$start_align_fault_report (AFR$C_EXCEPTION, 0, 0);
    errchk_sig (r0_status);

    cause_fault ();

    r0_status = sys$stop_align_fault_report ();
    errchk_sig (r0_status);

    /*
    ** ...or reporting into a buffer that can be periodically polled with
    ** sys$get_align_fault_data ()
    */
    r0_status = sys$start_align_fault_report (AFR$C_BUFFERED,
                                              af_buffer,
                                              sizeof (af_buffer));
    errchk_sig (r0_status);
    for (int i = 0; i < BUFFER_ITEMS; i++) {
        cause_fault ();
    }

    do {
        r0_status = sys$get_align_fault_data (get_buffer,
                                              sizeof (get_buffer),
                                              &return_len);
        errchk_sig (r0_status);

        offset = 0;
        while (offset < return_len) {
            data = (AFRDEF *)(&get_buffer[offset]);
            (void)printf ("AF with PC = %8.8X and VA = %8.8X\n",
                          data->afr$l_fault_pc_l,
                          data->afr$l_fault_va_l);
            offset += AFR$C_USER_LENGTH;
        }
    } while (return_len != 0);
}
