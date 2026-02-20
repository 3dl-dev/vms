
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <string.h>
#include <libvmdef.h>
#include <libdef.h>
#include <lib$routines.h>

#include "errchk.h"

/*
** This program demonstrates calls to LIB$CREATE_VM_ZONE, LIB$FIND_VM_ZONE,
** LIB$GET_VM, LIB$SHOW_VM, LIB$SHOW_VM_ZONE, LIB$VERIFY_VM_ZONE, LIB$FREE_VM,
** LIB$DELETE_VM_ZONE, and LIB$STAT_VM by replacing the standard C library
** functions malloc (), calloc (), realloc (), and free ().
*/
#define malloc mymalloc
#define calloc mycalloc
#define realloc myrealloc
#define free myfree

#define ZONE_NAME "Danger Zone"

/*
** Globally scoped variable to count blocks allocated and freed.
*/
static int block_count = 0;


/******************************************************************************/
static void *mymalloc (size_t size) {
/*
** Replace malloc () with this routine that calls lib$get_vm () to we can
** do nice memory analysis.
*/

static void *address;

static int r0_status;
static unsigned int zone_id = 0;

static unsigned int flags =
                     LIB$M_VM_BOUNDARY_TAGS|LIB$M_VM_GET_FILL0;
static int algorithm = LIB$K_VM_QUICK_FIT; /* Quick fit */
static int algorithm_arg = 32;	      /* 32 lookaside lists */

static const $DESCRIPTOR (zone_name_d, ZONE_NAME);

    /*
    ** If the zone doesn't exist, create it.
    */
    if (zone_id == 0) {
        r0_status = lib$create_vm_zone (&zone_id,
                                        &algorithm,
                                        &algorithm_arg,
                                        &flags,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        &zone_name_d,
                                        0,
                                        0);
        errchk_sig (r0_status);
    }

    /*
    ** Get the memory.
    */
    r0_status = lib$get_vm ((int *)&size,
                            &address,
                            &zone_id);
    if (!$VMS_STATUS_SUCCESS (r0_status)) {
        (void)printf ("lib$get_vm () returned an error status: %08x\n",
                      r0_status);
        return NULL;
    } else {
#ifdef DEBUG
        (void)printf ("Allocated %u bytes at %08x\n",
                      size,
                      address);
#endif /* DEBUG */
        block_count++;
        return address;
    }
}


/******************************************************************************/
static void *mycalloc (size_t elt_count,
                       size_t elt_size) {
/*
** Replace calloc () with this funtion.
*/

    return (malloc (elt_count * elt_size));
}


/******************************************************************************/
static void *myrealloc (void *ptr,
                        size_t size) {
/*
** Replace realloc () with this function.  Implementation is left as an
** exercise for the student ;-)
*/

    (void)printf ("realloc is not implemented\n");
    assert (0);
    return NULL;
}


/******************************************************************************/
static int get_zone_name (struct dsc$descriptor_s *string_d,
                          struct dsc$descriptor_s *output_d) {
/*
** Get the name of a VM zone.
*/

static char *s;
static char *o;
static int i;
static short int length; 
static char seen;

    /*
    ** The default string is in the form:
    ** 'Zone ID = nnnnnn, Zone Name = "name"'
    */
    seen = FALSE;
    length = output_d->dsc$w_length;
    o = output_d->dsc$a_pointer;
    s = string_d->dsc$a_pointer;

    for (i = 0; i < string_d->dsc$w_length - 1; i++) {
        if (!seen) {
            if (*s == '"') {
                output_d->dsc$w_length = 0;
                seen = TRUE;
            }
        } else {
            if (--length < 0) {
                return LIB$_STRTRU;
            } else {
                *o++ = *s;
                output_d->dsc$w_length++;
            }
        }
        s++;
    } 
    return SS$_NORMAL;
}


/******************************************************************************/
static void myfree (void *ptr) {
/*
** Replace free () with this function so we can use lib$free_vm.
*/

static int r0_status;
static unsigned int zone_id;
static unsigned int context = 0;
static int zone_detail = 1;

static char found = FALSE;

static char test_name[255+1];
static struct dsc$descriptor_s test_name_d = { 0,
                                               DSC$K_DTYPE_T,
                                               DSC$K_CLASS_S,
                                               test_name };

static const $DESCRIPTOR (zone_name_d, ZONE_NAME);

    /*
    ** Find the VM zone that mymalloc () created.
    */
    while (!found) {

        /*
        ** lib$find_vm_zone returns info on all 32 bit zones.
        */
	r0_status = lib$find_vm_zone (&context,
                                      &zone_id);
        errchk_sig (r0_status);

        /*
        ** For each zone, get the zone name.
        */
        test_name_d.dsc$w_length = sizeof(test_name) - 1;
        r0_status = lib$show_vm_zone (&zone_id,
                                      &zone_detail,
                                      get_zone_name,
                                      &test_name_d);
	errchk_sig (r0_status);

        /*
        ** And find out of this zone name is ours.
        */
        if ((zone_name_d.dsc$w_length == test_name_d.dsc$w_length) &&
            (memcmp (zone_name_d.dsc$a_pointer,
                     test_name_d.dsc$a_pointer,
                     test_name_d.dsc$w_length) == 0)) {
            found = TRUE;

            /*
            ** Just for demo purposes, let's verify the zone.
            */
            r0_status = lib$verify_vm_zone (&zone_id);
            errchk_sig (r0_status);
            continue;
        }
    }

    /*
    ** Free the memory.
    */
    r0_status = lib$free_vm (0,
                             &ptr,
                             &zone_id);
    errchk_sig (r0_status);

#ifdef DEBUG
    (void)printf ("Freed bytes at %08x\n",
                  ptr);
#endif /* DEBUG */

    /*
    ** If block count goes negative, we called free () one too many times...
    */
    if (--block_count < 0) {
        (void)printf ("Too liberal use of free ()\n");
        assert (0);
    }

    /*
    ** Delete the zone if there are no remaining blocks.
    */
    if (block_count == 0) {
        r0_status = lib$delete_vm_zone (&zone_id);
        errchk_sig (r0_status);
    }   
}


/******************************************************************************/
int main (void) {

#define MAX_ELEMENTS 699

static int r0_status;
static unsigned int calls;
static int code = 1;
static void *p_array[MAX_ELEMENTS];
static int i;

    r0_status = lib$show_vm ();
    errchk_sig (r0_status);

    (void)printf ("Allocating memory\n");
    for (i = 0; i < MAX_ELEMENTS; i++) {
        if ((i & 1) == 0) {
            p_array[i] = malloc (50);
        } else {
            p_array[i] = calloc (4, 10);
        }
    }

    r0_status = lib$show_vm ();
    errchk_sig (r0_status);

    r0_status = lib$stat_vm (&code, &calls);
    errchk_sig (r0_status);

    (void)printf ("There have been %u calls to LIB$GET_VM ()\n", calls);

    (void)printf ("Freeing memory\n");
    for (i = MAX_ELEMENTS - 1; i >= 0; i--) {
        free (p_array[i]);
    }

    r0_status = lib$show_vm ();
    errchk_sig (r0_status);
}
