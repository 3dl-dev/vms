/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vmsfs_volume.c - the userspace MOUNTED-VOLUME table (vms-351, epic vms-5eb
 * "B2"): device name -> (open backing fd + cached genuine-ODS-2 block-backed
 * reader handle, ods2_bdev_t).
 *
 * See vmsfs/volume.h for the contract and the atomic-safety / fail-honest
 * rationale. This module deliberately lives OUTSIDE the LIBVMSFS$SHR.EXE
 * shareable image (src/vmsfs/CMakeLists.txt builds it as a separate static
 * library that links src/vmsfs/ods2): folding the ODS-2 reader's block-device
 * primitives (pread/lseek and the ods2_bdev_* symbols) into the STRICT
 * symbol-vector link of LIBVMSFS$SHR (src/vmslink/mk_vmsfs_shr.sh, no
 * --allow-undefined) would cascade the flip's dependencies into the live
 * shareable before the atomic group is ready. Keeping it a distinct static
 * module linked directly by its consumers (PID 1 today; RMS/DCL when the flip
 * lands) is what keeps this additive.
 *
 * OVMX Project
 */

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "ssdef.h"

/* A device name never exceeds VMS_DEVNAM_MAX (vmsfs/device.h); reproduce the
 * cap locally rather than pull device.h in, keeping this module's dependency
 * set to ods2 + libc only. */
#define VMS_VOLUME_DEVNAM_MAX 32

/* Mutex protecting volume_table and volume_count_val. */
static pthread_mutex_t volume_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct {
    char        devname[VMS_VOLUME_DEVNAM_MAX + 1];
    int         fd;             /* backing block device / image fd (OWNED) */
    ods2_bdev_t bv;             /* validated block-backed reader over fd    */
    int         active;
} volume_table[VMS_VOLUME_MAX_DEVICES];

static int volume_count_val = 0;

/*
 * Normalize a device name: uppercase, strip a trailing colon -- the identical
 * convention normalize_devname() in vmsfs_device.c uses, so a device registered
 * here resolves under the SAME key the rest of the VMS namespace uses.
 */
static void normalize_devname(const char *input, char *output, size_t size)
{
    size_t i;
    for (i = 0; i < size - 1 && input[i] && input[i] != ':'; i++)
        output[i] = (char)toupper((unsigned char)input[i]);
    output[i] = '\0';
}

/* Close a slot's fd and mark it free. Caller holds volume_mutex. */
static void slot_release_locked(int i)
{
    if (volume_table[i].fd >= 0)
        close(volume_table[i].fd);
    volume_table[i].fd = -1;
    volume_table[i].devname[0] = '\0';
    volume_table[i].active = 0;
}

int vmsfs_volume_register(const char *devname, const char *backing_path,
                          ods2_status_t *ods2_out)
{
    if (ods2_out)
        *ods2_out = ODS2_OK;

    if (!devname || !backing_path)
        return SS$_BADPARAM;

    char normalized[VMS_VOLUME_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));
    if (normalized[0] == '\0')
        return SS$_BADPARAM;

    /*
     * Open + validate OUTSIDE the table lock -- ods2_bdev_open() does real I/O
     * (pread of the home block) and must not serialize every registrar. The fd
     * and the parsed handle are stack-local until we commit them under the lock.
     * O_CLOEXEC: a child (a RUN'd image) inherits neither this fd nor this
     * process's table; it registers its own.
     */
    int fd = open(backing_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return SS$_NOSUCHDEV;

    ods2_bdev_t bv;
    ods2_status_t st = ods2_bdev_open(&bv, fd, 0 /* auto-detect span */);
    if (st != ODS2_OK) {
        /* Fail-honest (Rule 9 / INV-6): the backing store is not a genuine
         * ODS-2 volume. Register NOTHING; report the reader's real error. */
        close(fd);
        if (ods2_out)
            *ods2_out = st;
        return SS$_DEVNOTMOUNT;
    }

    int result = SS$_DEVALLOC;  /* table full, unless a slot is found */
    pthread_mutex_lock(&volume_mutex);

    /* Re-register: drop any existing handle for this name first. */
    for (int i = 0; i < VMS_VOLUME_MAX_DEVICES; i++) {
        if (volume_table[i].active &&
            strcasecmp(volume_table[i].devname, normalized) == 0) {
            slot_release_locked(i);
            volume_count_val--;
            break;
        }
    }

    for (int i = 0; i < VMS_VOLUME_MAX_DEVICES; i++) {
        if (!volume_table[i].active) {
            strncpy(volume_table[i].devname, normalized,
                    VMS_VOLUME_DEVNAM_MAX);
            volume_table[i].devname[VMS_VOLUME_DEVNAM_MAX] = '\0';
            volume_table[i].fd = fd;
            volume_table[i].bv = bv;   /* bv.fd == fd (borrowed by the handle) */
            volume_table[i].active = 1;
            volume_count_val++;
            result = SS$_NORMAL;
            break;
        }
    }

    pthread_mutex_unlock(&volume_mutex);

    if (result != SS$_NORMAL)
        close(fd);  /* table was full: do not leak the fd we opened */

    return result;
}

const ods2_bdev_t *vmsfs_volume_handle(const char *devname)
{
    if (!devname)
        return NULL;

    char normalized[VMS_VOLUME_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));

    const ods2_bdev_t *handle = NULL;
    pthread_mutex_lock(&volume_mutex);

    for (int i = 0; i < VMS_VOLUME_MAX_DEVICES; i++) {
        if (volume_table[i].active &&
            strcasecmp(volume_table[i].devname, normalized) == 0) {
            /* The handle is read-only after registration; returning an
             * interior pointer is safe for the life of the registration
             * (owned by the table until unregister/exit). */
            handle = &volume_table[i].bv;
            break;
        }
    }

    pthread_mutex_unlock(&volume_mutex);
    return handle;
}

int vmsfs_volume_unregister(const char *devname)
{
    if (!devname)
        return SS$_BADPARAM;

    char normalized[VMS_VOLUME_DEVNAM_MAX + 1];
    normalize_devname(devname, normalized, sizeof(normalized));

    int result = SS$_NOSUCHDEV;
    pthread_mutex_lock(&volume_mutex);

    for (int i = 0; i < VMS_VOLUME_MAX_DEVICES; i++) {
        if (volume_table[i].active &&
            strcasecmp(volume_table[i].devname, normalized) == 0) {
            slot_release_locked(i);
            volume_count_val--;
            result = SS$_NORMAL;
            break;
        }
    }

    pthread_mutex_unlock(&volume_mutex);
    return result;
}

int vmsfs_volume_count(void)
{
    pthread_mutex_lock(&volume_mutex);
    int count = volume_count_val;
    pthread_mutex_unlock(&volume_mutex);
    return count;
}
