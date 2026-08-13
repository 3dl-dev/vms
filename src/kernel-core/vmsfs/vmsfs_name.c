// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_name.c - substrate-neutral ODS-2 filename FORMAT algorithms
 * (rd vms-00c, epic vms-8e8).
 *
 * These are pure ODS-2 name-shape operations promoted out of the Linux VFS
 * glue (src/kernel/vmsfs/vmsfs_blkdev.c, where they were file-static helpers)
 * into the shared core, exactly as V1 (rd vms-544) promoted version resolution
 * and retrieval-map math. Each carries NO host object -- it works only over
 * caller-provided byte buffers using the freestanding string/ctype vocabulary
 * the vmsfs_backend shim guarantees (strrchr, memcpy, strscpy, toupper,
 * strncasecmp, memcmp) -- so the move is behaviour-preserving on Linux and the
 * same source compiles against the NetBSD backend without a single `#if`.
 *
 * The block-coupled ODS-2 algorithms (the bitmap/cluster allocator and the
 * directory-block scanner) are NOT here: they need the block-buffer / volume-
 * geometry / inode seam the vmsfs_backend shim does not cover yet, and their
 * extraction is the next phase -- see src/kernel/vmsfs/README-backend.md.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own code over public, documented host
 * string/ctype primitives; no Linux/NetBSD/VSI source is copied.
 */

#include "vmsfs_core.h"

/*
 * Split "FOO.TXT" into name="FOO" and type="TXT".
 * If no dot, type is empty string.
 */
void vmsfs_split_name_type(const char *fullname,
                           char *name, size_t name_size,
                           char *type, size_t type_size)
{
    const char *dot = strrchr(fullname, '.');

    if (dot && dot != fullname) {
        size_t nlen = dot - fullname;

        if (nlen >= name_size)
            nlen = name_size - 1;
        memcpy(name, fullname, nlen);
        name[nlen] = '\0';
        strscpy(type, dot + 1, type_size);
    } else {
        strscpy(name, fullname, name_size);
        type[0] = '\0';
    }
}

/*
 * Uppercase a string in place. VMS/ODS-2 convention.
 */
void vmsfs_strupper(char *s)
{
    while (*s) {
        *s = toupper(*s);
        s++;
    }
}

/*
 * Case-insensitive comparison of a directory entry name against a target.
 */
bool vmsfs_name_match(const char *de_name, uint8_t de_name_len,
                      const char *target, unsigned int target_len,
                      int case_blind)
{
    if (de_name_len != target_len)
        return false;

    if (case_blind)
        return strncasecmp(de_name, target, target_len) == 0;
    else
        return memcmp(de_name, target, target_len) == 0;
}
