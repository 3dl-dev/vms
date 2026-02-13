/*
 * descrip.c - VMS String Descriptor Operations
 *
 * Implements runtime descriptor management and utility functions
 * for working with the VMS descriptor system. VMS uses descriptors
 * instead of C-style null-terminated strings; this module provides
 * the bridge between both worlds.
 *
 * Two descriptor classes are primarily handled:
 *   Class S (static)  - fixed-length, caller-supplied buffer
 *   Class D (dynamic) - heap-allocated, library-managed storage
 *
 * Note: Several basic descriptor functions (vms_init_descriptor,
 * vms_desc_to_cstr, vms_cstr_to_desc, vms_desc_alloc, vms_desc_free,
 * dsc$init, dsc$strncpy) are defined as static inline in descrip.h.
 * This file provides additional functions and legacy aliases that
 * are not suitable for header-only implementation.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "descrip.h"
#include "ssdef.h"

/*
 * vms_init_descriptor_d - Initialize an empty dynamic descriptor.
 *
 * Sets up a Class D descriptor with no allocated storage.
 * Use vms_desc_alloc() to allocate storage later.
 */
void vms_init_descriptor_d(struct dsc$descriptor_d *desc) {
    if (!desc) return;
    desc->dsc$w_length = 0;
    desc->dsc$b_dtype = DSC$K_DTYPE_T;
    desc->dsc$b_class = DSC$K_CLASS_D;
    desc->dsc$a_pointer = NULL;
}

/*
 * vms_desc_copy - Copy descriptor contents from src to dest.
 *
 * If dest is Class D (dynamic), allocates/reallocates storage as needed.
 * If dest is Class S (static), copies up to dest's length and pads with
 * spaces (standard VMS behaviour for fixed-length descriptors).
 */
uint32_t vms_desc_copy(struct dsc$descriptor *dest,
                        const struct dsc$descriptor *src) {
    if (!dest || !src) return SS$_BADPARAM;

    if (dest->dsc$b_class == DSC$K_CLASS_D) {
        /* Dynamic destination - reallocate to match source */
        struct dsc$descriptor_d *ddst = (struct dsc$descriptor_d *)dest;
        int status = vms_desc_alloc(ddst, src->dsc$w_length);
        if (status != 0) return SS$_INSFMEM;
        if (src->dsc$w_length > 0 && src->dsc$a_pointer) {
            memcpy(ddst->dsc$a_pointer, src->dsc$a_pointer, src->dsc$w_length);
        }
    } else {
        /* Static destination - truncate or pad */
        uint16_t copylen = src->dsc$w_length;
        if (copylen > dest->dsc$w_length) {
            copylen = dest->dsc$w_length;
        }
        if (copylen > 0 && src->dsc$a_pointer && dest->dsc$a_pointer) {
            memcpy(dest->dsc$a_pointer, src->dsc$a_pointer, copylen);
        }
        /* Pad with spaces if destination is longer (VMS convention) */
        if (copylen < dest->dsc$w_length && dest->dsc$a_pointer) {
            memset(dest->dsc$a_pointer + copylen, ' ',
                   dest->dsc$w_length - copylen);
        }
    }

    return SS$_NORMAL;
}

/*
 * vms_desc_length - Return the length of a descriptor's data.
 */
uint16_t vms_desc_length(const struct dsc$descriptor *desc) {
    if (!desc) return 0;
    return desc->dsc$w_length;
}

/* --- Legacy API aliases used internally by other modules --- */

uint32_t dsc$alloc_d(struct dsc$descriptor_d *desc, uint16_t length) {
    if (vms_desc_alloc(desc, length) != 0)
        return SS$_INSFMEM;
    return SS$_NORMAL;
}

uint32_t dsc$free_d(struct dsc$descriptor_d *desc) {
    vms_desc_free(desc);
    return SS$_NORMAL;
}

uint32_t dsc$copy(struct dsc$descriptor *dst,
                   const struct dsc$descriptor *src) {
    return vms_desc_copy(dst, src);
}

uint16_t dsc$length(const struct dsc$descriptor *desc) {
    return vms_desc_length(desc);
}

const char *dsc$pointer(const struct dsc$descriptor *desc) {
    if (!desc) return NULL;
    return desc->dsc$a_pointer;
}
