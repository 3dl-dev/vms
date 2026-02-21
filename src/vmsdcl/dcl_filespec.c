/*
 * dcl_filespec.c - VMS filespec handling for DCL
 *
 * Resolves VMS-style filespecs to Linux paths and vice versa.
 * Handles device/logical translation, directory specs ([DIR.SUB]),
 * relative specs ([.SUB], [-]), and wildcard expansion.
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

/* Forward declaration of logical name translation */
extern int dcl_translate_logical(const char *name, char *result, size_t result_size);

/* Forward declaration of vmsfs case-insensitive lookup */
extern int vmsfs_find_case_insensitive(const char *dir_path, const char *name,
                                        char *result, size_t result_size);

/*
 * Convert a VMS directory spec to a Linux path component.
 * [DIR.SUB]     -> /dir/sub/
 * [.SUB]        -> sub/         (relative)
 * [-]           -> ../          (parent)
 * [-.DIR]       -> ../dir/
 * [DIR]         -> /dir/
 */
static int translate_vms_dir(const char *vms_dir, char *linux_dir, size_t dir_size)
{
    if (!vms_dir || !linux_dir || dir_size == 0) return -1;

    linux_dir[0] = '\0';
    size_t out = 0;

    const char *p = vms_dir;

    /* Skip leading [ or < */
    if (*p == '[' || *p == '<') p++;

    /* Check for relative spec */
    if (*p == '.') {
        /* [.SUB] - relative to current directory */
        p++; /* skip the dot */
    } else if (*p == '-') {
        /* [-] or [-.DIR] - parent directory */
        while (*p == '-') {
            if (out + 3 < dir_size) {
                linux_dir[out++] = '.';
                linux_dir[out++] = '.';
                linux_dir[out++] = '/';
            }
            p++;
            if (*p == '.') p++;
        }
    } else if (*p != ']' && *p != '>' && *p != '\0') {
        /* Absolute directory - start with / */
        if (out < dir_size - 1) linux_dir[out++] = '/';
    }

    /* Convert directory components (dots become slashes) */
    while (*p && *p != ']' && *p != '>') {
        if (*p == '.') {
            if (out < dir_size - 1) linux_dir[out++] = '/';
        } else {
            if (out < dir_size - 1)
                linux_dir[out++] = (char)tolower((unsigned char)*p);
        }
        p++;
    }

    /* Ensure trailing slash */
    if (out > 0 && linux_dir[out - 1] != '/') {
        if (out < dir_size - 1) linux_dir[out++] = '/';
    }

    linux_dir[out] = '\0';
    return 0;
}

/*
 * Resolve a VMS filespec to a Linux path.
 *
 * Handles:
 *   device:[dir]file.ext;ver  - full filespec
 *   [dir]file.ext             - no device (use default)
 *   file.ext                  - just a filename
 *   [.sub]file.ext            - relative directory
 *   logical:file.ext          - logical name as device
 *
 * Returns 0 on success.
 */
int dcl_resolve_filespec(struct dcl_context *ctx, const char *spec,
                         char *linux_path, size_t path_size)
{
    if (!spec || !linux_path || path_size == 0) return -1;

    linux_path[0] = '\0';
    char device[128] = {0};
    char dir_spec[512] = {0};
    char name_part[512] = {0};

    const char *p = spec;

    /* Check for device: or logical: prefix */
    const char *colon = strchr(p, ':');
    const char *bracket = strchr(p, '[');

    if (colon && (!bracket || colon < bracket)) {
        /* Has a device/logical prefix */
        size_t dlen = (size_t)(colon - p);
        if (dlen < sizeof(device)) {
            memcpy(device, p, dlen);
            device[dlen] = '\0';
        }
        p = colon + 1;
    }

    /* Check for [directory] spec */
    if (*p == '[' || *p == '<') {
        const char *end = strchr(p, (*p == '[') ? ']' : '>');
        if (end) {
            size_t dirlen = (size_t)(end - p + 1);
            if (dirlen < sizeof(dir_spec)) {
                memcpy(dir_spec, p, dirlen);
                dir_spec[dirlen] = '\0';
            }
            p = end + 1;
        }
    }

    /* Rest is the filename */
    strncpy(name_part, p, sizeof(name_part) - 1);
    name_part[sizeof(name_part) - 1] = '\0';

    /* Remove version number (;N) from filename for Linux */
    char *semi = strrchr(name_part, ';');
    if (semi) *semi = '\0';

    /* Build the Linux path */
    char base_dir[1024] = {0};

    /* Resolve device/logical */
    if (device[0]) {
        char trans[512];
        if (dcl_translate_logical(device, trans, sizeof(trans)) == 0) {
            strncpy(base_dir, trans, sizeof(base_dir) - 1);
        } else {
            /* Try as a direct path mapping */
            snprintf(base_dir, sizeof(base_dir), "/vms/%s", device);
        }
    }

    /* Resolve directory spec */
    if (dir_spec[0]) {
        char linux_dir[512];
        translate_vms_dir(dir_spec, linux_dir, sizeof(linux_dir));

        if (linux_dir[0] == '/') {
            /* Absolute directory */
            if (base_dir[0]) {
                /* Append to device root */
                size_t blen = strlen(base_dir);
                if (blen > 0 && base_dir[blen - 1] == '/') {
                    snprintf(base_dir + blen, sizeof(base_dir) - blen,
                             "%s", linux_dir + 1);
                } else {
                    strncat(base_dir, linux_dir, sizeof(base_dir) - blen - 1);
                }
            } else {
                /* Use default device + this directory */
                snprintf(base_dir, sizeof(base_dir), "%s%s",
                         ctx->default_linux, linux_dir);
            }
        } else {
            /* Relative directory */
            if (!base_dir[0]) {
                strncpy(base_dir, ctx->default_linux, sizeof(base_dir) - 1);
            }
            size_t blen = strlen(base_dir);
            if (blen > 0 && base_dir[blen - 1] != '/') {
                if (blen < sizeof(base_dir) - 1) base_dir[blen++] = '/';
            }
            strncat(base_dir, linux_dir, sizeof(base_dir) - blen - 1);
        }
    } else if (!base_dir[0]) {
        /* No device and no directory - use current default */
        strncpy(base_dir, ctx->default_linux, sizeof(base_dir) - 1);
    }

    /* Ensure base_dir has trailing slash */
    size_t blen = strlen(base_dir);
    if (blen > 0 && base_dir[blen - 1] != '/') {
        if (blen < sizeof(base_dir) - 1) {
            base_dir[blen] = '/';
            base_dir[blen + 1] = '\0';
        }
    }

    /* Combine base_dir + filename */
    if (name_part[0]) {
        /* VMS is case-insensitive; try uppercase first, then case-insensitive scan */
        char upper_name[512];
        size_t i;
        for (i = 0; i < sizeof(upper_name) - 1 && name_part[i]; i++) {
            upper_name[i] = (char)toupper((unsigned char)name_part[i]);
        }
        upper_name[i] = '\0';

        /* Try uppercase (VMS canonical) */
        char try_path[1024];
        struct stat fst;
        snprintf(try_path, sizeof(try_path), "%s%s", base_dir, upper_name);
        if (stat(try_path, &fst) == 0) {
            strncpy(linux_path, try_path, path_size - 1);
            linux_path[path_size - 1] = '\0';
        } else {
            /* Try original case */
            snprintf(try_path, sizeof(try_path), "%s%s", base_dir, name_part);
            if (stat(try_path, &fst) == 0) {
                strncpy(linux_path, try_path, path_size - 1);
                linux_path[path_size - 1] = '\0';
            } else {
                /* Case-insensitive scan of directory */
                char found_name[256];
                /* Strip trailing slash from base_dir for opendir */
                char scan_dir[1024];
                strncpy(scan_dir, base_dir, sizeof(scan_dir) - 1);
                scan_dir[sizeof(scan_dir) - 1] = '\0';
                size_t sdlen = strlen(scan_dir);
                if (sdlen > 1 && scan_dir[sdlen - 1] == '/')
                    scan_dir[sdlen - 1] = '\0';

                if (vmsfs_find_case_insensitive(scan_dir, name_part,
                                                 found_name, sizeof(found_name)) == 0) {
                    snprintf(linux_path, path_size, "%s%s", base_dir, found_name);
                } else {
                    /* Default to uppercase for new files */
                    snprintf(linux_path, path_size, "%s%s", base_dir, upper_name);
                }
            }
        }
    } else {
        strncpy(linux_path, base_dir, path_size - 1);
        linux_path[path_size - 1] = '\0';
    }

    return 0;
}

/*
 * Convert a Linux path to a VMS-style display string.
 *
 * /home/user/dir/file.txt -> DISK$USER:[DIR]FILE.TXT
 */
int dcl_format_filespec(const char *linux_path, char *vms_spec, size_t spec_size)
{
    if (!linux_path || !vms_spec || spec_size == 0) return -1;

    /* For simple display, just show the path in a VMS-like format */
    /* Extract directory and filename parts */
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
        struct dcl_context *ctx = dcl_get_context();
        strncpy(result, ctx->default_linux, result_size - 1);
        result[result_size - 1] = '\0';
        return 0;
    }

    if (strcmp(upper, "SYS$LOGIN") == 0) {
        const char *home = getenv("HOME");
        if (home) {
            strncpy(result, home, result_size - 1);
            result[result_size - 1] = '\0';
            return 0;
        }
    }

    return -1; /* Not found */
}

/*
 * Resolve a filespec, trying it both as VMS format and as a plain
 * Linux path. Returns 0 on success.
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

    /* If it contains [ or :, parse as VMS filespec */
    if (strchr(spec, '[') || strchr(spec, ':')) {
        return dcl_resolve_filespec(ctx, spec, linux_path, path_size);
    }

    /* Plain filename - resolve relative to default directory */
    /* Strip VMS version number ;N if present */
    char name_buf[512];
    strncpy(name_buf, spec, sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    char *semi = strrchr(name_buf, ';');
    if (semi) *semi = '\0';

    /* VMS is case-insensitive; canonical form is UPPERCASE.
     * Try: uppercase (VMS canonical), original case, then case-insensitive scan. */
    char upper[512];
    size_t i;
    for (i = 0; i < sizeof(upper) - 1 && name_buf[i]; i++) {
        upper[i] = (char)toupper((unsigned char)name_buf[i]);
    }
    upper[i] = '\0';

    char try_path[1024];
    struct stat st;

    /* Try uppercase first (VMS stores filenames in uppercase) */
    snprintf(try_path, sizeof(try_path), "%s/%s", ctx->default_linux, upper);
    if (stat(try_path, &st) == 0) {
        strncpy(linux_path, try_path, path_size - 1);
        linux_path[path_size - 1] = '\0';
        return 0;
    }

    /* Try original case */
    snprintf(try_path, sizeof(try_path), "%s/%s", ctx->default_linux, name_buf);
    if (stat(try_path, &st) == 0) {
        strncpy(linux_path, try_path, path_size - 1);
        linux_path[path_size - 1] = '\0';
        return 0;
    }

    /* Case-insensitive directory scan */
    char found_name[256];
    if (vmsfs_find_case_insensitive(ctx->default_linux, name_buf,
                                     found_name, sizeof(found_name)) == 0) {
        snprintf(linux_path, path_size, "%s/%s", ctx->default_linux, found_name);
        return 0;
    }

    /* Not found - return uppercase (VMS canonical for new files) */
    snprintf(linux_path, path_size, "%s/%s", ctx->default_linux, upper);
    return 0;
}
