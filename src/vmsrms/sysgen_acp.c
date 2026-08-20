/*
 * sysgen_acp.c - SYS$SYSTEM:OVMXVMSSYS.PAR I/O over the executive Files-11
 * (ODS-2) ACP (vms-5f0 residual, epic vms-208).
 *
 * THE RESIDUAL THIS CLOSES. The atomic flip made SYS$DISK a genuine ODS-2
 * volume the executive ACP owns and retired the /vms POSIX passthrough. But the
 * SYSGEN parameter file was still read AND written through
 * vmsfs_to_linux_path()+fopen() (sysgen_params.h's inline readers,
 * tools/vms_sysgen.c's WRITE CURRENT, src/ovmx_init/sysboot.c) -- i.e. off /vms,
 * which no longer exists. So SYSGEN WRITE CURRENT never persisted to the real
 * volume (SCSNODE authored in one boot was gone by the next -- vms-b6a7), and a
 * fresh SCSD read the seed default instead of the authored RECNXINTERVAL
 * (vms-c3b). This module routes both directions through the SAME RMS-over-ACP
 * substrate every other on-volume file now uses (sysuaf_live.c is the model):
 * rms_open_named_handle() $ASSIGNs the mounted volume, IO$_ACCESS/IO$_CREATE
 * builds the window, and rms_io_* moves the bytes as virtual blocks over
 * /dev/vms.
 *
 * WHY BLOCK I/O AND NOT $GET/$PUT RECORDS. OVMXVMSSYS.PAR is a single fixed
 * binary blob (struct sysgen_file), NOT a record file -- the historical fopen
 * fwrote/fread the whole struct. rms_io_read_exact/write_exact move raw virtual
 * blocks through the ACP window and are oblivious to the FH2 record format, so
 * the blob round-trips byte-identically regardless of the created file's RFM
 * (RFM=VAR default). This is exactly the positioned-block vocabulary rms_io.h
 * documents.
 *
 * WEAK SEAM. These are the strong definitions of the ovmx_sysgen_acp_* symbols
 * sysgen_params.h declares #pragma weak, the same layering sysuaf.c uses for
 * ovmx_sysuaf_*: an image that links LIBVMSRMS$SHR (SYSGEN.EXE, SCSD.EXE,
 * SYSMAN.EXE, DCL.EXE, ...) resolves them and reaches the volume; an image that
 * does not (libvms's own inline callers) sees NULL and fails honest -- there is
 * NO /vms fallback (CLAUDE.md Rule 9 / INV-6).
 *
 * FAIL-HONEST. rms_open_named_handle() returns SS$_NOSUCHDEV when /dev/vms is
 * absent and SS$_NOSUCHFILE when the file is not on the volume; both are
 * propagated. A short/corrupt read is SS$_DATACHECK. Success is never faked
 * against a private substitute.
 */

#define _POSIX_C_SOURCE 200809L

/* This is the DEFINING TU: keep ovmx_sysgen_acp_read/write STRONG so they land
 * in LIBVMSRMS$SHR's native symbol vector (generated from nm type 'T'). Must be
 * set BEFORE sysgen_params.h, which #pragma-weaks them otherwise. */
#define OVMX_SYSGEN_ACP_STRONG 1

#include <stdint.h>
#include <string.h>

#include "sysgen_params.h"   /* struct sysgen_file, SYSGEN_MAGIC/VERSION */
#include "rms_io.h"
#include "ssdef.h"
#include "ovmx_layout.h"     /* VMS_PARAMS_PATH */

/*
 * ovmx_sysgen_acp_read - read the HIGHEST version of SYS$SYSTEM:OVMXVMSSYS.PAR
 * off the ACP-mounted system volume into *out (USE CURRENT semantics).
 * Returns a VMS status: SS$_NORMAL on success, the ACP open status
 * (SS$_NOSUCHFILE / SS$_NOSUCHDEV) on failure, SS$_DATACHECK on a short read or
 * a bad magic/version.
 */
uint32_t ovmx_sysgen_acp_read(struct sysgen_file *out)
{
    if (!out)
        return SS$_BADPARAM;

    uint32_t st = SS$_NOSUCHFILE;
    rms_file_t *h = rms_open_named_handle(VMS_PARAMS_PATH, /*want_write=*/0,
                                          /*create=*/0, &st);
    if (!h)
        return $VMS_STATUS_SUCCESS(st) ? SS$_NOSUCHFILE : st;

    struct sysgen_file tmp;
    ssize_t r = rms_io_read_exact(h, &tmp, sizeof(tmp));
    rms_close_named_handle(h);

    if (r < (ssize_t)sizeof(tmp))
        return SS$_DATACHECK;
    if (tmp.magic != SYSGEN_MAGIC || tmp.version != SYSGEN_VERSION)
        return SS$_DATACHECK;
    if (tmp.count > SYSGEN_MAX_PARAMS)
        tmp.count = SYSGEN_MAX_PARAMS;

    *out = tmp;
    return SS$_NORMAL;
}

/*
 * ovmx_sysgen_acp_write - persist *db to SYS$SYSTEM:OVMXVMSSYS.PAR on the
 * ACP-mounted system volume.
 *
 *   new_version != 0 (WRITE CURRENT): IO$_CREATE a fresh highest+1 version
 *       (the oracle's ALPHAVMSSYS.PAR ";2 over ;1" versioning) and write the
 *       blob into it. *out_version receives the minted version.
 *   new_version == 0 (WRITE ACTIVE):  open the highest existing version for
 *       write and overwrite it in place, so the running readers see the change
 *       without minting a new boot generation. If no version exists yet, this
 *       falls through to a create. *out_version receives the resolved version.
 *
 * Returns a VMS status (SS$_NORMAL, or the ACP failure status).
 */
uint32_t ovmx_sysgen_acp_write(const struct sysgen_file *db, int new_version,
                               int *out_version)
{
    if (!db)
        return SS$_BADPARAM;

    uint32_t st = SS$_ABORT;
    rms_file_t *h = NULL;

    if (!new_version) {
        /* ACTIVE: overwrite the highest existing version in place. */
        h = rms_open_named_handle(VMS_PARAMS_PATH, /*want_write=*/1,
                                  /*create=*/0, &st);
        /* No file yet -> mint version 1 (create), same as sysgen_commit_working. */
        if (!h && (st == SS$_NOSUCHFILE || $VMS_STATUS_SUCCESS(st)))
            new_version = 1;
        else if (!h)
            return st;
    }

    if (new_version) {
        h = rms_open_named_handle(VMS_PARAMS_PATH, /*want_write=*/1,
                                  /*create=*/1, &st);
        if (!h)
            return $VMS_STATUS_SUCCESS(st) ? SS$_ABORT : st;
    }

    int rc = rms_io_write_exact(h, db, sizeof(*db));
    int ver = (int)h->version;
    rms_io_fsync(h);
    rms_close_named_handle(h);

    if (rc != 0)
        return SS$_ABORT;
    if (out_version)
        *out_version = ver;
    return SS$_NORMAL;
}
