/*
 * vmsfs_case.c - Case-Insensitive Filename Resolution
 *
 * VMS filenames are case-insensitive and case-preserving (stored uppercase).
 * On Linux (case-sensitive filesystem), we need to perform case-insensitive
 * lookups by scanning directories for matching entries.
 *
 * This module provides:
 *   - Single-component case-insensitive lookup
 *   - Full path case-insensitive resolution
 *   - ODS-2 filename validation
 *
 * OVMX Project - VMS Filesystem Semantics Layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include "vmsfs/filespec.h"
#include "ssdef.h"
#include "rmsdef.h"

/*
 * vmsfs_find_case_insensitive - Case-insensitive file lookup in a directory.
 *
 * Scans the directory at dir_path, comparing each entry case-insensitively
 * with the given name. If a match is found, the actual on-disk filename
 * is copied to result.
 *
 * Parameters:
 *   dir_path    - Path to the directory to scan
 *   name        - Name to search for (case-insensitive)
 *   result      - Output buffer for the actual filename found
 *   result_size - Size of the result buffer
 *
 * Returns 0 on success (match found), -1 if not found or error.
 */
int vmsfs_find_case_insensitive(const char *dir_path, const char *name,
                                 char *result, size_t result_size)
{
    if (!dir_path || !name || !result || result_size == 0) {
        return -1;
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        return -1;
    }

    struct dirent *entry;
    int found = 0;

    while ((entry = readdir(d)) != NULL) {
        /* Skip . and .. entries */
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        if (strcasecmp(entry->d_name, name) == 0) {
            size_t len = strlen(entry->d_name);
            if (len >= result_size) {
                len = result_size - 1;
            }
            memcpy(result, entry->d_name, len);
            result[len] = '\0';
            found = 1;
            break;
        }
    }

    closedir(d);
    return found ? 0 : -1;
}

/*
 * vmsfs_resolve_path_case - Resolve each component of a path case-insensitively.
 *
 * Splits the path by '/', then for each directory component:
 *   1. Opens the parent directory
 *   2. Searches for a case-insensitive match
 *   3. Uses the actual on-disk name if found, or the original if not
 *   4. Builds the resolved path incrementally
 *
 * This handles the common case where a VMS program references a file
 * in lowercase (or any case) and the actual file on disk has different
 * casing.
 *
 * Parameters:
 *   full_path     - The path to resolve (must be absolute, starting with /)
 *   resolved      - Output buffer for the resolved path
 *   resolved_size - Size of the resolved buffer
 *
 * Returns 0 on success, -1 on error.
 */
int vmsfs_resolve_path_case(const char *full_path, char *resolved, size_t resolved_size)
{
    if (!full_path || !resolved || resolved_size == 0) {
        return -1;
    }

    /* Handle relative paths - just copy them through */
    if (full_path[0] != '/') {
        strncpy(resolved, full_path, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return 0;
    }

    /* Start building the resolved path from root */
    char current_dir[VMSFS_MAX_PATH];
    current_dir[0] = '/';
    current_dir[1] = '\0';

    resolved[0] = '\0';

    const char *p = full_path + 1;  /* Skip leading / */

    while (*p) {
        /* Extract the next path component */
        char component[256];
        const char *slash = strchr(p, '/');
        size_t clen;

        if (slash) {
            clen = (size_t)(slash - p);
        } else {
            clen = strlen(p);
        }

        if (clen == 0) {
            /* Double slash - skip */
            p++;
            continue;
        }

        if (clen >= sizeof(component)) {
            clen = sizeof(component) - 1;
        }
        memcpy(component, p, clen);
        component[clen] = '\0';

        /*
         * Look up this component case-insensitively in current_dir.
         * If we find a match, use the actual name. Otherwise, keep
         * the original component as-is (it might not exist yet).
         */
        char found_name[256];
        if (vmsfs_find_case_insensitive(current_dir, component,
                                         found_name, sizeof(found_name)) == 0) {
            /* Found a case-insensitive match */
            size_t cur_len = strlen(current_dir);
            if (cur_len == 1 && current_dir[0] == '/') {
                /* Root directory - don't add extra slash */
                snprintf(current_dir, sizeof(current_dir), "/%s", found_name);
            } else {
                if (cur_len + 1 + strlen(found_name) < sizeof(current_dir)) {
                    current_dir[cur_len] = '/';
                    strcpy(current_dir + cur_len + 1, found_name);
                }
            }
        } else {
            /* No match found - use original component */
            size_t cur_len = strlen(current_dir);
            if (cur_len == 1 && current_dir[0] == '/') {
                snprintf(current_dir, sizeof(current_dir), "/%s", component);
            } else {
                if (cur_len + 1 + clen < sizeof(current_dir)) {
                    current_dir[cur_len] = '/';
                    memcpy(current_dir + cur_len + 1, component, clen);
                    current_dir[cur_len + 1 + clen] = '\0';
                }
            }
        }

        /* Advance past this component */
        p += clen;
        if (*p == '/') p++;
    }

    strncpy(resolved, current_dir, resolved_size - 1);
    resolved[resolved_size - 1] = '\0';
    return 0;
}

/*
 * vmsfs_is_valid_ods2_name - Validate a filename against ODS-2 rules.
 *
 * ODS-2 naming rules:
 *   - Name: 1-39 characters
 *   - Type: 0-39 characters (after the dot)
 *   - Valid characters: A-Z, 0-9, underscore, dollar sign, hyphen
 *   - Only one dot allowed (separating name from type)
 *   - Version ;N is not part of the name/type
 *
 * Returns 1 if valid, 0 if invalid.
 */
int vmsfs_is_valid_ods2_name(const char *name)
{
    if (!name || !*name) return 0;

    const char *p = name;
    int name_len = 0;
    int type_len = 0;
    int in_type = 0;
    int dot_count = 0;

    while (*p && *p != ';') {
        if (*p == '.') {
            if (dot_count > 0) return 0;  /* Only one dot allowed in ODS-2 */
            dot_count++;
            in_type = 1;
            name_len = 0;  /* Reset for type length tracking */
        } else if (isalnum((unsigned char)*p) ||
                   *p == '_' || *p == '$' || *p == '-') {
            if (in_type) {
                type_len++;
                if (type_len > VMSFS_MAX_TYPE) return 0;
            } else {
                name_len++;
                if (name_len > VMSFS_MAX_NAME) return 0;
            }
        } else {
            return 0;  /* Invalid character */
        }
        p++;
    }

    /* Name part must have at least one character */
    if (!in_type && name_len == 0) return 0;

    return 1;
}
