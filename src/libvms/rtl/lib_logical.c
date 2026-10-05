/*
 * lib_logical.c - LIB$ Simplified Logical Name Routines
 *
 * Implements LIB$SET_LOGICAL, LIB$DELETE_LOGICAL and LIB$GET_LOGICAL, the RTL
 * convenience wrappers around the SYS$CRELNM / SYS$DELLNM system
 * services. These let a caller define or delete a single-valued
 * logical name without building an item list by hand (LIB$SET_LOGICAL
 * builds a one-entry LNM$_STRING item list from the equivalence-name
 * descriptor when the caller doesn't supply its own item list).
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual -
 *            LIB$SET_LOGICAL, LIB$DELETE_LOGICAL
 */

#include <stdint.h>
#include <stddef.h>
#include "ssdef.h"
#include "descrip.h"
#include "lnmdef.h"
#include "lib$routines.h"
#include "starlet.h"

/*
 * lib$set_logical - Define a logical name (simplified interface).
 *
 * @param lognam   Descriptor of the logical name to define (required).
 * @param eqvnam   Optional descriptor of the equivalence string. Used
 *                 to build a single LNM$_STRING item list entry when
 *                 itmlst is not supplied.
 * @param tabnam   Optional descriptor of the table name. Defaults to
 *                 LNM$PROCESS_TABLE if not supplied.
 * @param attr     Optional pointer to attribute flags, passed through
 *                 to SYS$CRELNM.
 * @param itmlst   Optional item list, passed through to SYS$CRELNM
 *                 verbatim if supplied (takes precedence over eqvnam).
 *
 * @return  SS$_NORMAL or SS$_SUPERSEDE on success (see SYS$CRELNM),
 *          SS$_BADPARAM if lognam is missing.
 */
uint32_t lib$set_logical(
    const struct dsc$descriptor_s *lognam,
    const struct dsc$descriptor_s *eqvnam,
    const struct dsc$descriptor_s *tabnam,
    const uint32_t *attr,
    const struct item_list_3 *itmlst)
{
    if (!lognam)
        return SS$_BADPARAM;

    static const struct dsc$descriptor_s default_table = {
        18, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE"
    };
    const struct dsc$descriptor_s *tbl = tabnam ? tabnam : &default_table;

    if (itmlst)
        return sys$crelnm(attr, tbl, lognam, NULL, itmlst);

    /* Build a one-entry item list from the equivalence-name descriptor. */
    struct item_list_3 built[2];
    built[0].item_code = LNM$_STRING;
    built[0].bufaddr   = eqvnam ? eqvnam->dsc$a_pointer : NULL;
    built[0].buflen    = eqvnam ? eqvnam->dsc$w_length : 0;
    built[0].retlen    = NULL;
    built[1].buflen = 0;
    built[1].item_code = 0;
    built[1].bufaddr = NULL;
    built[1].retlen = NULL;

    return sys$crelnm(attr, tbl, lognam, NULL, built);
}

/*
 * lib$get_logical - Translate a logical name (simplified interface).
 *
 * The RTL wrapper over SYS$TRNLNM: it asks for LNM$_STRING (and, when the caller
 * wants them, the LNM$_INDEX selected translation and LNM$_MAX_INDEX) and copies
 * the equivalence string into the caller's descriptor.
 *
 * @param lognam   Logical name (required).
 * @param resstr   Receives the equivalence string (required; any string class).
 * @param reslen   Optional; receives the length of the equivalence string. It may
 *                 alias resstr's own length field (a common idiom): the string is
 *                 copied first, the length stored last.
 * @param tabnam   Optional table (or search list of tables); default LNM$FILE_DEV.
 * @param maxidx   Optional; receives the highest translation index.
 * @param index    Optional; the translation to return (0 = first); a search list
 *                 has several.
 * @param acmode   Optional access-mode mask passed through to SYS$TRNLNM.
 * @param flags    Optional attribute flags (LNM$M_CASE_BLIND ...) passed to
 *                 SYS$TRNLNM.
 *
 * @return  SS$_NORMAL; SS$_NOLOGNAM when there is no such name; the status of
 *          SYS$TRNLNM otherwise; SS$_BADPARAM for a missing required argument.
 */
uint32_t lib$get_logical(
    const struct dsc$descriptor_s *lognam,
    struct dsc$descriptor_s *resstr,
    uint16_t *reslen,
    const struct dsc$descriptor_s *tabnam,
    int32_t *maxidx,
    const uint32_t *index,
    const uint8_t *acmode,
    const uint32_t *flags)
{
    if (!lognam || !resstr)
        return SS$_BADPARAM;

    static const struct dsc$descriptor_s default_table = {
        13, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$FILE_DEV"
    };
    const struct dsc$descriptor_s *tbl = tabnam ? tabnam : &default_table;

    char buf[LNM$C_MAXVALLEN + 1];
    uint16_t len = 0;
    int32_t mx = 0;
    uint32_t idx = index ? *index : 0;
    struct item_list_3 il[4];
    int n = 0;

    if (index) {
        il[n].buflen = sizeof(idx); il[n].item_code = LNM$_INDEX;
        il[n].bufaddr = &idx;       il[n].retlen = NULL; n++;
    }
    il[n].buflen = LNM$C_MAXVALLEN; il[n].item_code = LNM$_STRING;
    il[n].bufaddr = buf;            il[n].retlen = &len; n++;
    if (maxidx) {
        il[n].buflen = sizeof(mx);  il[n].item_code = LNM$_MAX_INDEX;
        il[n].bufaddr = &mx;        il[n].retlen = NULL; n++;
    }
    il[n].buflen = 0; il[n].item_code = 0; il[n].bufaddr = NULL; il[n].retlen = NULL;

    uint32_t st = sys$trnlnm(flags, tbl, lognam, acmode, il);
    if (!(st & 1))
        return st;

    uint16_t copy = len;
    uint32_t cs = lib$scopy_r_dx(&copy, buf, resstr);
    if (maxidx)
        *maxidx = mx;
    if (reslen)
        *reslen = len;
    return (cs & 1) ? st : cs;
}

/*
 * lib$delete_logical - Delete a logical name (simplified interface).
 *
 * @param lognam  Descriptor of the logical name to delete (required).
 * @param tabnam  Optional descriptor of the table name. Defaults to
 *                LNM$PROCESS_TABLE if not supplied.
 *
 * @return  SS$_NORMAL on success, SS$_NOLOGNAM if not found,
 *          SS$_BADPARAM if lognam is missing.
 */
uint32_t lib$delete_logical(
    const struct dsc$descriptor_s *lognam,
    const struct dsc$descriptor_s *tabnam)
{
    if (!lognam)
        return SS$_BADPARAM;

    static const struct dsc$descriptor_s default_table = {
        18, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"LNM$PROCESS_TABLE"
    };
    const struct dsc$descriptor_s *tbl = tabnam ? tabnam : &default_table;

    return sys$dellnm(tbl, lognam, NULL);
}
