/*
 * dcl_disk_logical.c - the DISK$<volume-label> system logical MOUNT
 * establishes and DISMOUNT removes (vms-f83, Engine B / vms-8ad, disk
 * transparency).
 *
 * VMS MOUNT defines DISK$<label> so a mounted volume can be named by its
 * label independent of the physical unit -- DISK$MYVOL:[DIR]FILE resolves
 * wherever MYVOL is mounted (VSI OpenVMS System Manager's Manual, Vol. 1,
 * "Mounting Volumes"; VSI OpenVMS DCL Dictionary, MOUNT). The label is read
 * from the volume itself (its home block), so the DISK$ name always reflects
 * the disk that is actually mounted; if the label cannot be read, MOUNT
 * defines no DISK$ logical rather than fabricating one (CLAUDE.md INV-DCL --
 * real behaviour or an honest omission, never a fake).
 *
 * These helpers are intentionally free of any executive / mount(2) coupling
 * (they touch only the logical-name manager) so they are exercised directly
 * on the host, against LNM$PROCESS, where ctest runs and no /dev/vms exists
 * (tests/vmsrms/test_disk_transparency.c). The full MOUNT -> DISK$ ->
 * DISK$label:[dir]file path is proven end-to-end in QEMU
 * (tests/qemu/test_mount_e2e.sh), the only place a real mount(2) happens.
 */

#include <ctype.h>
#include <string.h>

#include "dcl/disk_logical.h"
#include "ssdef.h"

/* ODS-2 / vmsfs volume labels are at most 12 characters. */
#define DCL_DISK_LABEL_MAX 12

uint32_t dcl_disk_logical_name(const char *label, char *out, size_t out_size)
{
    if (!label || !out || out_size < sizeof("DISK$") + DCL_DISK_LABEL_MAX)
        return SS$_BADPARAM;

    /* Copy the label, uppercasing, until a trailing-space run or the end.
     * vmsfs pads hb_volname with spaces, so trailing spaces are not part of
     * the label; an embedded space (never produced by INITIALIZE) or any
     * non-printable byte makes the label unusable. */
    char up[DCL_DISK_LABEL_MAX + 1];
    size_t n = 0;
    for (const char *p = label; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ')
            break;                 /* trailing pad -- label ends here */
        if (!isprint(c))
            return SS$_BADPARAM;   /* not a legible label */
        if (n >= DCL_DISK_LABEL_MAX)
            return SS$_BADPARAM;   /* longer than a volume label can be */
        up[n++] = (char)toupper(c);
    }
    if (n == 0)
        return SS$_BADPARAM;       /* empty label -- nothing to name */
    up[n] = '\0';

    /* "DISK$" + label, guaranteed to fit by the out_size check above. */
    memcpy(out, "DISK$", 5);
    memcpy(out + 5, up, n + 1);
    return SS$_NORMAL;
}

uint32_t dcl_mount_define_disk(lnm_manager_t *mgr, const char *table,
                               const char *label, const char *dev_name)
{
    if (!mgr || !table || !dev_name)
        return SS$_BADPARAM;

    char logical[sizeof("DISK$") + DCL_DISK_LABEL_MAX];
    uint32_t st = dcl_disk_logical_name(label, logical, sizeof(logical));
    if (!(st & 1))
        return st;

    /* Same lnm_create() path DEFINE uses. LNM_ATTR_TERMINAL: DISK$<label>
     * names the device and iteration stops there (the equivalence is the
     * concrete "DKA100:" device, not a further logical to chase). */
    return lnm_create(mgr, table, logical, dev_name,
                      LNM_ATTR_TERMINAL, LNM_MODE_USER);
}

uint32_t dcl_mount_remove_disk(lnm_manager_t *mgr, const char *table,
                               const char *label)
{
    if (!mgr || !table)
        return SS$_BADPARAM;

    char logical[sizeof("DISK$") + DCL_DISK_LABEL_MAX];
    uint32_t st = dcl_disk_logical_name(label, logical, sizeof(logical));
    if (!(st & 1))
        return st;

    return lnm_delete(mgr, table, logical, LNM_MODE_USER);
}
