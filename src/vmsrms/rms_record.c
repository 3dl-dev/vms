/*
 * rms_record.c - RMS Record Operation Dispatch
 *
 * Implements the sys$get, sys$put, sys$update, sys$delete, and
 * sys$find system services. Each operation validates the RAB/FAB
 * linkage, then dispatches to the appropriate organization-specific
 * handler (sequential, relative, or indexed).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * Each of the five validates the caller's RAB/FAB and dispatches to an
 * organization handler (rms_seq.c / rms_rel.c / rms_idx.c). Since vms-bc7 those
 * handlers reach file data through the Files-11 ODS-2 ACP: the block-I/O
 * substrate (rms_io.c) turns each positioned record transfer into IO$_READVBLK /
 * IO$_WRITEVBLK on the file's channel window over /dev/vms -- NOT a per-process
 * POSIX fd. The RECORD framing (RFM/RAT decode, key compares) is still done in
 * this process; only the block I/O beneath it is the executive's. RAB$M_ record
 * locking still has no executive lock manager behind it here (vms-407 owns that
 * missing arbitration, across rms_core.c, this file and rms_search.c).
 *
 * OVMX-PARTIAL: sys$get (vms-bc7) -- exec: IO$_READVBLK reads the record's
 *     virtual block(s) through the ACP window (rms_io_read).
 * OVMX-LOCAL: sys$get -- the RFM record framing / RAB cursor bookkeeping runs
 *     in this process; no executive record lock is taken.
 * OVMX-PARTIAL: sys$put (vms-bc7) -- exec: IO$_WRITEVBLK writes the record's
 *     virtual block(s) through the ACP window (rms_io_write), extending on EOF.
 * OVMX-LOCAL: sys$put -- the record framing / sequential-append positioning is
 *     this process's; no executive record lock is taken.
 * OVMX-PARTIAL: sys$update (vms-bc7) -- exec: IO$_WRITEVBLK rewrites the record
 *     in place through the ACP window.
 * OVMX-LOCAL: sys$update -- the in-process record framing decides what bytes to
 *     rewrite; no executive record lock is taken.
 * OVMX-PARTIAL: sys$delete (vms-bc7) -- exec: IO$_WRITEVBLK marks the record's
 *     cell through the ACP window.
 * OVMX-LOCAL: sys$delete -- the cell-status bookkeeping is this process's; no
 *     executive record lock is taken.
 * OVMX-PARTIAL: sys$find (vms-bc7) -- exec: IO$_READVBLK reads the cell status
 *     through the ACP window to position without transferring a record.
 * OVMX-LOCAL: sys$find -- the RAB positioning arithmetic is this process's; no
 *     executive record lock is taken.
 */

#include <stdio.h>
#include "rms_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "rms/rms.h"

/* Forward declarations: sequential file operations */
extern uint32_t rms_seq_get(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_seq_put(struct FAB *fab, struct RAB *rab);

/* Forward declarations: relative file operations */
extern uint32_t rms_rel_get(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_rel_put(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_rel_update(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_rel_delete(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_rel_find(struct FAB *fab, struct RAB *rab);

/* Forward declarations: indexed file operations */
extern uint32_t rms_idx_get(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_idx_put(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_idx_update(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_idx_delete(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_idx_find(struct FAB *fab, struct RAB *rab);
extern uint32_t rms_idx_flush(struct FAB *fab);
extern uint32_t rms_idx_cleanup(struct FAB *fab);

/*
 * Helper: validate RAB and extract associated FAB.
 * Returns NULL on error (sets rab$l_sts).
 */
static struct FAB *validate_rab(struct RAB *rab)
{
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return NULL;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        rab->rab$l_sts = RMS$_FAB;
        return NULL;
    }

    if (!fab->_rms_file) {
        rab->rab$l_sts = RMS$_ACC;
        return NULL;
    }

    return fab;
}

/*
 * sys$get - Read the next (or specified) record.
 *
 * Dispatches to the appropriate get handler based on the
 * file organization (sequential, relative, or indexed).
 * The record is placed in rab$l_ubf and rab$w_rsz is set.
 */
static uint32_t rms_impl_get(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = validate_rab(rab);
    if (!fab) return rab->rab$l_sts;

    uint32_t status;
    switch (fab->fab$b_org) {
        case FAB$C_SEQ:
            status = rms_seq_get(fab, rab);
            break;
        case FAB$C_REL:
            status = rms_rel_get(fab, rab);
            break;
        case FAB$C_IDX:
            status = rms_idx_get(fab, rab);
            break;
        default:
            status = RMS$_ORG;
            break;
    }

    rab->rab$l_sts = status;
    return status;
}

/*
 * sys$put - Write a record.
 *
 * For sequential files, appends to end. For relative files,
 * writes to the cell specified by rab$l_bkt. For indexed files,
 * inserts by key.
 */
static uint32_t rms_impl_put(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = validate_rab(rab);
    if (!fab) return rab->rab$l_sts;

    /* Verify write access */
    if (!(fab->fab$b_fac & FAB$M_PUT)) {
        rab->rab$l_sts = RMS$_IOP;
        return RMS$_IOP;
    }

    uint32_t status;
    switch (fab->fab$b_org) {
        case FAB$C_SEQ:
            status = rms_seq_put(fab, rab);
            break;
        case FAB$C_REL:
            status = rms_rel_put(fab, rab);
            break;
        case FAB$C_IDX:
            status = rms_idx_put(fab, rab);
            break;
        default:
            status = RMS$_ORG;
            break;
    }

    rab->rab$l_sts = status;
    return status;
}

/*
 * sys$update - Update the current record.
 *
 * Rewrites the record most recently accessed by $GET or $FIND.
 * Not supported for sequential files (returns RMS$_IOP).
 */
static uint32_t rms_impl_update(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = validate_rab(rab);
    if (!fab) return rab->rab$l_sts;

    /* Verify update access */
    if (!(fab->fab$b_fac & FAB$M_UPD)) {
        rab->rab$l_sts = RMS$_IOP;
        return RMS$_IOP;
    }

    uint32_t status;
    switch (fab->fab$b_org) {
        case FAB$C_SEQ:
            /* Sequential files do not support in-place update */
            status = RMS$_IOP;
            break;
        case FAB$C_REL:
            status = rms_rel_update(fab, rab);
            break;
        case FAB$C_IDX:
            status = rms_idx_update(fab, rab);
            break;
        default:
            status = RMS$_ORG;
            break;
    }

    rab->rab$l_sts = status;
    return status;
}

/*
 * sys$delete - Delete the current record.
 *
 * Marks the current record as deleted. Not supported for
 * sequential files (returns RMS$_IOP).
 */
static uint32_t rms_impl_delete(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = validate_rab(rab);
    if (!fab) return rab->rab$l_sts;

    /* Verify delete access */
    if (!(fab->fab$b_fac & FAB$M_DEL)) {
        rab->rab$l_sts = RMS$_IOP;
        return RMS$_IOP;
    }

    uint32_t status;
    switch (fab->fab$b_org) {
        case FAB$C_SEQ:
            /* Sequential files do not support record deletion */
            status = RMS$_IOP;
            break;
        case FAB$C_REL:
            status = rms_rel_delete(fab, rab);
            break;
        case FAB$C_IDX:
            status = rms_idx_delete(fab, rab);
            break;
        default:
            status = RMS$_ORG;
            break;
    }

    rab->rab$l_sts = status;
    return status;
}

/*
 * sys$find - Position to a record without reading its data.
 *
 * Establishes the "current record" context for a subsequent
 * $GET, $UPDATE, or $DELETE. For sequential files, this is
 * equivalent to $GET (the data is read but discarded after
 * positioning).
 */
static uint32_t rms_impl_find(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = validate_rab(rab);
    if (!fab) return rab->rab$l_sts;

    uint32_t status;
    switch (fab->fab$b_org) {
        case FAB$C_SEQ:
            /*
             * For sequential files, $FIND is the same as $GET
             * (we must read the record to advance past it).
             * We need a temporary buffer if the user hasn't provided one.
             */
            if (rab->rab$l_ubf && rab->rab$w_usz > 0) {
                status = rms_seq_get(fab, rab);
            } else {
                /* Use a temporary buffer */
                char temp[4096];
                char *save_ubf = rab->rab$l_ubf;
                uint16_t save_usz = rab->rab$w_usz;
                rab->rab$l_ubf = temp;
                rab->rab$w_usz = sizeof(temp);
                status = rms_seq_get(fab, rab);
                rab->rab$l_ubf = save_ubf;
                rab->rab$w_usz = save_usz;
            }
            break;
        case FAB$C_REL:
            status = rms_rel_find(fab, rab);
            break;
        case FAB$C_IDX:
            status = rms_idx_find(fab, rab);
            break;
        default:
            status = RMS$_ORG;
            break;
    }

    rab->rab$l_sts = status;
    return status;
}


/* ============================================================
 * Public RMS entry points: VMS three-argument form
 *   SYS$xxx cb ,[err] ,[suc]   (VSI OpenVMS RMS Reference, Part III)
 * Thin wrappers over the synchronous rms_impl_* bodies above that
 * dispatch the optional AST-level completion routine (rms_complete).
 * ============================================================ */
uint32_t sys$get(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_get(rab), rab, err, suc);
}

uint32_t sys$put(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_put(rab), rab, err, suc);
}

uint32_t sys$update(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_update(rab), rab, err, suc);
}

uint32_t sys$delete(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_delete(rab), rab, err, suc);
}

uint32_t sys$find(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_find(rab), rab, err, suc);
}
