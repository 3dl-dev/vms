// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_version.c - VMS file version parsing and management (ODS-2 core)
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
 * SUBSTRATE-NEUTRAL ODS-2 CORE (rd vms-544, epic vms-8e8). This file was
 * promoted out of src/kernel/vmsfs/ onto the kernel-backend shim: it names no
 * host type and includes no <linux/...> header. Its string/ctype primitives, its
 * fixed-width types and its error codes all come through vmsfs_core.h ->
 * vmsfs_backend.h, so the same source compiles on the Linux backend (this
 * build) and, later, on a NetBSD backend without a single `#if`.
 *
 * OVMX Project - Phase 4b: Kernel-native VMS Filesystem
 */

#include "vmsfs_core.h"

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
 * Returns 0 on success, -VMSFS_EINVAL on parse error, -VMSFS_ENAMETOOLONG if
 * too long.
 */
int vmsfs_parse_version(const char *name, char *base, size_t base_size,
                        int *version)
{
    const char *semi;
    size_t base_len;

    if (!name || !base || !version || base_size == 0)
        return -VMSFS_EINVAL;

    semi = strchr(name, ';');

    if (!semi) {
        /* No semicolon: entire name is the base, version = 0 (highest) */
        base_len = strlen(name);
        if (base_len >= base_size)
            return -VMSFS_ENAMETOOLONG;

        memcpy(base, name, base_len);
        base[base_len] = '\0';
        *version = 0;
        return 0;
    }

    /* Semicolon found: split into base and version */
    base_len = semi - name;
    if (base_len == 0)
        return -VMSFS_EINVAL;  /* No base name before semicolon */
    if (base_len >= base_size)
        return -VMSFS_ENAMETOOLONG;

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
                return -VMSFS_EINVAL;

            ver = ver * 10 + (*p - '0');
            if (ver > VMSFS_MAX_VERSION)
                return -VMSFS_EINVAL;
            p++;
        }

        *version = ver;
    }

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
 * Returns 0 on success, -VMSFS_EINVAL or -VMSFS_ENAMETOOLONG on error.
 */
int vmsfs_build_versioned_name(const char *base, int version,
                               char *result, size_t size)
{
    int n;

    if (!base || !result || size == 0)
        return -VMSFS_EINVAL;
    if (version < 1 || version > VMSFS_MAX_VERSION)
        return -VMSFS_EINVAL;

    n = snprintf(result, size, "%s;%d", base, version);
    if (n < 0 || (size_t)n >= size)
        return -VMSFS_ENAMETOOLONG;

    return 0;
}
