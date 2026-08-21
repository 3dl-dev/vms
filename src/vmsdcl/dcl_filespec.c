/*
 * dcl_filespec.c - VMS filespec handling for DCL
 *
 * Resolves VMS-style filespecs to Linux paths and vice versa.
 * All VMS-to-Linux translation is delegated to vmsfs_to_linux_path()
 * — the single translation function in the system.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/dcl_rms.h"          /* vms-481: DCL file access via RMS/$QIO-ACP */
#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"
#include "ovmx_layout.h"

/* Forward declaration of logical name translation */
extern int dcl_translate_logical(const char *name, char *result, size_t result_size);

/* Forward declaration of vmsfs case-insensitive lookup */
extern int vmsfs_find_case_insensitive(const char *dir_path, const char *name,
                                        char *result, size_t result_size);

/*
 * Resolve the VMS version marker on a Linux path in-place.
 *
 * sys$create() (src/vmsrms/rms_core.c) names files on disk with a real
 * ";N" suffix — "PARTS.DAT" is actually stored as "parts.dat;1". VMS
 * filespec lookup rules for a *specific* file (as opposed to a
 * DIRECTORY/PURGE wildcard scan, handled separately by
 * vmsfs_wildcard_match()) are:
 *
 *   - An explicit ";N" (N>0) names that exact version — left untouched;
 *     it is up to the caller's stat()/open() to report NOT FOUND if that
 *     version does not exist.
 *   - No version, a bare ";", or ";0" all mean "the highest existing
 *     version" — resolved here by scanning the directory the same way
 *     rms_core.c's own rms_resolve_version() does, and appending the
 *     ";N" that is actually on disk.
 *   - If no version of the file exists yet at all (the caller is about
 *     to create it, e.g. a COPY target), the path is left unversioned —
 *     this preserves prior behavior for file creation.
 *   - If only a legacy unversioned file exists (no ";N" copy at all —
 *     vmsfs_get_highest_version() reports that case as "version 1" per
 *     its own contract), the path is left unversioned too, since that is
 *     what is actually on disk.
 */
static void resolve_version_suffix(char *path, size_t path_size)
{
    char *last_slash = strrchr(path, '/');
    char *filename = last_slash ? last_slash + 1 : path;

    char *semi = strrchr(filename, ';');
    if (semi) {
        const char *p = semi + 1;
        if (*p == '\0') {
            *semi = '\0';  /* Bare ';' -> resolve to highest, below */
        } else {
            int alldigits = 1;
            for (const char *q = p; *q; q++) {
                if (!isdigit((unsigned char)*q)) { alldigits = 0; break; }
            }
            if (!alldigits) return;  /* Not a recognized version marker */

            long ver = strtol(p, NULL, 10);
            if (ver != 0) return;   /* Explicit ;N (N>0) — leave as-is */
            *semi = '\0';           /* ;0 -> resolve to highest, below */
        }
    }

    if (!last_slash) return;  /* No directory to scan against */

    char dir_part[VMSFS_MAX_PATH];
    size_t dir_len = (size_t)(filename - path - 1);
    if (dir_len == 0 || dir_len >= sizeof(dir_part)) return;
    memcpy(dir_part, path, dir_len);
    dir_part[dir_len] = '\0';

    char basename_buf[VMSFS_MAX_NAME + 1] = "";
    char ext_buf[VMSFS_MAX_TYPE + 1] = "";
    char *dot = strrchr(filename, '.');
    if (dot) {
        size_t blen = (size_t)(dot - filename);
        if (blen >= sizeof(basename_buf)) blen = sizeof(basename_buf) - 1;
        memcpy(basename_buf, filename, blen);
        basename_buf[blen] = '\0';
        strncpy(ext_buf, dot + 1, sizeof(ext_buf) - 1);
        ext_buf[sizeof(ext_buf) - 1] = '\0';
    } else {
        strncpy(basename_buf, filename, sizeof(basename_buf) - 1);
        basename_buf[sizeof(basename_buf) - 1] = '\0';
    }
    if (!basename_buf[0]) return;

    int highest = vmsfs_get_highest_version(dir_part, basename_buf,
                                             ext_buf[0] ? ext_buf : NULL);
    if (highest <= 0) return;  /* File doesn't exist at all yet */

    /* Confirm a real "name.type;highest" file exists before appending —
     * vmsfs_get_highest_version() also reports "1" for a plain
     * unversioned file, which must NOT get a synthesized ";1" suffix
     * since no file by that literal name exists. */
    size_t flen = strlen(filename);
    size_t avail = path_size - (size_t)(filename - path);
    int n = snprintf(filename + flen, avail - flen, ";%d", highest);
    if (n < 0 || (size_t)n >= avail - flen) return;

    struct stat st;
    if (stat(path, &st) != 0) {
        filename[flen] = '\0';  /* Not real — fall back to unversioned */
    }
}

/*
 * Case-insensitive file lookup in a directory.
 * Tries: the path as-is, then uppercase filename, then directory scan.
 * Returns 0 on success, -1 on failure.
 * On success, linux_path is updated to the found path.
 */
static int resolve_case(char *linux_path, size_t path_size)
{
    if (!linux_path[0] || linux_path[0] != '/')
        return 0;  /* No resolution needed for relative or empty paths */

    struct stat st;
    if (stat(linux_path, &st) == 0)
        return 0;  /* Path already exists as-is */

    /* Split into directory and filename */
    char *last_slash = strrchr(linux_path, '/');
    if (!last_slash || last_slash == linux_path)
        return -1;

    char dir_part[VMSFS_MAX_PATH];
    size_t dlen = (size_t)(last_slash - linux_path);
    if (dlen >= sizeof(dir_part)) return -1;
    memcpy(dir_part, linux_path, dlen);
    dir_part[dlen] = '\0';

    const char *filename = last_slash + 1;
    if (!filename[0]) return -1;

    /* Try uppercase filename */
    char upper[512];
    size_t i;
    for (i = 0; i < sizeof(upper) - 1 && filename[i]; i++)
        upper[i] = (char)toupper((unsigned char)filename[i]);
    upper[i] = '\0';

    char try_path[VMSFS_MAX_PATH];
    snprintf(try_path, sizeof(try_path), "%s/%s", dir_part, upper);
    if (stat(try_path, &st) == 0) {
        strncpy(linux_path, try_path, path_size - 1);
        linux_path[path_size - 1] = '\0';
        return 0;
    }

    /* Case-insensitive directory scan */
    char found_name[256];
    if (vmsfs_find_case_insensitive(dir_part, filename,
                                     found_name, sizeof(found_name)) == 0) {
        snprintf(try_path, sizeof(try_path), "%s/%s", dir_part, found_name);
        strncpy(linux_path, try_path, path_size - 1);
        linux_path[path_size - 1] = '\0';
        return 0;
    }

    /* Not found — leave the path as-is (for new file creation) */
    return -1;
}

/*
 * Resolve a VMS filespec to a Linux path.
 *
 * Delegates entirely to vmsfs_to_linux_path() for the VMS→Linux
 * translation, then does DCL-specific post-processing:
 *   - Strip version suffix (;N) since Linux doesn't use file versions
 *   - Case-insensitive file lookup with fallback to uppercase
 *
 * Returns 0 on success.
 */
int dcl_resolve_filespec(struct dcl_context *ctx, const char *spec,
                         char *linux_path, size_t path_size)
{
    (void)ctx;  /* defaults are handled by caller building full spec */

    if (!spec || !linux_path || path_size == 0) return -1;

    int status = vmsfs_to_linux_path(spec, linux_path, path_size);
    if (!$VMS_STATUS_SUCCESS(status)) {
        linux_path[0] = '\0';
        return -1;
    }

    /* Resolve the VMS version marker against what's actually on disk. */
    resolve_version_suffix(linux_path, path_size);

    /* Enhanced case-insensitive resolution */
    resolve_case(linux_path, path_size);

    return 0;
}

/*
 * Convert a Linux path to a VMS-style display string.
 *
 * /home/user/dir/file.txt -> SYS$DISK:[DIR]FILE.TXT
 */
int dcl_format_filespec(const char *linux_path, char *vms_spec, size_t spec_size)
{
    if (!linux_path || !vms_spec || spec_size == 0) return -1;

    /* Use the centralized Linux→VMS translation */
    int status = vmsfs_to_vms_spec(linux_path, vms_spec, spec_size);
    if ($VMS_STATUS_SUCCESS(status))
        return 0;

    /* Fallback: simple formatting */
    const char *last_slash = strrchr(linux_path, '/');
    char dir_part[512] = {0};
    char file_part[256] = {0};

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - linux_path);
        if (dlen < sizeof(dir_part)) {
            memcpy(dir_part, linux_path, dlen);
            dir_part[dlen] = '\0';
        }
        strncpy(file_part, last_slash + 1, sizeof(file_part) - 1);
    } else {
        strncpy(file_part, linux_path, sizeof(file_part) - 1);
    }

    /* Convert directory separators to dots, brackets around dir */
    char vms_dir[512] = {0};
    size_t vi = 0;
    if (dir_part[0]) {
        vms_dir[vi++] = '[';
        const char *p = dir_part;
        if (*p == '/') p++; /* skip leading / */
        while (*p && vi < sizeof(vms_dir) - 2) {
            if (*p == '/') {
                vms_dir[vi++] = '.';
            } else {
                vms_dir[vi++] = (char)toupper((unsigned char)*p);
            }
            p++;
        }
        vms_dir[vi++] = ']';
        vms_dir[vi] = '\0';
    }

    /* Uppercase the filename */
    char upper_file[256];
    size_t i;
    for (i = 0; i < sizeof(upper_file) - 1 && file_part[i]; i++) {
        upper_file[i] = (char)toupper((unsigned char)file_part[i]);
    }
    upper_file[i] = '\0';

    snprintf(vms_spec, spec_size, "SYS$DISK:%s%s", vms_dir, upper_file);
    return 0;
}

/*
 * Convert a Linux path to VMS directory format for display.
 * /home/baron/projects -> SYS$DISK:[HOME.BARON.PROJECTS]
 */
int dcl_format_directory(const char *linux_path, char *vms_dir, size_t dir_size)
{
    if (!linux_path || !vms_dir || dir_size == 0) return -1;

    char buf[1024];
    size_t bi = 0;

    /* Start with device prefix */
    const char *prefix = "SYS$DISK:[";
    size_t plen = strlen(prefix);
    memcpy(buf, prefix, plen);
    bi = plen;

    const char *p = linux_path;
    if (*p == '/') p++;

    int first = 1;
    while (*p && bi < sizeof(buf) - 2) {
        if (*p == '/') {
            if (!first) {
                buf[bi++] = '.';
            }
            first = 0;
        } else {
            buf[bi++] = (char)toupper((unsigned char)*p);
            first = 0;
        }
        p++;
    }

    /* Remove trailing dot if the path ended with / */
    if (bi > plen && buf[bi - 1] == '.') bi--;

    buf[bi++] = ']';
    buf[bi] = '\0';

    strncpy(vms_dir, buf, dir_size - 1);
    vms_dir[dir_size - 1] = '\0';
    return 0;
}

/*
 * dcl_directory_header_spec - VMS "DEV:[DIR]" spec for a DIRECTORY header.
 *
 * The DIRECTORY header must reflect the VMS directory the user is actually
 * looking at, derived from the VMS-side inputs (the explicit DIRECTORY
 * argument, else the process default) — NOT reverse-derived from the host
 * path. The host round-trip cannot recover the volume root [000000] from the
 * device-root mount (SYSDISK_MOUNT), so it rendered "SYS$DISK:[000000]" as
 * "SYS$DISK:[VMS]" and dropped the real device (vms-272).
 *
 * `spec` is the (de-ellipsized) DIRECTORY argument, or NULL/"" for a bare
 * DIRECTORY. `def` is ctx->default_dir in VMS format (e.g.
 * "SYS$DISK:[000000]"). The device and directory of the header are taken from
 * `spec` when it supplies them, otherwise inherited from `def`. An absent or
 * empty directory renders as the volume root [000000], matching VMS, where
 * [000000] is the Master File Directory.
 *
 * Returns 0 on success; on any parse failure the buffer is left with a safe
 * best-effort spec so the header is never blank.
 */
int dcl_directory_header_spec(const char *def, const char *spec,
                              char *vms_dir, size_t dir_size)
{
    if (!vms_dir || dir_size == 0) return -1;

    vmsfs_filespec_t dparts;
    memset(&dparts, 0, sizeof(dparts));
    if (def && def[0])
        vmsfs_parse_filespec(def, &dparts);

    vmsfs_filespec_t sparts;
    memset(&sparts, 0, sizeof(sparts));
    int have_spec = (spec && spec[0]);
    if (have_spec)
        vmsfs_parse_filespec(spec, &sparts);

    /* Device: from the argument if it named one, else the default's. */
    vmsfs_filespec_t out;
    memset(&out, 0, sizeof(out));
    if (have_spec && sparts.has_device && sparts.device[0]) {
        strncpy(out.device, sparts.device, sizeof(out.device) - 1);
        out.has_device = 1;
    } else if (dparts.has_device && dparts.device[0]) {
        strncpy(out.device, dparts.device, sizeof(out.device) - 1);
        out.has_device = 1;
    }

    /* Directory: from the argument if it named one, else the default's. */
    const char *dir = NULL;
    if (have_spec && sparts.has_directory)
        dir = sparts.directory;
    else if (dparts.has_directory)
        dir = dparts.directory;

    /* Volume root: absent or empty directory is the MFD, spelled [000000]. */
    strncpy(out.directory, (dir && dir[0]) ? dir : "000000",
            sizeof(out.directory) - 1);
    out.has_directory = 1;

    if (!$VMS_STATUS_SUCCESS(vmsfs_compose_filespec(&out, vms_dir, dir_size))) {
        /* Compose should not fail for a device+directory-only spec, but never
         * leave the header blank. */
        snprintf(vms_dir, dir_size, "%s%s[%s]",
                 out.has_device ? out.device : "",
                 out.has_device ? ":" : "",
                 out.directory);
    }
    return 0;
}

/*
 * Translate a logical name to its equivalence string.
 *
 * Queries the LNM manager first (process/job/group/system search list).
 * Falls back to hardcoded values for SYS$DISK and SYS$LOGIN which are
 * process-context-dependent and may not yet be in the LNM tables.
 */
int dcl_translate_logical(const char *name, char *result, size_t result_size)
{
    if (!name || !result || result_size == 0) return -1;

    /* Uppercase the name for comparison */
    char upper[256];
    size_t i;
    for (i = 0; i < sizeof(upper) - 1 && name[i]; i++) {
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    upper[i] = '\0';

    /*
     * Query the LNM manager via the standard LNM$FILE_DEV search list
     * (process -> job -> group -> system).
     */
    lnm_manager_t *mgr = lnm_get_manager();
    if (mgr) {
        uint16_t rlen = 0;
        uint32_t status = lnm_translate(mgr, LNM_FILE_DEV, upper,
                                        result, result_size, &rlen, NULL);
        if (status == SS$_NORMAL || status == SS$_SUPERSEDE)
            return 0;
    }

    /*
     * Fallback: handle process-context logicals that the LNM manager
     * may not have been initialized with yet.
     */
    if (strcmp(upper, "SYS$DISK") == 0) {
        strncpy(result, "SYS$SYSDEVICE", result_size - 1);
        result[result_size - 1] = '\0';
        return 0;
    }

    if (strcmp(upper, "SYS$LOGIN") == 0) {
        strncpy(result, "SYS$SYSDEVICE:[USERS]", result_size - 1);
        result[result_size - 1] = '\0';
        return 0;
    }

    return -1; /* Not found */
}

/*
 * Resolve a filespec, trying it both as VMS format and as a plain
 * Linux path.
 *
 * All VMS→Linux translation goes through vmsfs_to_linux_path().
 * This function handles:
 *   - Linux path passthrough (starts with / or ./ or ../)
 *   - VMS filespec with device/directory (contains : or [)
 *   - Plain filename (resolve relative to default directory)
 *
 * Returns 0 on success.
 */
int dcl_resolve_path(struct dcl_context *ctx, const char *spec,
                     char *linux_path, size_t path_size)
{
    if (!spec || !linux_path || path_size == 0) return -1;

    /* If it starts with / or ./ or ../, treat as Linux path directly */
    if (spec[0] == '/' || (spec[0] == '.' && (spec[1] == '/' || spec[1] == '.'))) {
        strncpy(linux_path, spec, path_size - 1);
        linux_path[path_size - 1] = '\0';
        return 0;
    }

    /*
     * Build a full VMS filespec by filling in defaults, then let
     * vmsfs_to_linux_path() do the translation.
     */
    char full_spec[1024];

    if (strchr(spec, '[') || strchr(spec, ':')) {
        /*
         * The spec names a device and/or a directory. VMS filespec defaulting:
         * a spec that supplies a directory (or file) but NOT a device inherits
         * the DEVICE from the current default directory — a device-less
         * "[SUB]" lists SUB on the current default's device, never the volume
         * root. Without this, a bracketed device-less spec falls through to
         * vmsfs's device-less path, which resolves the directory against the
         * system-disk mount root (SYSDISK_MOUNT, /vms) — so "DIRECTORY [SUB]"
         * after SET DEFAULT onto another device listed the wrong volume.
         * (VSI OpenVMS DCL Dictionary, DIRECTORY — the listing is relative to
         * the current default when the device is omitted; VSI OpenVMS User's
         * Manual, "File Specifications" — an omitted device field defaults to
         * the current default device. Clean-room, Rule 8: public docs only.)
         */
        vmsfs_filespec_t sparts;
        memset(&sparts, 0, sizeof(sparts));
        vmsfs_parse_filespec(spec, &sparts);
        if (!sparts.has_device) {
            vmsfs_filespec_t dparts;
            memset(&dparts, 0, sizeof(dparts));
            vmsfs_parse_filespec(ctx->default_dir, &dparts);
            if (dparts.has_device && dparts.device[0]) {
                snprintf(full_spec, sizeof(full_spec), "%s:%s",
                         dparts.device, spec);
            } else {
                strncpy(full_spec, spec, sizeof(full_spec) - 1);
                full_spec[sizeof(full_spec) - 1] = '\0';
            }
        } else {
            strncpy(full_spec, spec, sizeof(full_spec) - 1);
            full_spec[sizeof(full_spec) - 1] = '\0';
        }
    } else {
        /*
         * Plain filename — prefix with default directory.
         * ctx->default_dir is VMS format like "SYS$DISK:[USERS.SYSTEM]"
         */
        snprintf(full_spec, sizeof(full_spec), "%s%s",
                 ctx->default_dir, spec);
    }

    return dcl_resolve_filespec(ctx, full_spec, linux_path, path_size);
}


/* ============================================================================
 * vms-481 (epic vms-208): DCL file access through RMS / $QIO-to-ACP.
 *
 * These helpers let DCL file commands and F$ lexicals reach files the VMS way
 * -- RMS ($OPEN/$GET/$CREATE/$PUT/$SEARCH) + rms_file_attr -- which on the
 * product runtime route to the Files-11 ODS-2 ACP over /dev/vms, and on the
 * netbsd-vax cross keep RMS's own POSIX backend (vms-d5d). DCL no longer calls
 * opendir/stat/fopen on a vmsfs_to_linux_path("/vms/...") path for these
 * commands. FAIL-HONEST: no ACP-mounted SYS$DISK => the real RMS error, never a
 * silent POSIX fallback (Rule 9 / INV-6).
 * ========================================================================== */

/* Build the effective VMS filespec (device/dir defaulted) DCL hands to RMS.
 * Same defaulting as dcl_resolve_path, but stops at the VMS spec (no Linux
 * path). A Linux-passthrough spec ('/', './', '../') is returned unchanged. */
int dcl_rms_effective_spec(struct dcl_context *ctx, const char *spec,
                           char *out, size_t outsz)
{
    if (!ctx || !spec || !out || outsz == 0)
        return -1;

    if (spec[0] == '/' || (spec[0] == '.' && (spec[1] == '/' || spec[1] == '.'))) {
        strncpy(out, spec, outsz - 1);
        out[outsz - 1] = '\0';
        return 0;
    }

    if (strchr(spec, '[') || strchr(spec, ':')) {
        /* A spec that supplies a directory (or file) but NOT a device inherits
         * the device from the current default directory (VSI OpenVMS User's
         * Manual, "File Specifications"; clean-room, Rule 8). */
        vmsfs_filespec_t sparts;
        memset(&sparts, 0, sizeof(sparts));
        vmsfs_parse_filespec(spec, &sparts);
        if (!sparts.has_device) {
            vmsfs_filespec_t dparts;
            memset(&dparts, 0, sizeof(dparts));
            vmsfs_parse_filespec(ctx->default_dir, &dparts);
            if (dparts.has_device && dparts.device[0])
                snprintf(out, outsz, "%s:%s", dparts.device, spec);
            else { strncpy(out, spec, outsz - 1); out[outsz - 1] = '\0'; }
        } else { strncpy(out, spec, outsz - 1); out[outsz - 1] = '\0'; }
    } else {
        /* Plain filename -- prefix with the default directory (VMS format). */
        snprintf(out, outsz, "%s%s", ctx->default_dir, spec);
    }
    return 0;
}

/* ---- Sequential record READ (TYPE, COPY source) ---- */

struct dcl_rms_reader {
    struct FAB fab;
    struct RAB rab;
    char spec[1024];
    char rbuf[32768];
    int  connected;
};

struct dcl_rms_reader *dcl_rms_read_open(struct dcl_context *ctx, const char *spec,
                                         uint32_t *rms_status)
{
    struct dcl_rms_reader *r = calloc(1, sizeof(*r));
    uint32_t st;
    if (!r) { if (rms_status) *rms_status = RMS$_DME; return NULL; }

    if (dcl_rms_effective_spec(ctx, spec, r->spec, sizeof(r->spec)) != 0) {
        if (rms_status) *rms_status = RMS$_SYN;
        free(r); return NULL;
    }

    r->fab = cc$rms_fab;
    r->fab.fab$l_fna = r->spec;
    r->fab.fab$b_fns = (uint8_t)strlen(r->spec);
    r->fab.fab$b_org = FAB$C_SEQ;
    r->fab.fab$b_fac = FAB$M_GET;
    /* Frame records the way the file was written: read the record format from
     * the file's ODS-2 header (rms_file_attr -> FAT) and supply it on $OPEN.
     * (RMS-over-ACP does not yet persist/return RFM through $OPEN itself --
     * FAT-persist is a deferred vms-bc7 rung -- so the reader supplies it; when
     * the header carries no usable RFM we leave RMS's default.) */
    {
        struct rms_fileattr _fa;
        if (rms_file_attr(r->spec, &_fa) == RMS$_NORMAL &&
            _fa.rfm >= FAB$C_FIX && _fa.rfm <= FAB$C_STMCR) {
            r->fab.fab$b_rfm = _fa.rfm;
            if (_fa.mrs) r->fab.fab$w_mrs = _fa.mrs;
        }
    }
    st = sys$open(&r->fab, 0, 0);
    if (st != RMS$_NORMAL) { if (rms_status) *rms_status = st; free(r); return NULL; }

    r->rab = cc$rms_rab;
    r->rab.rab$l_fab = &r->fab;
    r->rab.rab$l_ubf = r->rbuf;
    r->rab.rab$w_usz = sizeof(r->rbuf);
    st = sys$connect(&r->rab, 0, 0);
    if (st != RMS$_NORMAL) {
        if (rms_status) *rms_status = st;
        sys$close(&r->fab, 0, 0); free(r); return NULL;
    }
    r->connected = 1;
    if (rms_status) *rms_status = RMS$_NORMAL;
    return r;
}

int dcl_rms_read_record(struct dcl_rms_reader *r, char *buf, size_t bufsz, int *eof)
{
    uint32_t st;
    if (eof) *eof = 0;
    if (!r || !buf || bufsz == 0) return -1;
    st = sys$get(&r->rab, 0, 0);
    if (st == RMS$_EOF) { if (eof) *eof = 1; return -1; }
    if (st != RMS$_NORMAL) return -1;
    {
        size_t n = r->rab.rab$w_rsz;
        if (n > bufsz - 1) n = bufsz - 1;
        memcpy(buf, r->rab.rab$l_ubf, n);
        buf[n] = '\0';
        return (int)n;
    }
}

void dcl_rms_read_close(struct dcl_rms_reader *r)
{
    if (!r) return;
    sys$close(&r->fab, 0, 0);
    free(r);
}

/* ---- Sequential record WRITE (CREATE, COPY dest) ---- */

struct dcl_rms_writer {
    struct FAB fab;
    struct RAB rab;
    char spec[1024];
    int  connected;
};

struct dcl_rms_writer *dcl_rms_write_create(struct dcl_context *ctx, const char *spec,
                                            uint8_t rfm, uint8_t rat, uint16_t mrs,
                                            uint32_t *rms_status)
{
    struct dcl_rms_writer *w = calloc(1, sizeof(*w));
    uint32_t st;
    if (!w) { if (rms_status) *rms_status = RMS$_DME; return NULL; }

    if (dcl_rms_effective_spec(ctx, spec, w->spec, sizeof(w->spec)) != 0) {
        if (rms_status) *rms_status = RMS$_SYN;
        free(w); return NULL;
    }

    w->fab = cc$rms_fab;
    w->fab.fab$l_fna = w->spec;
    w->fab.fab$b_fns = (uint8_t)strlen(w->spec);
    w->fab.fab$b_org = FAB$C_SEQ;
    w->fab.fab$b_rfm = rfm ? rfm : FAB$C_VAR;
    w->fab.fab$b_rat = rat;
    w->fab.fab$w_mrs = mrs;
    w->fab.fab$b_fac = FAB$M_PUT;
    st = sys$create(&w->fab, 0, 0);
    if (st != RMS$_NORMAL) {
        fprintf(stderr, "DBG4AC sys$create '%s' rfm=%u -> st=%u (0x%08x)\n",
                w->spec, (unsigned)w->fab.fab$b_rfm, (unsigned)st, (unsigned)st);
        if (rms_status) *rms_status = st;
        free(w);
        return NULL;
    }

    w->rab = cc$rms_rab;
    w->rab.rab$l_fab = &w->fab;
    st = sys$connect(&w->rab, 0, 0);
    if (st != RMS$_NORMAL) {
        if (rms_status) *rms_status = st;
        sys$close(&w->fab, 0, 0); free(w); return NULL;
    }
    w->connected = 1;
    if (rms_status) *rms_status = RMS$_NORMAL;
    return w;
}

int dcl_rms_write_record(struct dcl_rms_writer *w, const char *buf, size_t len)
{
    uint32_t st;
    if (!w) return -1;
    w->rab.rab$l_rbf = (char *)buf;
    w->rab.rab$w_rsz = (uint16_t)len;
    st = sys$put(&w->rab, 0, 0);
    return (st == RMS$_NORMAL) ? 0 : -1;
}

uint32_t dcl_rms_write_close(struct dcl_rms_writer *w)
{
    uint32_t st;
    if (!w) return RMS$_FAB;
    st = sys$close(&w->fab, 0, 0);
    free(w);
    return st;
}

/* ---- Wildcard directory iteration (DIRECTORY, F$SEARCH) ---- */

struct dcl_rms_dir {
    struct FAB fab;
    struct NAM nam;
    char pattern[1024];
    char esa[512];
    char rsa[512];
};

struct dcl_rms_dir *dcl_rms_dir_open(struct dcl_context *ctx, const char *pattern)
{
    struct dcl_rms_dir *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    if (dcl_rms_effective_spec(ctx, pattern, d->pattern, sizeof(d->pattern)) != 0) {
        free(d); return NULL;
    }

    d->fab = cc$rms_fab;
    d->fab.fab$l_fna = d->pattern;
    d->fab.fab$b_fns = (uint8_t)strlen(d->pattern);
    d->nam = cc$rms_nam;
    d->nam.nam$l_esa = d->esa;
    d->nam.nam$b_ess = (uint8_t)(sizeof(d->esa) > 255 ? 255 : sizeof(d->esa));
    d->nam.nam$l_rsa = d->rsa;
    d->nam.nam$b_rss = (uint8_t)(sizeof(d->rsa) > 255 ? 255 : sizeof(d->rsa));
    d->fab.fab$l_nam = &d->nam;

    /* $PARSE expands the pattern into the NAM (device logical translated, the
     * expanded string that $SEARCH then iterates). A syntax error => no dir. */
    if (sys$parse(&d->fab, 0, 0) != RMS$_NORMAL) { free(d); return NULL; }
    return d;
}

int dcl_rms_dir_next(struct dcl_rms_dir *d, char *spec, size_t specsz,
                     uint16_t *fid_num, uint16_t *fid_seq, uint8_t *fid_rvn)
{
    uint32_t st;
    if (!d) return 0;
    st = sys$search(&d->fab, 0, 0);
    if (st != RMS$_NORMAL) return 0;    /* RMS$_NMF or fail-honest end */
    if (spec && specsz) {
        size_t n = d->nam.nam$b_rsl;
        if (n > specsz - 1) n = specsz - 1;
        memcpy(spec, d->nam.nam$l_rsa, n);
        spec[n] = '\0';
    }
    if (fid_num || fid_seq || fid_rvn) {
        uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
        rms_search_fid(&d->nam, &num, &seq, &rvn, &nmx);
        if (fid_num) *fid_num = num;
        if (fid_seq) *fid_seq = seq;
        if (fid_rvn) *fid_rvn = rvn;
    }
    return 1;
}

void dcl_rms_dir_close(struct dcl_rms_dir *d)
{
    if (!d) return;
    rms_search_end(&d->nam);      /* release the channel if we stopped early */
    free(d);
}

/* ---- File erase (DELETE) ---- */

uint32_t dcl_rms_erase(struct dcl_context *ctx, const char *spec)
{
    char vspec[1024];
    struct FAB fab;
    if (!spec || !spec[0]) return RMS$_FAB;
    if (dcl_rms_effective_spec(ctx, spec, vspec, sizeof(vspec)) != 0)
        return RMS$_SYN;

    fab = cc$rms_fab;
    fab.fab$l_fna = vspec;
    fab.fab$b_fns = (uint8_t)strlen(vspec);
    /* $ERASE routes to rms_impl_erase: $ASSIGN + IO$_DELETE over the ACP when
     * the executive is present, POSIX unlink on the executive-absent defer. The
     * explicit version in `vspec` selects exactly that version (a ";*" tail
     * would delete every version in one op). */
    return sys$erase(&fab, 0, 0);
}

/* ---- One file's genuine ODS-2 attributes (DIRECTORY /FULL, F$FILE_ATTRIBUTES) ---- */

uint32_t dcl_rms_attr(struct dcl_context *ctx, const char *spec,
                      struct rms_fileattr *out)
{
    char vspec[1024];
    if (!out) return RMS$_FAB;
    if (dcl_rms_effective_spec(ctx, spec, vspec, sizeof(vspec)) != 0)
        return RMS$_SYN;
    return rms_file_attr(vspec, out);
}

/* ---- Stage an image's genuine bytes off the ODS-2 volume over the ACP (vms-104) ---- */
uint32_t dcl_rms_stage(struct dcl_context *ctx, const char *spec,
                       const char *dest)
{
    char vspec[1024];
    if (!spec || !dest)
        return RMS$_FAB;
    if (dcl_rms_effective_spec(ctx, spec, vspec, sizeof(vspec)) != 0)
        return RMS$_SYN;
    return rms_stage_over_acp(vspec, dest);
}
