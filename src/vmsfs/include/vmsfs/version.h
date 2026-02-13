#ifndef __VMSFS_VERSION_H
#define __VMSFS_VERSION_H

#include <stdint.h>
#include <stddef.h>

/* Get the highest version number for a file (scan directory for ;N suffixes) */
int vmsfs_get_highest_version(const char *linux_dir, const char *basename, const char *ext);

/* Get the Linux filename for a specific version */
int vmsfs_version_filename(const char *basename, const char *ext, int version,
                           char *result, size_t result_size);

/* Create a new version of a file (auto-increment version) */
int vmsfs_create_new_version(const char *linux_dir, const char *basename, const char *ext,
                              char *result_path, size_t path_size, int *new_version);

/* Purge old versions, keeping the highest N */
int vmsfs_purge_versions(const char *linux_dir, const char *basename, const char *ext,
                          int keep_count);

/* List all versions of a file */
int vmsfs_list_versions(const char *linux_dir, const char *basename, const char *ext,
                         int *versions, int max_versions, int *count);

/* Resolve version ;0 (highest), ;-1 (next lower), etc. to actual version number */
int vmsfs_resolve_version(const char *linux_dir, const char *basename, const char *ext,
                           int requested_version);

#endif /* __VMSFS_VERSION_H */
