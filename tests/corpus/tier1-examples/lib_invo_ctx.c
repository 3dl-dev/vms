
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#ifdef __VAX
#  error AXP/IA64/x86_64 specific code
#endif /* __VAX */

#define __NEW_STARLET 1

/*
** Because we are trying to show all traceback info, and because the program
** is so simple, ensure the compiler doesn't optimize away the intermediate
** function calls by switching off optimization.
*/
#pragma optimize level=0

#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <libicb.h>
#include <tbkdef.h>
#include <tbk$routines.h>
#include <lib$routines.h>

#include "errchk.h"

#ifdef __ia64
/*
** The documentation still says that these two functions are available on
** Alpha and IA64, even though the 8.2 release notes documented the fact that
** this was incorrect.  It seems strange that the engineers would have two
** RTL routines with _exactly_the_same_ parameters...
*/
#  define lib$get_curr_invo_context lib$i64_get_curr_invo_context
#  define lib$get_prev_invo_context lib$i64_get_prev_invo_context
#endif /* __ia64 */
#ifdef __x86_64
/*
** Ditto x86
*/
#  define lib$get_curr_invo_context lib$x86_get_curr_invo_context
#  define lib$get_prev_invo_context lib$x86_get_prev_invo_context
#endif /* __x86_64 */


/******************************************************************************/
static void trace_calls (void) {
/*
** This routine traverses the call frames to produce a stack dump similar to
** what happens when you signal an unhandled exception.  This can be called
** from anywhere (not kernel mode!) to obtain this information and allow the
** calling program to proceed.  This is Alpha/IA64 specific code.
*/

static INVO_CONTEXT_BLK icb;

static unsigned __int64 the_pc;
static unsigned __int64 flags = 0;

static unsigned int line;
static int finished;
static int r0_status;

static char image[512];
static char module[512];
static char routine[512];
static char spaces[] = "                 "; /* 17 spaces */

static struct dsc$descriptor_s image_d = { sizeof (image) - 1,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            image };
static struct dsc$descriptor_s module_d = { sizeof (module) - 1,
                                             DSC$K_DTYPE_T,
                                             DSC$K_CLASS_S,
                                             module };
static struct dsc$descriptor_s routine_d = { sizeof (routine) - 1,
                                              DSC$K_DTYPE_T,
                                              DSC$K_CLASS_S,
                                              routine };

static TBK_API_PARAM tbkitms;

    (void)memset (&tbkitms, 0, sizeof (TBK_API_PARAM));
    tbkitms.tbk$w_length = TBK$K_LENGTH;
    tbkitms.tbk$b_version = TBK$K_VERSION;
    tbkitms.tbk$pq_image_desc = (struct _descriptor *)&image_d;
    tbkitms.tbk$pq_module_desc = (struct _descriptor *)&module_d;
    tbkitms.tbk$pq_routine_desc = (struct _descriptor *)&routine_d;
    tbkitms.tbk$pq_listing_lineno = &line;
    tbkitms.tbk$pq_symbolize_flags = &flags;

#ifdef __ia64
    /*
    ** Itanium requires that we initialize our icb.
    */
    r0_status = lib$i64_init_invo_context (&icb, LIBICB$K_INVO_CONTEXT_VERSION);
    errchk_sig (r0_status);
#endif /* __ia64 */
#ifdef __x86_64
    /*
    ** Ditto x86_64
    */
    r0_status = lib$x86_init_invo_context (&icb, LIBICB$K_INVO_CONTEXT_VERSION);
    errchk_sig (r0_status);
#endif /* __x86_64 */


    lib$get_curr_invo_context (&icb);

    (void)printf ("Image             "
                  "Module            "
                  "Routine               "
                  "Line     abs PC\n");
        
    finished = FALSE;
    while (!finished) {
#ifdef __alpha
        /*
        ** While Alpha requires the PC and the frame pointer.
        */
        tbkitms.tbk$q_faulting_pc = icb.libicb$q_program_counter;
        tbkitms.tbk$q_faulting_fp = icb.libicb$q_ireg[29];
#endif /* __alpha */
#ifdef __ia64
        /*
        ** Itanium only needs the PC.
        */
        tbkitms.tbk$q_faulting_pc = (unsigned __int64)icb.libicb$ih_pc;
#endif /* __ia64 */
#ifdef __x86_64
	/*
	** Ditto x86, except x86 calls it the "instruction pointer"
	*/
	tbkitms.tbk$q_faulting_pc = (unsigned __int64)icb.libicb$ih_ip;
#endif /* __x86_64 */

#ifdef __alpha
        r0_status = tbk$alpha_symbolize ((unsigned int *)&tbkitms);
#endif /* __alpha */
#ifdef __ia64
        /*
        ** As I said, weird that the engineers would have two routines with
        ** the same parameters.
        */
        r0_status = tbk$i64_symbolize (&tbkitms);
#endif /* __ia64 */
        errchk_stop (r0_status);

        /*
        ** Let's print out the first 17 characters of the names, as that's all
        ** there is space for on a 80 char width terminal.
        */
        if (image_d.dsc$w_length > strlen (spaces)) {
            image_d.dsc$w_length = strlen (spaces);
        }
        if (module_d.dsc$w_length > strlen (spaces)) {
            module_d.dsc$w_length = strlen (spaces);
        }
        if (routine_d.dsc$w_length > strlen (spaces)) {
            routine_d.dsc$w_length = strlen (spaces);
        }

#ifdef __alpha
        the_pc = icb.libicb$q_program_counter;
#endif /* __alpha */
#ifdef __ia64
        the_pc = (unsigned __int64)icb.libicb$ih_pc;
#endif /* __ia64 */
#ifdef __x86_64
	the_pc = (unsigned __int64)icb.libicb$ih_ip;
#endif /* __x86_64 */

        (void)printf ("%-.*s%s %-.*s%s %-.*s%s %8ld %16.16LX\n", 
                      image_d.dsc$w_length,
                      image_d.dsc$a_pointer,
                      &spaces[image_d.dsc$w_length],
                      module_d.dsc$w_length,
                      module_d.dsc$a_pointer,
                      &spaces[module_d.dsc$w_length],
                      routine_d.dsc$w_length,
                      routine_d.dsc$a_pointer,
                      &spaces[routine_d.dsc$w_length],
                      line, 
                      the_pc); 

        /*
        ** Get the next call frame.
        */
        r0_status = lib$get_prev_invo_context (&icb);
        if (!$VMS_STATUS_SUCCESS (r0_status)) {
            finished = TRUE;
        } else {
            if (icb.libicb$v_bottom_of_stack) {
                finished = TRUE;
            }
        }
    }

#ifdef __ia64
    r0_status = lib$i64_prev_invo_end (&icb);
    errchk_sig (r0_status);
#endif /* __ia64 */
}


/******************************************************************************/
static void function2 (void) {

    trace_calls ();
}


/******************************************************************************/
static void function1 (void) {

    function2 ();
}


/******************************************************************************/
int main (void) {

    function1();

}
