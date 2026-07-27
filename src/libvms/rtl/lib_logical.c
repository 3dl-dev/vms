/*
 * lib_logical.c - LIB$ Simplified Logical Name Routines
 *
 * Implements LIB$SET_LOGICAL and LIB$DELETE_LOGICAL, the RTL
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
