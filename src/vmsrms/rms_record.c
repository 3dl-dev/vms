/*
 * rms_record.c - RMS Record Operation Dispatch
 *
 * Implements the sys$get, sys$put, sys$update, sys$delete, and
 * sys$find system services, and the block-I/O services sys$read and
 * sys$write (vms-254). Each operation validates the RAB/FAB
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
 * this process; only the block I/O beneath it is the executive's. Since vms-0dd
 * (docs/design-rms-record-lock.md) RAB$M_ record-locking intent (rab$l_rop) ALSO
 * reaches the executive lock manager: a real $ENQ/$DEQ (vms_kif_enq/vms_kif_deq),
 * a CHILD of the FAB's file-access lock (vms-50e), arbitrated by the same DLM
 * that already arbitrates file-level share intent -- when the FAB holds one
 * (h->access_lkid != 0; ACP absent or the vms-5f0 POSIX defer takes none).
 *
 * OVMX-PARTIAL: sys$get (vms-bc7) -- exec: IO$_READVBLK reads the
 *     record's virtual block(s) through the ACP window (rms_io_read); a default
 *     (locking) $get also takes a real per-record $ENQ (EX, child of the file
 *     lock) -- RMS$_RLK on a real $ENQ conflict, never a local flag.
 * OVMX-LOCAL: sys$get -- the RFM record framing / RAB cursor bookkeeping and the
 *     rab$l_rop -> lock-mode decision run in this process.
 * OVMX-PARTIAL: sys$put (vms-bc7) -- exec: IO$_WRITEVBLK writes the
 *     record's virtual block(s) through the ACP window (rms_io_write), extending
 *     on EOF; also takes a real EX $ENQ on the freshly written record.
 * OVMX-LOCAL: sys$put -- the record framing / sequential-append positioning is
 *     this process's.
 * OVMX-PARTIAL: sys$update (vms-bc7) -- exec: IO$_WRITEVBLK rewrites the
 *     record in place through the ACP window; requires and operates against the
 *     real record $ENQ the stream's prior $get granted (RMS$_CUR if it holds
 *     none).
 * OVMX-LOCAL: sys$update -- the in-process record framing decides what bytes to
 *     rewrite.
 * OVMX-PARTIAL: sys$delete (vms-bc7) -- exec: IO$_WRITEVBLK marks the
 *     record's cell through the ACP window; same real-$ENQ requirement as
 *     sys$update.
 * OVMX-LOCAL: sys$delete -- the cell-status bookkeeping is this process's.
 * OVMX-PARTIAL: sys$find (vms-bc7) -- exec: IO$_READVBLK reads the cell
 *     status through the ACP window to position without transferring a record;
 *     takes the same real per-record $ENQ a $get does.
 * OVMX-LOCAL: sys$find -- the RAB positioning arithmetic is this process's.
 * OVMX-PARTIAL: sys$read (vms-254) -- exec: IO$_READVBLK transfers the
 *     requested virtual blocks through the ACP window (rms_io_read), clamped at
 *     the end of file the ODS-2 header records.
 * OVMX-LOCAL: sys$read -- the FAC check, the next-block (RAB$L_BKT=0) context
 *     and the RSZ/RFA bookkeeping are this process's.
 * OVMX-PARTIAL: sys$write (vms-254) -- exec: IO$_WRITEVBLK writes the virtual
 *     blocks through the ACP window (rms_io_write), extending the file and
 *     moving its end of file to the end of the transfer.
 * OVMX-LOCAL: sys$write -- the FAC check and the next-block context are this
 *     process's.
 */

#include <stdio.h>
#include "rms_internal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "rms/rms.h"

#include "rms_io.h"     /* rms_file_t; rms_io_* for block I/O (vms-254)       */
#if defined(OVMX_HAVE_ACP)
#include "vms_kif.h"    /* vms_kif_enq/deq -- the record $ENQ (vms-0dd)      */
#endif

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

#if defined(OVMX_HAVE_ACP)
/*
 * ============================================================
 * Record-level locking behind the DLM (vms-0dd, half b,
 * docs/design-rms-record-lock.md). Completes RMS-behind-DLM after vms-50e's
 * FILE-level share arbitration: a RAB's "current record" now takes a real
 * per-record $ENQ, a CHILD of the FAB's file-access lock, so two streams
 * contending for one record are arbitrated by the real executive lock
 * manager -- never a userspace record table (INV-6). Centralized HERE at
 * the dispatch level (not duplicated in rms_seq/rel/idx.c), one record lock
 * per stream (RAB._rec_lock_lkid).
 * ============================================================
 */

/*
 * rms_record_lock_resnam - the per-(file,record) DLM resource name: the
 * file's FID plus a locator derived from the record's byte offset within
 * the data fork (rab->_last_rec_offset, set by every org get/find handler
 * when it locates a record -- rms_seq_get/rms_rel_get/rms_rel_find/
 * rms_idx_get/rms_idx_find). Two streams on the SAME record (same file,
 * same offset) contend on the SAME resource. Distinct from the file lock's
 * "RMS$"+hex(fid) resource (vms-50e).
 *
 * Clean-room note (Rule 8): no org handler in this tree populates rab$w_rfa
 * (a real VMS RFA -- area/page/offset within a bucket); RFA-addressed
 * $GET(RAB$C_RFA) is not implemented anywhere here. Splitting the byte
 * offset every org handler already computes into a page/offset-shaped pair
 * is OVMX's own construction over the public "one resource per record"
 * contract (Guide to OpenVMS File Applications) -- not a disclosed
 * VMS-internal RFA encoding, exactly like rms_fileshare_mode's own note.
 */
static void rms_record_lock_resnam(const rms_file_t *h, uint64_t rec_offset,
                                    char out[32])
{
    unsigned fidhi = (unsigned)h->fid_num | ((unsigned)h->fid_nmx << 16);
    unsigned page = (unsigned)(rec_offset >> 16);
    unsigned off  = (unsigned)(rec_offset & 0xFFFFu);
    snprintf(out, 32, "RMSR%X.%X.%X", fidhi, page, off);
}

/* Release the stream's current record lock, if any -- a real $DEQ, never a
 * silent local clear (INV-6). A no-op when the stream holds none (an NLK or
 * RLK read never stashes one). */
static void rms_reclock_release(struct RAB *rab)
{
    if (rab->_rec_lock_lkid) {
        vms_kif_deq(rab->_rec_lock_lkid, NULL, 0);
        rab->_rec_lock_lkid = 0;
    }
}

/*
 * rms_reclock_after_locate - the $GET/$FIND record-lock seam. Runs AFTER the
 * org handler has located the record (rab->_last_rec_offset valid) and ONLY
 * on a successful locate. rab$l_rop decides the DLM realization -- CRITICAL:
 * RAB$M_NLK/RAB$M_RLK are READ MODIFIERS, not lock modes (the design doc's
 * corrected table, grounded in the RMS status codes):
 *
 *   default   -- the LOCKING read: EX $ENQ (parid = the file-access lock).
 *                Grant -> stash the lkid on the stream. Conflict
 *                (SS$_NOTQUEUED) -> RMS$_RLK (another stream holds it).
 *   RAB$M_NLK -- "no lock": take NO $ENQ. A dirty read; holds nothing, never
 *                blocks, never blocked. status_in passes through.
 *   RAB$M_RLK -- "read locked record": read through a lock the record may
 *                already carry, WITHOUT taking one. PROBE with a throwaway
 *                EX/NOQUEUE $ENQ: refused (SS$_NOTQUEUED) means the record
 *                IS genuinely locked by another stream right now ->
 *                RMS$_OK_RLK; granted means it was free -> immediately $DEQ
 *                (RLK holds nothing) and status_in passes through. The
 *                OK_RLK-vs-normal distinction is decided by REAL DLM state,
 *                never a userspace guess (INV-6).
 *
 * No file-access lock on this FAB (h->access_lkid == 0: ACP absent, the
 * vms-5f0 POSIX defer, or a pre-lock internal handle) -- there is no record
 * locking subsystem here; status_in passes through unchanged.
 */
static uint32_t rms_reclock_after_locate(struct FAB *fab, struct RAB *rab,
                                          uint32_t status_in)
{
    rms_file_t *h = (rms_file_t *)fab->_rms_file;
    if (!h || !h->access_lkid)
        return status_in;

    uint32_t rop = rab->rab$l_rop;
    if (rop & RAB$M_NLK)
        return status_in;

    char resnam[32];
    rms_record_lock_resnam(h, (uint64_t)rab->_last_rec_offset, resnam);

    uint32_t lkid = 0;
    uint32_t st = vms_kif_enq(0, LCK_K_EXMODE, LCK_M_NOQUEUE, resnam,
                              h->access_lkid, 0, 0, 0, &lkid, NULL);

    if (rop & RAB$M_RLK) {
        if (st == SS$_NOTQUEUED)
            return RMS$_OK_RLK;
        if ($VMS_STATUS_SUCCESS(st))
            vms_kif_deq(lkid, NULL, 0);   /* RLK holds nothing -- release it right back */
        return status_in;
    }

    /* default: the locking read */
    if (st == SS$_NOTQUEUED)
        return RMS$_RLK;
    if ($VMS_STATUS_SUCCESS(st))
        rab->_rec_lock_lkid = lkid;
    return status_in;
}

/*
 * rms_reclock_after_put - $PUT does NOT hold a persistent record lock. It
 * RELEASES the stream's current-record lock (a $PUT moves the stream off any
 * record a prior $GET held) and takes none of its own.
 *
 * Why not lock the written record: the org $PUT handlers do not populate a
 * per-record locator (rms_seq_put never sets _last_rec_offset -- the same
 * rab$w_rfa gap the design doc notes for $GET), so a $PUT lock would name a
 * STALE/colliding resource (every sequential $PUT would collide on offset 0).
 * Worse, a $PUT lock would name a stale/colliding resource that the read-back's
 * own $GET then re-derives and blocks on (the exact test_syssvc_rms_acp
 * regression). (The executive now cascades child record locks when their file
 * lock is $DEQ'd at $CLOSE -- release_child_locks, vms_lock.c -- so a held
 * record lock no longer OUTLIVES its file; but a $PUT still holds none, both
 * because its locator is unreliable and because a fresh append needs no hold.)
 * A record lock a subsequent $UPDATE needs is
 * taken by the $GET that precedes it, which is the VMS pattern the
 * done-condition exercises. A held $PUT lock (for $PUT-then-$UPDATE on the same
 * stream without an intervening $GET) waits on per-record put locators +
 * vms-489, tracked in vms-3ce.
 */
static uint32_t rms_reclock_after_put(struct FAB *fab, struct RAB *rab,
                                       uint32_t status_in)
{
    rms_file_t *h = (rms_file_t *)fab->_rms_file;
    if (!h || !h->access_lkid)
        return status_in;

    rms_reclock_release(rab);   /* $PUT holds no record lock (see above) */
    return status_in;
}

/* rms_reclock_require_held - the $UPDATE/$DELETE gate: both operate on the
 * record the stream currently holds locked from its prior default $get (its
 * stashed _rec_lock_lkid). No file-access lock on this FAB -> no record
 * locking subsystem, nothing to require. A file-access lock WITH no stashed
 * record lock (the last locate was RAB$M_NLK/RAB$M_RLK, or there was none)
 * -> RMS$_CUR ("no current record") per VMS -- never a silent write to an
 * unlocked record (INV-6). */
static int rms_reclock_require_held(struct FAB *fab, struct RAB *rab)
{
    rms_file_t *h = (rms_file_t *)fab->_rms_file;
    if (!h || !h->access_lkid)
        return 1;                  /* no lock subsystem here: nothing to gate */
    return rab->_rec_lock_lkid != 0;
}
#endif /* OVMX_HAVE_ACP */

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

#if defined(OVMX_HAVE_ACP)
    /* vms-0dd: a new locate drops whatever "current record" lock this
     * stream held before -- the DLM release, not a silent clear. */
    rms_reclock_release(rab);
#endif

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

#if defined(OVMX_HAVE_ACP)
    if ($VMS_STATUS_SUCCESS(status))
        status = rms_reclock_after_locate(fab, rab, status);
#endif

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

#if defined(OVMX_HAVE_ACP)
    if ($VMS_STATUS_SUCCESS(status))
        status = rms_reclock_after_put(fab, rab, status);
#endif

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

#if defined(OVMX_HAVE_ACP)
    /* vms-0dd: $UPDATE operates on the record this stream holds locked from
     * its prior default $get -- no stashed lock on this file-locked FAB ->
     * RMS$_CUR, never a silent write to a record nobody here holds. */
    if (!rms_reclock_require_held(fab, rab)) {
        rab->rab$l_sts = RMS$_CUR;
        return RMS$_CUR;
    }
#endif

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

#if defined(OVMX_HAVE_ACP)
    /* vms-0dd: same gate as $UPDATE -- $DELETE needs this stream's own
     * record lock on the target. */
    if (!rms_reclock_require_held(fab, rab)) {
        rab->rab$l_sts = RMS$_CUR;
        return RMS$_CUR;
    }
#endif

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

#if defined(OVMX_HAVE_ACP)
    /* vms-0dd: $FIND establishes a new "current record" exactly like $GET --
     * same release-then-acquire seam. */
    rms_reclock_release(rab);
#endif

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

#if defined(OVMX_HAVE_ACP)
    if ($VMS_STATUS_SUCCESS(status))
        status = rms_reclock_after_locate(fab, rab, status);
#endif

    rab->rab$l_sts = status;
    return status;
}


/* ============================================================
 * Block I/O: $READ / $WRITE (vms-254)
 *
 * OpenVMS RMS Reference Manual, $READ and $WRITE: transfer whole virtual
 * blocks of a file opened for block I/O (FAB$V_BIO or FAB$V_BRO in FAC), with
 * no record interpretation. Inputs: RAB$L_BKT = starting VBN (0 = the block
 * after the last one transferred on this stream), RAB$L_UBF/RAB$W_USZ for
 * $READ, RAB$L_RBF/RAB$W_RSZ for $WRITE. Outputs: RAB$W_RSZ = bytes
 * transferred (and RAB$L_RBF = RAB$L_UBF) for $READ; RAB$W_RFA = the starting
 * VBN for both. FAC GET grants $READ, FAC PUT/UPD grants $WRITE.
 *
 * End of file: a $READ that starts at or beyond the end of file returns
 * RMS$_EOF; one that runs into it transfers only the bytes up to the end of
 * file (RSZ < USZ). A $WRITE that ends beyond the end of file moves the end of
 * file to the end of the transfer -- byte-exact here (the ODS-2 FFB lands at
 * RSZ mod 512 of the last block), which is what a byte-stream client (the C
 * RTL's read()/write() on a stream file) needs to round-trip a file's length.
 *
 * The transfer runs through the file's rms_file_t: the ACP window
 * (IO$_READVBLK / IO$_WRITEVBLK) when the executive is present, the
 * executive-absent POSIX defer otherwise -- the same backend $GET/$PUT use.
 * The stream's next-block context is rab->_current_offset (a block-aligned
 * byte offset), which $CONNECT and $REWIND zero.
 * ============================================================ */
#define RMS_BIO_BLK 512u

static void rms_bio_set_rfa(struct RAB *rab, uint32_t vbn)
{
    /* RFA0 (longword) = VBN, RFA4 (word) = 0: the block-I/O RFA. */
    rab->rab$w_rfa.rfa$w_area   = (uint16_t)(vbn & 0xFFFFu);
    rab->rab$w_rfa.rfa$w_page   = (uint16_t)(vbn >> 16);
    rab->rab$w_rfa.rfa$w_offset = 0;
}

/* Common prologue: validate the RAB/FAB, check FAC, resolve the start VBN.
 * Returns RMS$_NORMAL and *start_out = the transfer's byte offset, or an
 * error status (also stored in rab$l_sts). */
static uint32_t rms_bio_begin(struct RAB *rab, struct FAB **fab_out,
                              int writing, uint64_t *start_out)
{
    if (!rab || rab->rab$b_bid != RAB$C_BID)
        return RMS$_RAB;
    struct FAB *fab = validate_rab(rab);
    if (!fab)
        return rab->rab$l_sts;

    uint8_t need = writing ? (uint8_t)(FAB$M_PUT | FAB$M_UPD) : (uint8_t)FAB$M_GET;
    if (!(fab->fab$b_fac & (FAB$M_BIO | FAB$M_BRO)) || !(fab->fab$b_fac & need)) {
        rab->rab$l_sts = RMS$_FAC;
        return RMS$_FAC;
    }

    uint64_t start;
    if (rab->rab$l_bkt != 0)
        start = (uint64_t)(rab->rab$l_bkt - 1u) * RMS_BIO_BLK;
    else
        start = rab->_current_offset < 0 ? 0 : (uint64_t)rab->_current_offset;

    *fab_out = fab;
    *start_out = start;
    return RMS$_NORMAL;
}

static uint32_t rms_impl_read(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    struct FAB *fab = NULL;
    uint64_t start = 0;
    uint32_t st = rms_bio_begin(rab, &fab, 0, &start);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    if (rab->rab$w_usz == 0 || !rab->rab$l_ubf) {
        rab->rab$l_sts = RMS$_USZ;
        return RMS$_USZ;
    }

    rms_file_t *f = fab->_rms_file;
    off_t eof = rms_io_lseek(f, 0, SEEK_END);
    if (eof < 0) {
        rab->rab$l_sts = RMS$_RER;
        return RMS$_RER;
    }
    rms_bio_set_rfa(rab, (uint32_t)(start / RMS_BIO_BLK) + 1u);
    if (start >= (uint64_t)eof) {
        rab->rab$w_rsz = 0;
        rab->rab$l_sts = RMS$_EOF;
        return RMS$_EOF;
    }
    if (rms_io_lseek(f, (off_t)start, SEEK_SET) < 0) {
        rab->rab$l_sts = RMS$_RER;
        return RMS$_RER;
    }
    ssize_t n = rms_io_read(f, rab->rab$l_ubf, rab->rab$w_usz);
    if (n < 0) {
        rab->rab$l_sts = RMS$_RER;
        return RMS$_RER;
    }
    rab->rab$l_rbf = rab->rab$l_ubf;
    rab->rab$w_rsz = (uint16_t)n;
    rab->_current_offset =
        (off_t)(start + (((uint64_t)n + RMS_BIO_BLK - 1u) / RMS_BIO_BLK) * RMS_BIO_BLK);
    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

static uint32_t rms_impl_write(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    struct FAB *fab = NULL;
    uint64_t start = 0;
    uint32_t st = rms_bio_begin(rab, &fab, 1, &start);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    if (rab->rab$w_rsz != 0 && !rab->rab$l_rbf) {
        rab->rab$l_sts = RMS$_RBF;
        return RMS$_RBF;
    }

    rms_file_t *f = fab->_rms_file;
    rms_bio_set_rfa(rab, (uint32_t)(start / RMS_BIO_BLK) + 1u);
    if (rms_io_lseek(f, (off_t)start, SEEK_SET) < 0) {
        rab->rab$l_sts = RMS$_WER;
        return RMS$_WER;
    }
    if (rab->rab$w_rsz != 0) {
        ssize_t n = rms_io_write(f, rab->rab$l_rbf, rab->rab$w_rsz);
        if (n != (ssize_t)rab->rab$w_rsz) {
            rab->rab$l_sts = RMS$_WER;
            return RMS$_WER;
        }
    }
    rab->_current_offset =
        (off_t)(start + (((uint64_t)rab->rab$w_rsz + RMS_BIO_BLK - 1u) / RMS_BIO_BLK) * RMS_BIO_BLK);
    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
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

uint32_t sys$read(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_read(rab), rab, err, suc);
}

uint32_t sys$write(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_write(rab), rab, err, suc);
}
