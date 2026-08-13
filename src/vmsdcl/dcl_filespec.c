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
        /* Already has device or directory — pass through to vmsfs */
        strncpy(full_spec, spec, sizeof(full_spec) - 1);
        full_spec[sizeof(full_spec) - 1] = '\0';
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
