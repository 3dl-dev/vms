/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_sysdisk.c - the SYS$DISK read adapter (epic vms-5eb, the ODS-2 runtime
 * flip). See vmsfs/sysdisk.h for the contract and the atomic-safety /
 * fail-honest rationale.
 *
 * This module lives WITH vmsfs_volume.c, OUTSIDE the LIBVMSFS$SHR.EXE strict
 * symbol-vector shareable (src/vmsfs/CMakeLists.txt links it into the distinct
 * vmsfs_volume static library): it consumes vmsfs_volume_handle() +
 * ods2_bdev_*, whose pread/block primitives must NOT cascade into the live
 * shareable before the atomic group (R2/R3/R5/R6) is ready. Its consumers link
 * it directly (RMS/DCL/MOUNT when the flip lands).
 *
 * All this module adds over vmsfs_volume.c + ods2_path.c is the PATH BRIDGE:
 * "/vms/A/B/.../NAME.EXT;ver"  ->  { comps={A,B,...}, filename="NAME.EXT",
 * version=ver }  ->  the ods2_bdev resolve/read/list calls over the handle.
 * It adds NO on-disk format facts (Rule 8) -- it only sequences the reader's
 * existing directory-walk + extent-read primitives, keyed off the resolved
 * Linux path every /vms consumer already computes via vmsfs_to_linux_path.
 *
 * OVMX Project
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>   /* flock(2) -- the single-writer serialization broker */

#include "vmsfs/sysdisk.h"
#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "ovmx_layout.h"   /* SYSDISK_MOUNT ("/vms"), SYSDISK_DEVICE ("DKA0") */
#include "ssdef.h"

/*
 * Path shape limits. A resolved SYS$DISK path is "/vms" + up to
 * SYSDISK_MAX_COMPS "/"-separated directory components + a final filename,
 * each up to SYSDISK_MAX_NAME bytes (ODS-2 long names run to 86 chars; the
 * cap is generous but bounded so nothing here allocates from the path).
 */
#define SYSDISK_MAX_COMPS   32
#define SYSDISK_MAX_NAME    128

/*
 * Map a genuine-ODS-2 reader status to a VMS status code (odd == success).
 * NOTFOUND is the ordinary "no such file/dir"; a checksum/format/range/IO
 * error is a real medium fault reported HONESTLY as SS$_DATACHECK, never
 * masked as "file not found".
 */
static int ods2_status_to_vms(ods2_status_t st)
{
    switch (st) {
    case ODS2_OK:            return SS$_NORMAL;
    case ODS2_ERR_NOTFOUND:  return SS$_NOSUCHFILE;
    case ODS2_ERR_ARGS:      return SS$_BADPARAM;
    case ODS2_ERR_SIZE:      return SS$_DATACHECK;
    case ODS2_ERR_CHECKSUM:
    case ODS2_ERR_FORMAT:
    case ODS2_ERR_RANGE:
    case ODS2_ERR_IO:
    default:                 return SS$_DATACHECK;
    }
}

/*
 * Cross-process SYS$DISK handle resolver (epic vms-5eb, the atomic flip's
 * missing substrate).
 *
 * vmsfs_volume.c's table is process-local by construction (an fd + ods2_bdev_t
 * are per-process). PID 1 registers DKA0: at boot via register_system_volume(),
 * but the flip's live consumers -- RMS in LOGINOUT, DCL in the shell image,
 * every RUN'd image -- run in OTHER processes whose tables start empty. Without
 * a per-process registration path each of those would get a NULL handle and
 * fail SS$_DEVNOTMOUNT, so the reroute could never reach login. This helper
 * closes that gap: it returns the already-registered handle if present, else
 * lazily registers SYS$DISK in THIS process from the boot device path PID 1
 * exports in the OVMX_SYSDISK_DEV environment variable (inherited across VMS
 * image activation), then returns the freshly-registered handle.
 *
 * OVMX DESIGN CHOICE (not VMS-authentic; labelled per CLAUDE.md Rule 8). The
 * env-exported backing path is the minimal honest cross-process channel for the
 * flip; the VMS-faithful refinement is to translate the SYS$SYSDEVICE logical
 * to the boot device and resolve its backing store, which a later rung can
 * substitute here without touching any consumer.
 *
 * FAIL-HONEST (Rule 9 / INV-6). If OVMX_SYSDISK_DEV is unset/empty, or names a
 * store that is absent or not a genuine ODS-2 volume, registration fails and
 * this returns NULL -- the caller then surfaces SS$_DEVNOTMOUNT, never a silent
 * POSIX fallback. Registration is idempotent: a second call returns the same
 * cached handle (vmsfs_volume_register re-registers by name).
 */
static const ods2_bdev_t *sysdisk_handle(void)
{
    const ods2_bdev_t *bv = vmsfs_volume_handle(SYSDISK_DEVICE);
    if (bv)
        return bv;

    const char *backing = getenv("OVMX_SYSDISK_DEV");
    if (!backing || !backing[0])
        return NULL;   /* fail-honest: no boot-device channel in this process */

    if (vmsfs_volume_register(SYSDISK_DEVICE, backing, NULL) != SS$_NORMAL)
        return NULL;   /* fail-honest: not a genuine ODS-2 volume / unopenable */

    return vmsfs_volume_handle(SYSDISK_DEVICE);
}

int ods2_sysdisk_owns_path(const char *linux_path)
{
    if (!linux_path)
        return 0;

    size_t mlen = strlen(SYSDISK_MOUNT);
    if (strncmp(linux_path, SYSDISK_MOUNT, mlen) != 0)
        return 0;

    /* Boundary: exactly "/vms", or "/vms/..." -- never "/vmsXYZ". */
    char after = linux_path[mlen];
    return (after == '\0' || after == '/');
}

/*
 * Split the portion of `linux_path` after SYSDISK_MOUNT into non-empty
 * "/"-separated components. `store` is a caller-provided
 * [SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME] scratch buffer; `comps` is filled with
 * pointers into it. Returns the component count, or -1 on overflow / malformed
 * input. A trailing '/' and repeated '/' are tolerated (empty segments
 * skipped), mirroring POSIX path leniency.
 */
static int split_path(const char *linux_path,
                      char store[][SYSDISK_MAX_NAME],
                      const char *comps[SYSDISK_MAX_COMPS])
{
    const char *p = linux_path + strlen(SYSDISK_MOUNT);
    int n = 0;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - start);

        if (n >= SYSDISK_MAX_COMPS || len == 0 || len >= SYSDISK_MAX_NAME)
            return -1;

        memcpy(store[n], start, len);
        store[n][len] = '\0';
        comps[n] = store[n];
        n++;
    }

    return n;
}

/*
 * Split a final path component "NAME.EXT;ver" into the ODS-2 directory-entry
 * name ("NAME.EXT", the ";ver" suffix removed in place) and the version. An
 * absent or ";0" version yields 0 == "highest". A non-numeric ";..." is left
 * attached to the name (an odd but harmless literal match attempt) -- the
 * common, well-formed case is what matters.
 */
static uint16_t split_version(char *filename)
{
    char *semi = strchr(filename, ';');
    if (!semi)
        return 0;

    char *endp = NULL;
    long v = strtol(semi + 1, &endp, 10);
    if (endp == semi + 1 || (endp && *endp != '\0'))
        return 0;   /* not a plain number -- leave name as-is, want highest */

    *semi = '\0';   /* strip ";ver" from the name */
    if (v < 0)
        v = 0;
    if (v > 0xFFFF)
        v = 0xFFFF;
    return (uint16_t)v;
}

int ods2_sysdisk_resolve_file(const char *linux_path,
                              const ods2_bdev_t **bv_out,
                              ods2_fid_t *fid_out,
                              void *header_out, size_t header_len)
{
    if (bv_out)
        *bv_out = NULL;

    if (!linux_path || !header_out || header_len < ODS2_BLOCK_SIZE)
        return SS$_BADPARAM;
    if (!ods2_sysdisk_owns_path(linux_path))
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = sysdisk_handle();
    if (!bv)
        return SS$_DEVNOTMOUNT;   /* fail-honest: no ODS-2 SYS$DISK mounted */

    char store[SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME];
    const char *comps[SYSDISK_MAX_COMPS];
    int n = split_path(linux_path, store, comps);
    if (n < 1)
        return SS$_BADPARAM;   /* no filename component (the MFD is not a file) */

    /* Last component is the filename (with an optional ";ver"); the preceding
     * components are the directory chain walked from the MFD. */
    char *filename = store[n - 1];
    uint16_t version = split_version(filename);
    unsigned ndirs = (unsigned)(n - 1);

    ods2_status_t st = ods2_bdev_resolve_file(bv, comps, ndirs, filename,
                                              version, fid_out,
                                              header_out, header_len);
    if (st != ODS2_OK)
        return ods2_status_to_vms(st);

    if (bv_out)
        *bv_out = bv;
    return SS$_NORMAL;
}

int ods2_sysdisk_read_file(const char *linux_path,
                           void *out, size_t out_cap, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!out)
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = NULL;
    uint8_t header[ODS2_BLOCK_SIZE];
    int vst = ods2_sysdisk_resolve_file(linux_path, &bv, NULL,
                                        header, sizeof(header));
    if (vst != SS$_NORMAL)
        return vst;

    size_t got = 0;
    ods2_status_t st = ods2_bdev_read_file(bv, header, out, out_cap, &got);
    if (st != ODS2_OK)
        return ods2_status_to_vms(st);

    if (out_len)
        *out_len = got;
    return SS$_NORMAL;
}

int ods2_sysdisk_list_dir(const char *linux_path, ods2_dir_cb cb, void *ctx)
{
    if (!linux_path || !cb)
        return SS$_BADPARAM;
    if (!ods2_sysdisk_owns_path(linux_path))
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = sysdisk_handle();
    if (!bv)
        return SS$_DEVNOTMOUNT;

    char store[SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME];
    const char *comps[SYSDISK_MAX_COMPS];
    int n = split_path(linux_path, store, comps);
    if (n < 0)
        return SS$_BADPARAM;   /* n == 0 is legal: list the MFD */

    uint8_t header[ODS2_BLOCK_SIZE];
    ods2_status_t st = ods2_bdev_resolve_dir(bv, comps, (unsigned)n, NULL,
                                             header, sizeof(header));
    if (st != ODS2_OK)
        return ods2_status_to_vms(st);

    st = ods2_bdev_list_dir(bv, header, cb, ctx);
    if (st != ODS2_OK)
        return ods2_status_to_vms(st);

    return SS$_NORMAL;
}

/* ================================================================
 * SYS$DISK WRITE adapter (vms-02e, epic vms-5eb -- the WRITE half of the
 * ODS-2 runtime flip). See vmsfs/sysdisk.h for the contract, the ADDITIVE /
 * fail-honest rationale, and the concurrency note. Each entry point opens a
 * per-call block-device-backed writer over the registered SYS$DISK volume's
 * fd (ods2_wvolume_open_bdev), mutates, and closes it (flush + free, fd
 * borrowed). It adds NO on-disk format facts (Rule 8) -- it only sequences
 * the writer's existing create/append/dir-insert primitives over the same
 * "/vms/..." path bridge the read adapter uses.
 * ================================================================ */

/* Usable byte span of a registered volume handle (its validated block count). */
static uint64_t sysdisk_span(const ods2_bdev_t *bv)
{
    return (uint64_t)bv->nblocks * (uint64_t)ODS2_BLOCK_SIZE;
}

/* ----------------------------------------------------------------------------
 * SINGLE-WRITER SERIALIZATION BROKER (vms-49d).
 *
 * The one SYS$DISK block device has NO transaction manager, yet at boot THREE
 * independent processes write it concurrently: PID 1's OPERATOR.LOG append
 * (opcom), a login LASTLOGIN write, and RMS $PUT. Each write entry point below
 * opens a per-call block-device-backed writer (ods2_wvolume_open_bdev, which
 * RECONSTRUCTS the free-block and free-FID watermarks by reading the on-disk
 * bitmaps), mutates, and flushes. Two such spans interleaving on the same
 * device tear blocks and lose updates: both open_bdev calls read the SAME
 * watermark and allocate the SAME LBN/FID, and two dir_insert read-modify-
 * writes of one parent directory block clobber each other (last writer wins,
 * dropping the other's entry). Because the watermark is read INSIDE open_bdev,
 * the critical section must begin BEFORE open_bdev and end AFTER close -- so
 * the lock lives here at the adapter entry points, not down inside the writer.
 *
 * MECHANISM: flock(fd, LOCK_EX) on the registered volume's block-device fd,
 * held across the whole open -> read-modify-write -> flush -> close span, then
 * released. flock is a REAL OS-level, inode-associated advisory lock: two
 * DIFFERENT processes (each with its own open file description on the device,
 * exactly the boot case -- every process lazily registers SYS$DISK for itself
 * via sysdisk_handle()) contend on it and are serialized. This is INV-6 clean:
 * it is a kernel lock on the shared object, never a per-process fake that
 * reports success while sharing nothing.
 *
 * OVMX DESIGN CHOICE (not VMS-authentic; labelled per CLAUDE.md Rule 8). The
 * VMS-faithful mechanism is an executive Distributed Lock Manager resource --
 * sys$enq of an EX-mode lock on a per-volume resource name -- which also
 * generalizes to the clustered/MSCP-served case. flock-on-device is the
 * minimal correct single-node broker for the additive write half; a later rung
 * can substitute a DLM lock here without touching any consumer. See
 * docs/design-ods2-runtime-flip.md.
 *
 * FAIL-SAFE: if flock somehow fails (e.g. EINTR after retry, or a backing fd
 * that does not support locking), the write proceeds UNSERIALIZED rather than
 * failing the write -- honesty about the medium (the data would still be
 * written) is preferred to spuriously refusing a boot-time log append. This is
 * a best-effort broker over an existing correct single-writer path, not a new
 * failure mode. The concurrency test (tests/ods2/test_ods2_write_broker.c)
 * proves the lock is what prevents corruption under real fork()'d contention.
 */
static int sysdisk_wlock(int fd)
{
    int rc;
    do {
        rc = flock(fd, LOCK_EX);
    } while (rc != 0 && errno == EINTR);
    return rc;   /* 0 == held; nonzero == unlocked (proceed unserialized) */
}

static void sysdisk_wunlock(int fd, int locked)
{
    if (locked == 0)
        (void)flock(fd, LOCK_UN);
}

int ods2_sysdisk_create_file(const char *linux_path,
                             const void *data, size_t data_len)
{
    if (!linux_path || (!data && data_len > 0))
        return SS$_BADPARAM;
    if (!ods2_sysdisk_owns_path(linux_path))
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = sysdisk_handle();
    if (!bv)
        return SS$_DEVNOTMOUNT;   /* fail-honest: no ODS-2 SYS$DISK mounted */

    char store[SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME];
    const char *comps[SYSDISK_MAX_COMPS];
    int n = split_path(linux_path, store, comps);
    if (n < 1)
        return SS$_BADPARAM;      /* no filename component (cannot create the MFD) */

    char *filename = store[n - 1];
    uint16_t version = split_version(filename);
    if (version == 0)
        version = 1;              /* an absent ";ver" creates version 1 */
    unsigned ndirs = (unsigned)(n - 1);

    /* vms-49d broker: serialize the whole resolve -> open_bdev -> mutate ->
     * flush span against every other writer of this device (held before
     * open_bdev, whose watermark read is part of the critical section). */
    int locked = sysdisk_wlock(bv->fd);

    /* Resolve the (already-existing) parent directory over the reader handle. */
    uint8_t dirhdr[ODS2_BLOCK_SIZE];
    ods2_fid_t parent_fid;
    ods2_status_t st = ods2_bdev_resolve_dir(bv, comps, ndirs, &parent_fid,
                                             dirhdr, sizeof(dirhdr));
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    ods2_wvolume_t wvol;
    st = ods2_wvolume_open_bdev(bv->fd, sysdisk_span(bv), &wvol);
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    ods2_fid_t newfid;
    st = ods2_wvolume_create_file_raw(&wvol, filename, version,
                                      (const uint8_t *)data, data_len,
                                      parent_fid, &newfid);
    if (st == ODS2_OK)
        st = ods2_wvolume_dir_insert(&wvol, parent_fid, filename, version,
                                     newfid);
    ods2_wvolume_close(&wvol);   /* flushes; surfaces any pwrite failure below */
    sysdisk_wunlock(bv->fd, locked);

    return ods2_status_to_vms(st);
}

int ods2_sysdisk_append_file(const char *linux_path,
                             const void *data, size_t data_len)
{
    if (!linux_path || (!data && data_len > 0))
        return SS$_BADPARAM;
    if (!ods2_sysdisk_owns_path(linux_path))
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = sysdisk_handle();
    if (!bv)
        return SS$_DEVNOTMOUNT;

    char store[SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME];
    const char *comps[SYSDISK_MAX_COMPS];
    int n = split_path(linux_path, store, comps);
    if (n < 1)
        return SS$_BADPARAM;

    char *filename = store[n - 1];
    uint16_t version = split_version(filename);   /* 0 == highest */
    unsigned ndirs = (unsigned)(n - 1);

    /* vms-49d broker: serialize the whole span (see ods2_sysdisk_create_file). */
    int locked = sysdisk_wlock(bv->fd);

    /* Resolve the EXISTING file's FID over the reader handle. */
    uint8_t fhdr[ODS2_BLOCK_SIZE];
    ods2_fid_t file_fid;
    ods2_status_t st = ods2_bdev_resolve_file(bv, comps, ndirs, filename,
                                              version, &file_fid,
                                              fhdr, sizeof(fhdr));
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    ods2_wvolume_t wvol;
    st = ods2_wvolume_open_bdev(bv->fd, sysdisk_span(bv), &wvol);
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    st = ods2_wvolume_append_file(&wvol, file_fid, data, data_len);
    ods2_wvolume_close(&wvol);
    sysdisk_wunlock(bv->fd, locked);

    return ods2_status_to_vms(st);
}

int ods2_sysdisk_mkdir(const char *linux_path)
{
    if (!linux_path)
        return SS$_BADPARAM;
    if (!ods2_sysdisk_owns_path(linux_path))
        return SS$_BADPARAM;

    const ods2_bdev_t *bv = sysdisk_handle();
    if (!bv)
        return SS$_DEVNOTMOUNT;

    char store[SYSDISK_MAX_COMPS][SYSDISK_MAX_NAME];
    const char *comps[SYSDISK_MAX_COMPS];
    int n = split_path(linux_path, store, comps);
    if (n < 1)
        return SS$_BADPARAM;      /* need at least the new directory's name */

    /* The last component names the new directory (bare, no type); its on-disk
     * directory-entry name is "NAME.DIR". The preceding components are the
     * already-existing parent chain. */
    const char *newname = comps[n - 1];
    char dirname[SYSDISK_MAX_NAME + 8];
    int m = snprintf(dirname, sizeof(dirname), "%s.DIR", newname);
    if (m <= 0 || (size_t)m >= sizeof(dirname))
        return SS$_BADPARAM;
    unsigned ndirs = (unsigned)(n - 1);

    /* vms-49d broker: serialize the whole span (see ods2_sysdisk_create_file). */
    int locked = sysdisk_wlock(bv->fd);

    uint8_t dirhdr[ODS2_BLOCK_SIZE];
    ods2_fid_t parent_fid;
    ods2_status_t st = ods2_bdev_resolve_dir(bv, comps, ndirs, &parent_fid,
                                             dirhdr, sizeof(dirhdr));
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    ods2_wvolume_t wvol;
    st = ods2_wvolume_open_bdev(bv->fd, sysdisk_span(bv), &wvol);
    if (st != ODS2_OK) {
        sysdisk_wunlock(bv->fd, locked);
        return ods2_status_to_vms(st);
    }

    ods2_fid_t newfid;
    st = ods2_wvolume_create_dir(&wvol, dirname, 1, parent_fid, &newfid);
    if (st == ODS2_OK)
        st = ods2_wvolume_dir_insert(&wvol, parent_fid, dirname, 1, newfid);
    ods2_wvolume_close(&wvol);
    sysdisk_wunlock(bv->fd, locked);

    return ods2_status_to_vms(st);
}
