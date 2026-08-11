#ifndef __VMSFS_DEVICE_H
#define __VMSFS_DEVICE_H

#include <stddef.h>
#include <stdint.h>

/*
 * VMS Device Table
 *
 * Maps VMS device names (DKA0:, DKB0:, etc.) to their Linux mount points.
 * This is the ONE place in the system where Unix paths are acknowledged.
 * Everything above this layer speaks VMS.
 *
 * Populated by STARTUP.EXE after mounting filesystems.
 * Queried by vmsfs_resolve_device() during path translation.
 */

#define VMS_MAX_DEVICES     16
#define VMS_DEVNAM_MAX      32
#define VMS_MOUNT_POINT_MAX 256

/* Register a device with its Linux mount point */
int vmsfs_device_add(const char *devname, const char *mount_point);

/* Look up a device's Linux mount point. Returns SS$_NORMAL or SS$_NOSUCHDEV */
int vmsfs_device_resolve(const char *devname, char *mount_point, size_t size);

/* Remove a device from the table */
int vmsfs_device_remove(const char *devname);

/* Get number of registered devices */
int vmsfs_device_count(void);

/*
 * vms-d8e: Query a filespec device's concealed / rooted-directory attributes.
 *
 * Translates `device` (a bare device name, with or without a trailing ':')
 * as a logical name through LNM$FILE_DEV and inspects the logical's
 * translation attributes. A logical defined
 * /TRANSLATION_ATTRIBUTES=CONCEALED (LNM$M_CONCEALED, e.g.
 * DEFINE/TRANS=CONCEALED DISK$USER DKA100:[USERS.]) is a concealed device;
 * when its equivalence names a rooted directory (a '.' immediately before
 * the closing ']', "dev:[root.]") it is also a rooted-directory logical.
 * These are the LNM$M_CONCEALED / rooted properties RMS $PARSE reports as
 * NAM$M_CNCL_DEV / NAM$M_ROOT_DIR (VSI OpenVMS RMS Reference, NAM block
 * nam$l_fnb; VSI OpenVMS DCL Dictionary, DEFINE /TRANSLATION_ATTRIBUTES;
 * VSI OpenVMS User's Manual, "Rooted Directories").
 *
 * On return *is_concealed / *is_rooted (either may be NULL) are 0/1. Returns
 * SS$_NORMAL if `device` translated as a logical name (concealed or not),
 * SS$_NOLOGNAM if it is not a logical name at all (both outputs 0), or
 * SS$_BADPARAM for a bad argument.
 */
uint32_t vmsfs_device_concealed_rooted(const char *device,
                                       int *is_concealed, int *is_rooted);

#endif /* __VMSFS_DEVICE_H */
