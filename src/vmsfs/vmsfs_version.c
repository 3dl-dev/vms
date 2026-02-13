/*
 * vmsfs_version.c - VMS File Version Management
 *
 * Implements file versioning on top of the Linux filesystem.
 * Files are stored as "name.ext;N" where N is the version number.
 *
 * VMS versioning semantics:
 *   - Every file creation automatically increments the version
 *   - ;0 or no version means the highest existing version
 *   - ;N means a specific version
 *   - PURGE removes old versions, keeping the highest N
 *   - Maximum version number is 32767
 *
 * OVMX Project - VMS Filesystem Semantics Layer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>

#include "vmsfs/version.h"
#include "vmsfs/filespec.h"
#include "ssdef.h"
#include "rmsdef.h"

/* Maximum VMS version number */
#define VMS_MAX_VERSION 32767

/* Maximum number of versions we'll track for a single file */
#define MAX_TRACKED_VERSIONS 1024

/*
 * Build the combined "basename.ext" string used for matching directory entries.
 * If ext is NULL or empty, just the basename is used.
 */
static int build_base_pattern(const char *basename, const char *ext,
                               char *pattern, size_t pattern_size)
{
    if (!basename || !pattern || pattern_size == 0) return -1;

    if (ext && ext[0]) {
        snprintf(pattern, pattern_size, "%s.%s", basename, ext);
    } else {
        strncpy(pattern, basename, pattern_size - 1);
        pattern[pattern_size - 1] = '\0';
    }
    return 0;
}

/*
 * Check if a directory entry matches our base pattern followed by ;N.
 * Comparison is case-insensitive.
 *
 * Returns the version number if matched, or 0 if no match.
 */
static int match_versioned_entry(const char *entry_name, const char *base_pattern,
                                  size_t base_len)
{
    if (strncasecmp(entry_name, base_pattern, base_len) != 0) {
        return 0;
    }

    if (entry_name[base_len] != ';') {
        return 0;
    }

    const char *ver_str = entry_name + base_len + 1;
    if (!*ver_str) return 0;

    /* Verify all digits */
    const char *p = ver_str;
    while (*p) {
        if (!isdigit((unsigned char)*p)) return 0;
        p++;
    }

    int ver = atoi(ver_str);
    if (ver < 1 || ver > VMS_MAX_VERSION) return 0;

    return ver;
}

/*
 * vmsfs_get_highest_version - Find the highest version number for a file.
 *
 * Scans the directory for all files matching "basename.ext;N" and returns
 * the highest N found.
 *
 * If no versioned files exist, checks for an unversioned "basename.ext"
 * and treats it as version 1.
 *
 * Parameters:
 *   linux_dir  - Directory to scan
 *   basename   - File name without extension (e.g., "LOGIN")
 *   ext        - File extension without dot (e.g., "COM"), may be NULL
 *
 * Returns: highest version number, or 0 if no files found.
 */
int vmsfs_get_highest_version(const char *linux_dir, const char *basename, const char *ext)
{
    if (!linux_dir || !basename) return 0;

    char base_pattern[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 2];
    if (build_base_pattern(basename, ext, base_pattern, sizeof(base_pattern)) < 0) {
        return 0;
    }
    size_t base_len = strlen(base_pattern);

    DIR *d = opendir(linux_dir);
    if (!d) return 0;

    int highest = 0;
    int found_unversioned = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        int ver = match_versioned_entry(entry->d_name, base_pattern, base_len);
        if (ver > highest) {
            highest = ver;
        }

        /* Also check for exact unversioned match */
        if (strcasecmp(entry->d_name, base_pattern) == 0) {
            found_unversioned = 1;
        }
    }

    closedir(d);

    /*
     * If no versioned files found but an unversioned file exists,
     * treat it as version 1.
     */
    if (highest == 0 && found_unversioned) {
        highest = 1;
    }

    return highest;
}

/*
 * vmsfs_version_filename - Construct "basename.ext;N" for a given version.
 *
 * Parameters:
 *   basename    - File name without extension
 *   ext         - File extension without dot (may be NULL)
 *   version     - Version number (must be > 0)
 *   result      - Output buffer for the constructed filename
 *   result_size - Size of the output buffer
 *
 * Returns SS$_NORMAL on success.
 */
int vmsfs_version_filename(const char *basename, const char *ext, int version,
                           char *result, size_t result_size)
{
    if (!basename || !result || result_size == 0) {
        return SS$_BADPARAM;
    }
    if (version < 1 || version > VMS_MAX_VERSION) {
        return SS$_BADPARAM;
    }

    int n;
    if (ext && ext[0]) {
        n = snprintf(result, result_size, "%s.%s;%d", basename, ext, version);
    } else {
        n = snprintf(result, result_size, "%s;%d", basename, version);
    }

    if (n < 0 || (size_t)n >= result_size) {
        return SS$_RESULTOVF;
    }

    return SS$_NORMAL;
}

/*
 * vmsfs_create_new_version - Create a new version of a file.
 *
 * Determines the next version number (highest + 1), constructs the full
 * path, and creates the file. The file is created with O_CREAT|O_EXCL
 * to avoid race conditions.
 *
 * Parameters:
 *   linux_dir   - Directory where the file will be created
 *   basename    - File name without extension
 *   ext         - File extension without dot (may be NULL)
 *   result_path - Output: full path of the created file
 *   path_size   - Size of result_path buffer
 *   new_version - Output: the version number assigned
 *
 * Returns SS$_NORMAL on success, RMS$_CRE on creation failure.
 */
int vmsfs_create_new_version(const char *linux_dir, const char *basename, const char *ext,
                              char *result_path, size_t path_size, int *new_version)
{
    if (!linux_dir || !basename || !result_path || path_size == 0) {
        return SS$_BADPARAM;
    }

    int highest = vmsfs_get_highest_version(linux_dir, basename, ext);
    int next_ver = highest + 1;

    if (next_ver > VMS_MAX_VERSION) {
        return SS$_BADPARAM;  /* Version number overflow */
    }

    /* Build the versioned filename */
    char versioned_name[VMSFS_MAX_FILESPEC];
    int status = vmsfs_version_filename(basename, ext, next_ver,
                                         versioned_name, sizeof(versioned_name));
    if (!$VMS_STATUS_SUCCESS(status)) {
        return status;
    }

    /* Construct the full path */
    int n = snprintf(result_path, path_size, "%s/%s", linux_dir, versioned_name);
    if (n < 0 || (size_t)n >= path_size) {
        return SS$_RESULTOVF;
    }

    /* Create the file with O_CREAT|O_EXCL to prevent race conditions */
    int fd = open(result_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            /*
             * Race condition: another process created this version.
             * Try the next version number.
             */
            next_ver++;
            if (next_ver > VMS_MAX_VERSION) return RMS$_CRE;

            status = vmsfs_version_filename(basename, ext, next_ver,
                                             versioned_name, sizeof(versioned_name));
            if (!$VMS_STATUS_SUCCESS(status)) return status;

            n = snprintf(result_path, path_size, "%s/%s", linux_dir, versioned_name);
            if (n < 0 || (size_t)n >= path_size) return SS$_RESULTOVF;

            fd = open(result_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (fd < 0) return RMS$_CRE;
        } else {
            return RMS$_CRE;
        }
    }

    close(fd);

    if (new_version) {
        *new_version = next_ver;
    }

    return SS$_NORMAL;
}

/*
 * Comparison function for sorting version numbers in ascending order.
 */
static int compare_versions_asc(const void *a, const void *b)
{
    return (*(const int *)a) - (*(const int *)b);
}

/*
 * vmsfs_purge_versions - Purge old versions, keeping the highest N.
 *
 * Lists all versions of a file, sorts them, and deletes all but the
 * highest keep_count versions.
 *
 * Parameters:
 *   linux_dir  - Directory containing the files
 *   basename   - File name without extension
 *   ext        - File extension without dot (may be NULL)
 *   keep_count - Number of highest versions to keep (minimum 1)
 *
 * Returns the count of files deleted, or a negative VMS status on error.
 */
int vmsfs_purge_versions(const char *linux_dir, const char *basename, const char *ext,
                          int keep_count)
{
    if (!linux_dir || !basename) {
        return SS$_BADPARAM;
    }

    if (keep_count < 1) keep_count = 1;

    /* List all existing versions */
    int versions[MAX_TRACKED_VERSIONS];
    int count = 0;
    int status = vmsfs_list_versions(linux_dir, basename, ext,
                                      versions, MAX_TRACKED_VERSIONS, &count);
    if (!$VMS_STATUS_SUCCESS(status) || count <= keep_count) {
        return 0;  /* Nothing to purge */
    }

    /* Sort versions in ascending order */
    qsort(versions, (size_t)count, sizeof(int), compare_versions_asc);

    /* Delete all but the highest keep_count versions */
    int deleted = 0;
    int delete_count = count - keep_count;

    for (int i = 0; i < delete_count; i++) {
        char versioned_name[VMSFS_MAX_FILESPEC];
        int st = vmsfs_version_filename(basename, ext, versions[i],
                                         versioned_name, sizeof(versioned_name));
        if (!$VMS_STATUS_SUCCESS(st)) continue;

        char fullpath[VMSFS_MAX_PATH];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", linux_dir, versioned_name);

        if (unlink(fullpath) == 0) {
            deleted++;
        }
    }

    return deleted;
}

/*
 * vmsfs_list_versions - List all existing version numbers for a file.
 *
 * Scans the directory and collects all version numbers for files
 * matching the given basename and extension.
 *
 * Parameters:
 *   linux_dir    - Directory to scan
 *   basename     - File name without extension
 *   ext          - File extension without dot (may be NULL)
 *   versions     - Output array for version numbers
 *   max_versions - Maximum entries in the versions array
 *   count        - Output: number of versions found
 *
 * Returns SS$_NORMAL on success.
 */
int vmsfs_list_versions(const char *linux_dir, const char *basename, const char *ext,
                         int *versions, int max_versions, int *count)
{
    if (!linux_dir || !basename || !versions || !count || max_versions <= 0) {
        return SS$_BADPARAM;
    }

    *count = 0;

    char base_pattern[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 2];
    if (build_base_pattern(basename, ext, base_pattern, sizeof(base_pattern)) < 0) {
        return SS$_BADPARAM;
    }
    size_t base_len = strlen(base_pattern);

    DIR *d = opendir(linux_dir);
    if (!d) {
        return RMS$_DNF;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && *count < max_versions) {
        int ver = match_versioned_entry(entry->d_name, base_pattern, base_len);
        if (ver > 0) {
            versions[*count] = ver;
            (*count)++;
        }
    }

    closedir(d);
    return SS$_NORMAL;
}

/*
 * vmsfs_resolve_version - Resolve a requested version to an actual version number.
 *
 * Resolution rules:
 *   version == 0  -> Return the highest existing version
 *   version > 0   -> Return that version if it exists, else -1
 *   version == -1 -> Error (invalid)
 *
 * Parameters:
 *   linux_dir         - Directory to scan
 *   basename          - File name without extension
 *   ext               - File extension without dot (may be NULL)
 *   requested_version - The version to resolve
 *
 * Returns the actual version number, or -1 if not found/invalid.
 */
int vmsfs_resolve_version(const char *linux_dir, const char *basename, const char *ext,
                           int requested_version)
{
    if (!linux_dir || !basename) return -1;

    if (requested_version == 0) {
        /* ;0 means highest version */
        int highest = vmsfs_get_highest_version(linux_dir, basename, ext);
        return (highest > 0) ? highest : -1;
    }

    if (requested_version < 0) {
        /* Negative versions are not valid in VMS */
        return -1;
    }

    /* Specific version requested - verify it exists */
    char base_pattern[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 2];
    if (build_base_pattern(basename, ext, base_pattern, sizeof(base_pattern)) < 0) {
        return -1;
    }
    size_t base_len = strlen(base_pattern);

    DIR *d = opendir(linux_dir);
    if (!d) return -1;

    int found = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        int ver = match_versioned_entry(entry->d_name, base_pattern, base_len);
        if (ver == requested_version) {
            found = 1;
            break;
        }
    }

    closedir(d);

    if (found) {
        return requested_version;
    }

    /*
     * Check if the unversioned file exists and requested version is 1.
     * An unversioned file is implicitly version 1.
     */
    if (requested_version == 1) {
        char unversioned_path[VMSFS_MAX_PATH];
        snprintf(unversioned_path, sizeof(unversioned_path), "%s/%s",
                 linux_dir, base_pattern);

        struct stat st;
        if (stat(unversioned_path, &st) == 0) {
            return 1;
        }
    }

    return -1;  /* Version not found */
}
