/*
 * lib_dyndesc.c - LIB$ Dynamic String Descriptor Management
 *
 * Implements the LIB$ RTL routines that allocate and free dynamic
 * string descriptors (DSC$K_CLASS_D). These are thin wrappers around
 * the descrip.h helper functions (vms_desc_alloc / vms_desc_free)
 * that already implement the correct CLASS_D allocation semantics.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual -
 *            LIB$SGET1_DD, LIB$SFREE1_DD, LIB$SFREEN_DD
 *
 * Call sites in the wild (Eight-Cubed conformance corpus) pass the
 * dynamic descriptor to LIB$SFREE1_DD via an explicit
 * "(unsigned __int64 *)" cast, so the declared parameter type here
 * matches that convention (a raw 64-bit-wide pointer to the
 * descriptor's address, reinterpreted internally as
 * struct dsc$descriptor_d *). LIB$SGET1_DD and LIB$SFREEN_DD are
 * always called with the descriptor's real type, so those take
 * struct dsc$descriptor_d * directly.
 */

#include <stdint.h>
#include <stddef.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

/*
 * lib$sget1_dd - Allocate one dynamic string descriptor.
 *
 * @param len     Pointer to longword containing the requested length
 *                in bytes.
 * @param dyndsc  Pointer to the dynamic descriptor to initialize.
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if dyndsc is NULL,
 *          SS$_INSFMEM if the allocation fails.
 *
 * Any storage already referenced by dyndsc is NOT freed first (per
 * documented LIB$SGET1_DD behavior - the descriptor is assumed to be
 * freshly initialized or already empty). Callers that reuse a
 * descriptor should free it first with LIB$SFREE1_DD.
 */
uint32_t lib$sget1_dd(const uint32_t *len, struct dsc$descriptor_d *dyndsc) {
    if (!dyndsc)
        return SS$_BADPARAM;

    uint32_t want = len ? *len : 0;
    if (want > 0xFFFFu)
        return SS$_BADPARAM;  /* dsc$w_length is a 16-bit field */

    if (vms_desc_alloc(dyndsc, (uint16_t)want) != 0)
        return SS$_INSFMEM;

    return SS$_NORMAL;
}

/*
 * lib$sfree1_dd - Free one dynamic string descriptor.
 *
 * @param dyndsc  Pointer to the dynamic descriptor to free (declared
 *                as a raw 64-bit pointer to match corpus call sites'
 *                "(unsigned __int64 *)&desc" casts; reinterpreted as
 *                struct dsc$descriptor_d * internally).
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if dyndsc is NULL.
 *
 * Frees the buffer (if any) and resets the descriptor to an empty
 * dynamic descriptor. Safe to call on an already-empty descriptor.
 */
uint32_t lib$sfree1_dd(uint64_t *dyndsc) {
    if (!dyndsc)
        return SS$_BADPARAM;

    struct dsc$descriptor_d *d = (struct dsc$descriptor_d *)dyndsc;
    vms_desc_free(d);

    return SS$_NORMAL;
}

/*
 * lib$sfreen_dd - Free N dynamic string descriptors in one call.
 *
 * @param n            Pointer to longword count of descriptors.
 * @param dyndsc_array  Pointer to the first element of an array of
 *                       dynamic descriptors.
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if either argument is
 *          missing.
 */
uint32_t lib$sfreen_dd(const uint32_t *n, struct dsc$descriptor_d *dyndsc_array) {
    if (!n || !dyndsc_array)
        return SS$_BADPARAM;

    for (uint32_t i = 0; i < *n; i++)
        vms_desc_free(&dyndsc_array[i]);

    return SS$_NORMAL;
}
