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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

    const ods2_bdev_t *bv = vmsfs_volume_handle(SYSDISK_DEVICE);
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

    const ods2_bdev_t *bv = vmsfs_volume_handle(SYSDISK_DEVICE);
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
