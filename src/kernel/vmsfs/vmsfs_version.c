// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_version.c - VMS file version parsing and management
 *
 * Implements version-aware filename parsing for the VMS filesystem.
 * VMS files use the naming convention "NAME.TYPE;VERSION" where VERSION
 * is an integer from 1 to 32767.
 *
 * Version semantics:
 *   ;0 or omitted -> refers to the highest existing version
 *   ;N            -> refers to exactly version N
 *   New creation  -> automatically gets highest + 1
 *
 * OVMX Project - Phase 4b: Kernel-native VMS Filesystem
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/file.h>

#include "vmsfs.h"

/*
 * vmsfs_parse_version - Parse a VMS filename into base name and version.
 *
 * The semicolon separates the base name from the version number.
 *   "FOO.TXT;3"  -> base="FOO.TXT", version=3
 *   "FOO.TXT;0"  -> base="FOO.TXT", version=0  (resolve to highest)
 *   "FOO.TXT"    -> base="FOO.TXT", version=0  (resolve to highest)
 *   "FOO.TXT;"   -> error (semicolon but no number)
 *
 * @name:      input filename (null-terminated)
 * @base:      output buffer for the base name (without ;N)
 * @base_size: size of the base buffer
 * @version:   output version number (0 = highest)
 *
 * Returns 0 on success, -EINVAL on parse error, -ENAMETOOLONG if too long.
 */
int vmsfs_parse_version(const char *name, char *base, size_t base_size,
                        int *version)
{
    const char *semi;
    size_t base_len;

    if (!name || !base || !version || base_size == 0)
        return -EINVAL;

    semi = strchr(name, ';');

    if (!semi) {
        /* No semicolon: entire name is the base, version = 0 (highest) */
        base_len = strlen(name);
        if (base_len >= base_size)
            return -ENAMETOOLONG;

        memcpy(base, name, base_len);
        base[base_len] = '\0';
        *version = 0;
        return 0;
    }

    /* Semicolon found: split into base and version */
    base_len = semi - name;
    if (base_len == 0)
        return -EINVAL;  /* No base name before semicolon */
    if (base_len >= base_size)
        return -ENAMETOOLONG;

    memcpy(base, name, base_len);
    base[base_len] = '\0';

    /* Parse the version number after the semicolon */
    semi++;  /* skip the ';' */
    if (*semi == '\0') {
        /* Trailing semicolon with no digits: treat as version 0 */
        *version = 0;
        return 0;
    }

    /* Verify all remaining characters are digits */
    {
        const char *p = semi;
        int ver = 0;

        while (*p) {
            if (!isdigit((unsigned char)*p))
                return -EINVAL;

            ver = ver * 10 + (*p - '0');
            if (ver > VMSFS_MAX_VERSION)
                return -EINVAL;
            p++;
        }

        *version = ver;
    }

    return 0;
}

/*
 * Case-insensitive comparison for matching backing directory entries
 * against a base filename pattern.  Returns true if the entry's name
 * (up to but not including the semicolon) matches @base.
 */
static bool vmsfs_name_matches_base(const char *entry_name, const char *base)
{
    const char *semi;
    size_t base_len = strlen(base);
    size_t entry_base_len;

    semi = strchr(entry_name, ';');
    if (!semi)
        return false;  /* Backing entries should always have ;N */

    entry_base_len = semi - entry_name;
    if (entry_base_len != base_len)
        return false;

    return (strncasecmp(entry_name, base, base_len) == 0);
}

/*
 * Extract the version number from a backing dir entry name.
 * The entry must be in the form "BASE;N".
 * Returns the version number, or 0 if the entry is not versioned.
 */
static int vmsfs_extract_version(const char *entry_name)
{
    const char *semi;
    const char *p;
    int ver = 0;

    semi = strchr(entry_name, ';');
    if (!semi)
        return 0;

    p = semi + 1;
    if (*p == '\0')
        return 0;

    while (*p) {
        if (!isdigit((unsigned char)*p))
            return 0;
        ver = ver * 10 + (*p - '0');
        if (ver > VMSFS_MAX_VERSION)
            return 0;
        p++;
    }

    return ver;
}

/*
 * vmsfs_find_highest_version - Find the highest version of a file.
 *
 * Iterates over the backing directory @backing_dir looking for entries
 * that match the given @base name (case-insensitively) and returns the
 * highest version number found.
 *
 * @backing_dir: dentry of the backing directory
 * @base:        base filename to match (e.g., "FOO.TXT")
 *
 * Returns the highest version number (>= 1), or 0 if no matching files.
 *
 * This function opens the backing directory and iterates using
 * iterate_dir().  We use a small callback context to collect the max.
 */
struct vmsfs_highest_ctx {
    struct dir_context ctx;
    const char *base;
    int highest;
};

static bool vmsfs_highest_actor(struct dir_context *ctx, const char *name,
                                int namlen, loff_t offset, u64 ino,
                                unsigned int d_type)
{
    struct vmsfs_highest_ctx *hctx =
        container_of(ctx, struct vmsfs_highest_ctx, ctx);
    char namebuf[VMSFS_MAX_FILENAME + 1];
    int ver;

    /* Skip . and .. */
    if (namlen == 1 && name[0] == '.')
        return true;
    if (namlen == 2 && name[0] == '.' && name[1] == '.')
        return true;

    if (namlen > VMSFS_MAX_FILENAME)
        return true;

    memcpy(namebuf, name, namlen);
    namebuf[namlen] = '\0';

    if (!vmsfs_name_matches_base(namebuf, hctx->base))
        return true;

    ver = vmsfs_extract_version(namebuf);
    if (ver > hctx->highest)
        hctx->highest = ver;

    return true;  /* continue iteration */
}

/*
 * vmsfs_find_highest_version - Find the highest version of a file.
 *
 * NOTE: This function is currently unused (dead code). The dentry_open()
 * call requires a valid vfsmount which is not available in the calling
 * context. Callers should use vmsfs_scan_highest_from_sbi() in
 * vmsfs_file.c instead, which opens via filp_open with a full path.
 *
 * Retained as a stub returning 0 to preserve the API; remove once all
 * callers have migrated.
 */
int vmsfs_find_highest_version(struct dentry *backing_dir, const char *base)
{
    (void)backing_dir;
    (void)base;
    return 0;
}

/*
 * vmsfs_build_versioned_name - Construct "BASE;N".
 *
 * @base:    base filename (e.g., "FOO.TXT")
 * @version: version number (must be >= 1)
 * @result:  output buffer
 * @size:    size of output buffer
 *
 * Returns 0 on success, -EINVAL or -ENAMETOOLONG on error.
 */
int vmsfs_build_versioned_name(const char *base, int version,
                               char *result, size_t size)
{
    int n;

    if (!base || !result || size == 0)
        return -EINVAL;
    if (version < 1 || version > VMSFS_MAX_VERSION)
        return -EINVAL;

    n = snprintf(result, size, "%s;%d", base, version);
    if (n < 0 || (size_t)n >= size)
        return -ENAMETOOLONG;

    return 0;
}
