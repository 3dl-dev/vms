#ifndef __VMSFS_FILESPEC_H
#define __VMSFS_FILESPEC_H

#include <stdint.h>
#include <stddef.h>

/* Maximum lengths */
#define VMSFS_MAX_NODE      64
#define VMSFS_MAX_DEVICE    64
#define VMSFS_MAX_DIRECTORY 256
#define VMSFS_MAX_NAME      39
#define VMSFS_MAX_TYPE      39
#define VMSFS_MAX_VERSION   6
#define VMSFS_MAX_FILESPEC  512
#define VMSFS_MAX_PATH      1024

/* Parsed VMS filespec: NODE::DEVICE:[DIR.SUBDIR]NAME.TYPE;VERSION */
typedef struct vmsfs_filespec {
    char node[VMSFS_MAX_NODE + 1];
    char device[VMSFS_MAX_DEVICE + 1];
    char directory[VMSFS_MAX_DIRECTORY + 1];
    char name[VMSFS_MAX_NAME + 1];
    char type[VMSFS_MAX_TYPE + 1];
    int  version;           /* -1 = not specified, 0 = highest, >0 = specific */
    /* Flags indicating which fields were present */
    unsigned has_node      : 1;
    unsigned has_device    : 1;
    unsigned has_directory : 1;
    unsigned has_name      : 1;
    unsigned has_type      : 1;
    unsigned has_version   : 1;
    unsigned is_wildcard   : 1;
} vmsfs_filespec_t;

/* Parse a VMS filespec string into components */
int vmsfs_parse_filespec(const char *spec, vmsfs_filespec_t *result);

/* Compose a VMS filespec from components */
int vmsfs_compose_filespec(const vmsfs_filespec_t *parts, char *result, size_t result_size);

/* Translate a VMS filespec to a Linux path */
int vmsfs_to_linux_path(const char *vms_spec, char *linux_path, size_t path_size);

/* Translate a Linux path to a VMS filespec */
int vmsfs_to_vms_spec(const char *linux_path, char *vms_spec, size_t spec_size);

/* Resolve a VMS device/logical to a Linux directory prefix.
 * Resolution order: device table → LNM → fallback to /vms */
int vmsfs_resolve_device(const char *device, char *linux_dir, size_t dir_size);

/* Translate VMS directory spec [DIR.SUBDIR] to Linux path component */
int vmsfs_translate_directory(const char *vms_dir, char *linux_dir, size_t dir_size);

/* Match a filename against a VMS wildcard pattern (*, %, ...) */
int vmsfs_wildcard_match(const char *pattern, const char *name);

#endif /* __VMSFS_FILESPEC_H */
