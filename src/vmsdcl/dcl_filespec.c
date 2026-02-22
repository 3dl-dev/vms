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
#include "ovmx_layout.h"

/* Forward declaration of logical name translation */
extern int dcl_translate_logical(const char *name, char *result, size_t result_size);

/* Forward declaration of vmsfs case-insensitive lookup */
extern int vmsfs_find_case_insensitive(const char *dir_path, const char *name,
                                        char *result, size_t result_size);

/*
 * Strip a VMS version suffix (;N) from a Linux path in-place.
 * Only strips if the suffix is a valid version number.
 */
static void strip_version_suffix(char *path)
{
    char *semi = strrchr(path, ';');
    if (!semi) return;

    /* Only strip if everything after ; is digits */
    const char *p = semi + 1;
    if (*p == '\0') return;  /* bare ; with nothing after */
    while (*p) {
        if (!isdigit((unsigned char)*p)) return;
        p++;
    }
    *semi = '\0';
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

    /* Strip version suffix — Linux filesystem doesn't use ;N */
    strip_version_suffix(linux_path);

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
