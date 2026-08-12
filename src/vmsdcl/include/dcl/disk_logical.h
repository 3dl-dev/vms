#ifndef __DCL_DISK_LOGICAL_H
#define __DCL_DISK_LOGICAL_H

#include <stddef.h>
#include <stdint.h>

#include "vms/logical.h"

/*
 * DISK$<volume-label> system logical, established by MOUNT (vms-f83, Engine B
 * / vms-8ad, disk transparency).
 *
 * VMS MOUNT defines a logical name DISK$<label> whose equivalence is the
 * mounted device, so a volume can be addressed by its label independent of the
 * physical unit it happens to occupy -- DISK$MYVOL:[DIR]FILE works wherever
 * MYVOL is mounted (VSI OpenVMS System Manager's Manual, Vol. 1, "Mounting
 * Volumes"; VSI OpenVMS DCL Dictionary, MOUNT -- the volume-label logical name).
 * DISMOUNT removes it.
 *
 * The label the DISK$ name is built from is READ FROM THE VOLUME (its home
 * block), never fabricated from the command line: if the label cannot be read
 * MOUNT defines no DISK$ logical rather than inventing one (CLAUDE.md INV-DCL).
 */

/*
 * dcl_disk_logical_name - build the "DISK$<LABEL>" logical name from a volume
 * label. The label is uppercased and validated: it must be 1..12 characters
 * (the ODS-2 / vmsfs volume-label limit) of printable, non-space ASCII after
 * trimming trailing spaces (vmsfs pads hb_volname with spaces). `out` receives
 * the NUL-terminated "DISK$<LABEL>" string.
 *
 * Returns SS$_NORMAL on success, SS$_BADPARAM for a NULL/oversized buffer or a
 * label that is empty or out of range after trimming.
 */
uint32_t dcl_disk_logical_name(const char *label, char *out, size_t out_size);

/*
 * dcl_mount_define_disk - define the DISK$<label> logical (equivalence =
 * `dev_name`, e.g. "DKA100:") in `table`, via the same lnm_create() path
 * DEFINE uses. `label` is the volume label read from the mounted volume.
 * Returns lnm_create()'s status, or SS$_BADPARAM / the dcl_disk_logical_name()
 * status if the label is unusable (in which case NOTHING is defined).
 */
uint32_t dcl_mount_define_disk(lnm_manager_t *mgr, const char *table,
                               const char *label, const char *dev_name);

/*
 * dcl_mount_remove_disk - delete the DISK$<label> logical from `table`
 * (DISMOUNT). Returns lnm_delete()'s status, or SS$_BADPARAM / the
 * dcl_disk_logical_name() status if the label is unusable.
 */
uint32_t dcl_mount_remove_disk(lnm_manager_t *mgr, const char *table,
                               const char *label);

#endif /* __DCL_DISK_LOGICAL_H */
