#ifndef __VMSFS_DEVICE_H
#define __VMSFS_DEVICE_H

#include <stddef.h>

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

#endif /* __VMSFS_DEVICE_H */
