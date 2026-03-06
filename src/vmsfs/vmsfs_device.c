/*
 * vmsfs_device.c - VMS Device Table
 *
 * Maps VMS device names to Linux mount points. This is the lowest layer
 * of the VMS namespace — the single point where Unix paths are known.
 *
 * On a real VMS system, devices are managed by the I/O subsystem with
 * Unit Control Blocks (UCBs). For OVMX, a simple static table suffices.
 *
 * OVMX Project
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>

#include "vmsfs/device.h"
#include "ssdef.h"

/* Mutex protecting device_table and device_count_val */
static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
    char devname[VMS_DEVNAM_MAX + 1];
    char mount_point[VMS_MOUNT_POINT_MAX];
    int  active;
} device_table[VMS_MAX_DEVICES];

static int device_count_val = 0;

/*
 * Normalize a device name: uppercase, strip trailing colon.
 */
static void normalize_devname(const char *input, char *output, size_t size)
{
    size_t i;
    for (i = 0; i < size - 1 && input[i] && input[i] != ':'; i++) {
        output[i] = (char)toupper((unsigned char)input[i]);
    }
    output[i] = '\0';
}

int vmsfs_device_add(const char *devname, const char *mount_point)
{
    if (!devname || !mount_point)
        return SS$_BADPARAM;

    char normalized[VMS_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));

    int result = SS$_DEVALLOC;  /* Table full */
    pthread_mutex_lock(&device_mutex);

    /* Check for existing entry — update mount point */
    for (int i = 0; i < VMS_MAX_DEVICES; i++) {
        if (device_table[i].active &&
            strcasecmp(device_table[i].devname, normalized) == 0) {
            strncpy(device_table[i].mount_point, mount_point,
                    VMS_MOUNT_POINT_MAX - 1);
            device_table[i].mount_point[VMS_MOUNT_POINT_MAX - 1] = '\0';
            result = SS$_NORMAL;
            goto out;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < VMS_MAX_DEVICES; i++) {
        if (!device_table[i].active) {
            strncpy(device_table[i].devname, normalized,
                    VMS_DEVNAM_MAX);
            device_table[i].devname[VMS_DEVNAM_MAX] = '\0';
            strncpy(device_table[i].mount_point, mount_point,
                    VMS_MOUNT_POINT_MAX - 1);
            device_table[i].mount_point[VMS_MOUNT_POINT_MAX - 1] = '\0';
            device_table[i].active = 1;
            device_count_val++;
            result = SS$_NORMAL;
            goto out;
        }
    }

out:
    pthread_mutex_unlock(&device_mutex);
    return result;
}

int vmsfs_device_resolve(const char *devname, char *mount_point, size_t size)
{
    if (!devname || !mount_point || size == 0)
        return SS$_BADPARAM;

    char normalized[VMS_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));

    int result = SS$_NOSUCHDEV;
    pthread_mutex_lock(&device_mutex);

    for (int i = 0; i < VMS_MAX_DEVICES; i++) {
        if (device_table[i].active &&
            strcasecmp(device_table[i].devname, normalized) == 0) {
            strncpy(mount_point, device_table[i].mount_point, size - 1);
            mount_point[size - 1] = '\0';
            result = SS$_NORMAL;
            break;
        }
    }

    pthread_mutex_unlock(&device_mutex);
    return result;
}

int vmsfs_device_remove(const char *devname)
{
    if (!devname)
        return SS$_BADPARAM;

    char normalized[VMS_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));

    int result = SS$_NOSUCHDEV;
    pthread_mutex_lock(&device_mutex);

    for (int i = 0; i < VMS_MAX_DEVICES; i++) {
        if (device_table[i].active &&
            strcasecmp(device_table[i].devname, normalized) == 0) {
            device_table[i].active = 0;
            device_table[i].devname[0] = '\0';
            device_table[i].mount_point[0] = '\0';
            device_count_val--;
            result = SS$_NORMAL;
            break;
        }
    }

    pthread_mutex_unlock(&device_mutex);
    return result;
}

int vmsfs_device_count(void)
{
    pthread_mutex_lock(&device_mutex);
    int count = device_count_val;
    pthread_mutex_unlock(&device_mutex);
    return count;
}
