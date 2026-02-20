
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <ieeedef.h>
#include <math.h>
#include <lib$routines.h>
#include <starlet.h>

#include "errchk.h"

#ifndef __IEEE_FLOAT
#  error "Please compile with an /IEEE_MODE option"
#endif /* __IEEE_FLOAT */

#ifdef __VAX
#  error "Alpha/IA64 only"
#endif /* __VAX */


/******************************************************************************/
static int handler (int sigargs[], int mechargs[]) {

static int status;

    switch (sigargs[1]) {
	case SS$_HPARITH :
        case SS$_FLTDIV_F :
        case SS$_FLTDIV :
            (void)printf ("Divide by zero detected!\n");
            (void)sys$unwind (0,0);
            break;
        case SS$_UNWIND :
            status = sigargs[1];
            break;
        default:
            status = SS$_RESIGNAL;
            break;
    }
    return status;
}


/******************************************************************************/
int main (void) {

static IEEE clr_mask;
static IEEE set_mask;
static IEEE prev_fpc;
static IEEE junk_fpc;

static double pi = 3.1415926535897932384626433832795;
static double z;
static double a = 63.0;
static double b = 9.0;
static double c = 6.3;
static double d = 0.9;

static int i;
static int r0_status;
static int junk;
static int prev_prc;
static int prev_rnd;

#if __ia64 || __x86_64 

    /*
    ** Get the precision we have now.
    */
    r0_status = sys$ieee_set_precision_mode (IEEE$C_PM_NO_CHANGE,
                                             &prev_prc);
    errchk_sig (r0_status);

    /*
    ** Switch to double precision.
    */
    r0_status = sys$ieee_set_precision_mode (IEEE$C_PM_DOUBLE,
                                             &junk);
    errchk_sig (r0_status);

    z = tan (pi / 2.0);
    (void)printf ("%f\n", z);

    /*
    ** Switch to single precision.
    */  
    r0_status = sys$ieee_set_precision_mode (IEEE$C_PM_SINGLE,
                                             &junk);
    errchk_sig (r0_status);

    z = tan (pi / 2.0);
    (void)printf ("%f\n", z);

    /*
    ** Get rounding now.
    */
    r0_status = sys$ieee_set_rounding_mode (IEEE$C_RM_NO_CHANGE,
                                            &prev_rnd);
    errchk_sig (r0_status);

    /*
    ** Switch to truncate.
    */
    r0_status = sys$ieee_set_rounding_mode (IEEE$C_RM_TRUNCATE,
                                            &junk);
    errchk_sig (r0_status);

    i = (int)(a / b);
    (void)printf ("%d\n", i);
    i = (int)(c / d);
    (void)printf ("%d\n", i);

    /*
    ** Switch to nearest.
    */
    r0_status = sys$ieee_set_rounding_mode (IEEE$C_RM_NEAREST,
                                            &junk);
    errchk_sig (r0_status);

    i = (int)(a / b);
    (void)printf ("%d\n", i);
    i = (int)(c / d);
    (void)printf ("%d\n", i);


    /*
    ** Revert to our previous precision.
    */
    r0_status = sys$ieee_set_precision_mode (prev_prc,
                                             &junk);
    errchk_sig (r0_status);

    /*
    ** Restore original rounding mode
    */
    r0_status = sys$ieee_set_rounding_mode (prev_rnd,
                                            &junk);
    errchk_sig (r0_status);
#endif /* __ia64 || __x86_64 */

    /*
    ** Get original floating point control.
    */
    r0_status = sys$ieee_set_fp_control (0,
                                         0,
                                         &prev_fpc);
    errchk_sig (r0_status);

    clr_mask.ieee$q_flags = IEEE$M_TRAP_ENABLE_DZE;
    set_mask.ieee$q_flags = IEEE$M_TRAP_ENABLE_DZE;

    /*
    ** Establish a condition handler.
    */
    (void)lib$establish (handler);

    /*
    ** Switch off divide by zero traps.
    */
    r0_status = sys$ieee_set_fp_control (&clr_mask,
                                         0,
                                         &junk_fpc);
    errchk_sig (r0_status);

    /*
    ** Our condition handler will not be called when we accidently on
    ** purpose divide by zero.
    */

    (void)printf ("%f\n", 1.0 / (b - b));    

    /*
    ** Switch on divide by zero traps.
    */
    r0_status = sys$ieee_set_fp_control (0,
                                         &set_mask,
                                         &junk_fpc);
    errchk_sig (r0_status);

    /*
    ** Now our condition handler will be called as the system will trap the
    ** divide by zero and signal an error.
    */
    (void)printf ("%f\n", 1.0 / (b - b));    


    /*
    ** Restore original floating point control.
    */
    r0_status = sys$ieee_set_fp_control (0,
                                         &prev_fpc,
                                         0);
    errchk_sig (r0_status);
}
