/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_backend_linux.h - the LINUX realization of the OVMX ODS-2 / vmsfs
 * kernel-backend shim (rd vms-544, epic vms-8e8).
 *
 * DO NOT include this directly -- include "vmsfs_backend.h", which selects this
 * file on a Linux kernel build. See that header for the vocabulary contract.
 *
 * Every name a core ODS-2 file uses resolves here to the EXACT primitive the
 * filesystem already used, so an algorithm moved onto the shim compiles to
 * behaviour-identical code:
 *
 *   uint8/16/32/64_t, size_t, bool    <- <linux/types.h>
 *   strchr/strrchr/strlen/strncasecmp <- <linux/string.h>
 *     /memcpy/memcmp/strscpy
 *   isdigit/toupper                   <- <linux/ctype.h>
 *   snprintf                          <- <linux/kernel.h>
 *   VMSFS_EINVAL / ENAMETOOLONG /     == the Linux errno values EINVAL /
 *     EIO / ENOSPC                       ENAMETOOLONG / EIO / ENOSPC
 *
 * Because each VMSFS_E* is defined to the host errno, a core function that
 * returns -VMSFS_EINVAL returns exactly the -EINVAL the Linux VFS glue in
 * src/kernel/vmsfs/ expects -- the extraction is byte-value-identical on the
 * return path.
 *
 * Clean-room (CLAUDE.md Rule 8): these mappings name only public, documented
 * Linux kernel APIs. No code is copied from the Linux source.
 */

#ifndef OVMX_VMSFS_BACKEND_LINUX_H
#define OVMX_VMSFS_BACKEND_LINUX_H

#include <linux/types.h>    /* uint8/16/32/64_t, size_t, bool */
#include <linux/string.h>   /* strchr, strrchr, strlen, strncasecmp, memcpy, memcmp, strscpy */
#include <linux/ctype.h>    /* isdigit, toupper */
#include <linux/kernel.h>   /* snprintf */
#include <linux/errno.h>    /* EINVAL, ENAMETOOLONG, EIO, ENOSPC */

/* ---- normalized ODS-2-core error codes == the host errno values ---- */
#define VMSFS_EINVAL       EINVAL
#define VMSFS_ENAMETOOLONG ENAMETOOLONG
#define VMSFS_EIO          EIO
#define VMSFS_ENOSPC       ENOSPC

#endif /* OVMX_VMSFS_BACKEND_LINUX_H */
