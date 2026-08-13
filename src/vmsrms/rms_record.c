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
 * organization handler (rms_seq.c / rms_rel.c / rms_idx.c) that reads and
 * writes through fab->_linux_fd, this process's own descriptor. RAB$M_ options
 * that request record locking have no executive lock manager behind them here,
 * so two processes reading and writing the same file are not serialised by
 * anything RMS does.
 *
 * These five cited vms-5b4 until vms-fab; it is closed and owned none of them.
 * vms-407 owns the missing arbitration, across rms_core.c, this file and
 * rms_search.c.
 *
 * OVMX-USERSPACE: sys$get (vms-407) -- read(2) through the caller's own fd.
 * OVMX-USERSPACE: sys$put (vms-407) -- write(2) through the caller's own fd.
 * OVMX-USERSPACE: sys$update (vms-407) -- rewrite in place through that fd.
 * OVMX-USERSPACE: sys$delete (vms-407) -- marks the record through that fd.
 * OVMX-USERSPACE: sys$find (vms-407) -- positions that fd without transferring
 *     a record.
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
extern void rms_idx_flush(struct FAB *fab);
extern void rms_idx_cleanup(struct FAB *fab);

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

    if (fab->_linux_fd < 0) {
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
