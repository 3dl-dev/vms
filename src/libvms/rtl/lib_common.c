/*
 * lib_common.c - LIB$ process common storage area
 *
 * Implements:
 *
 *   LIB$PUT_COMMON  - Copy a string into the per-process common area
 *   LIB$GET_COMMON  - Copy the per-process common area into a string
 *
 * The process common area is a 252-byte region of per-process storage,
 * plus the length of the data currently held in it.  LIB$PUT_COMMON
 * stores up to 252 bytes; LIB$GET_COMMON returns what was stored.  The
 * area is inherently per-process (never shared between processes), so a
 * process-lifetime buffer is the architecturally correct backing store.
 *
 * LIMITATION: on real VMS this storage is process-PERMANENT — it survives
 * across image activations within the same process (that is what the
 * eight-cubed demo's "run me again" behaviour relies on).  OVMX does not
 * yet carry the storage across an image activation boundary, so a fresh
 * process starts with an empty common area.  Within a single image the
 * store/retrieve semantics are complete and honest.  (Follow-up: back the
 * common area with executive-resident P1 process-permanent storage.)
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$PUT_COMMON,
 *            LIB$GET_COMMON (252-byte process common area).
 */

#include <stdint.h>
#include <string.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"

#define LIB_COMMON_SIZE 252

static uint8_t  common_area[LIB_COMMON_SIZE];
static uint16_t common_len = 0;

/*
 * lib$put_common - Store up to 252 bytes into the common area.
 */
uint32_t lib$put_common(const struct dsc$descriptor_s *string)
{
    if (!string)
        return SS$_BADPARAM;

    uint16_t n = string->dsc$w_length;
    uint32_t status = SS$_NORMAL;

    if (n > LIB_COMMON_SIZE) {
        n = LIB_COMMON_SIZE;
        status = LIB$_STRTRU;
    }
    if (string->dsc$a_pointer && n)
        memcpy(common_area, string->dsc$a_pointer, n);
    common_len = n;
    return status;
}

/*
 * lib$get_common - Retrieve the common area into the destination string.
 *
 * The destination descriptor's length is the buffer capacity on input;
 * resultant_length receives the number of bytes actually returned.
 */
uint32_t lib$get_common(struct dsc$descriptor_s *resultant_string,
                        uint16_t *resultant_length)
{
    if (!resultant_string)
        return SS$_BADPARAM;

    uint16_t capacity = resultant_string->dsc$w_length;
    uint16_t n = common_len;
    uint32_t status = SS$_NORMAL;

    if (n > capacity) {
        n = capacity;
        status = LIB$_STRTRU;
    }
    if (resultant_string->dsc$a_pointer && n)
        memcpy(resultant_string->dsc$a_pointer, common_area, n);
    if (resultant_length)
        *resultant_length = n;
    return status;
}
