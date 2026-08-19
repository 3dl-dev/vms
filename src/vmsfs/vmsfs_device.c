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

#include <stdio.h>
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

/*
 * Is `mount_point` present in the kernel's own mount table right now?
 *
 * This is the SAME cross-process, kernel-reported truth src/vmsdcl/
 * dcl_cmd_misc.c's mount_point_is_mounted() and src/product/product.c's
 * pd_mount_point_is_mounted() read: /proc/mounts, the kernel's OWN mount
 * table, NOT a field only the mounting process can see. Reproduced here (not
 * shared) because vmsfs is BELOW vmsdcl in the library graph and must not pull
 * a DCL symbol; the parse is trivial and identical.
 *
 * Parsed by hand with fopen/fgets rather than getmntent(3): DECC$SHR's symbol
 * vector exports the plain stdio family the VMS-native LINK.EXE build links
 * against everywhere, but not setmntent/getmntent -- see the note on
 * mount_point_is_mounted() in dcl_cmd_misc.c.
 */
static int mount_table_has(const char *mount_point)
{
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp)
        return 0;

    /* /proc/mounts: "<source> <mount_point> <fstype> <opts> ...". Compare the
     * SECOND field's exact extent, not a substring -- "/mnt/dka1" must not
     * match "/mnt/dka100". */
    char line[512];
    size_t mp_len = strlen(mount_point);
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *field2 = strchr(line, ' ');
        if (!field2) continue;
        field2++;
        char *after = strchr(field2, ' ');
        if (!after) continue;
        size_t flen = (size_t)(after - field2);
        if (flen == mp_len && strncmp(field2, mount_point, flen) == 0) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/*
 * Executive-mount-table fallback for a device the PER-PROCESS device_table
 * does not carry (vms-8b6).
 *
 * THE BUG THIS CLOSES. device_table above is process-private. MOUNT (cmd_mount,
 * src/vmsdcl/dcl_cmd_misc.c) does a real mount(2) of the unit's backing device
 * as vmsfs at /mnt/<devnam> AND records the mapping here -- but only in the
 * MOUNTING process's copy of this table. A forked/exec'd child (AUTHORIZE.EXE,
 * SYSGEN.EXE, any RUN'd image) starts with a fresh table holding only the
 * bootstrap DKA0:, so it could not resolve a device its DCL parent mounted and
 * failed with %..-NOSUCHDEV / %UAF-E-NOSUCHUSER even though the volume was
 * genuinely mounted and system-wide (the exact install gap vms-718 hit). VMS
 * mounts are EXECUTIVE / system-wide state, not per-process -- so device
 * resolution must consult that shared state, not one process's private copy
 * (CLAUDE.md INV-6: the kernel is the source of truth for an executive
 * facility; never a per-process view of it).
 *
 * WHAT MAKES THIS AUTHENTIC, NOT A NEW FAKE. The device -> mount-point mapping
 * is a PURE FUNCTION of the name (/mnt/<lowercase devnam>), the identical
 * convention cmd_mount / mount_point_for_device use, so any process computes it
 * with no shared userspace state. Whether the unit is actually mounted RIGHT
 * NOW is then answered by /proc/mounts -- the kernel's own mount table, global
 * and cross-process. If nothing is mounted there, the unit is no more available
 * to this process than to any other and we return NOSUCHDEV honestly (never a
 * fabricated success). DKA0: never reaches here: every process seeds it into
 * device_table at startup (vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT)), so
 * the fast path above always hits for it; only dynamically MOUNTed units
 * (/mnt/<dev>) fall through to this.
 */
static int vmsfs_device_resolve_executive(const char *normalized_dev,
                                          char *mount_point, size_t size)
{
    /* /mnt/<lowercase devnam> -- the vmsfs mount-point convention (matches
     * mount_point_for_device in src/vmsdcl/dcl_cmd_misc.c and
     * pd_mount_point_for_device in src/product/product.c). */
    char mp[VMS_MOUNT_POINT_MAX];
    char lower[VMS_DEVNAM_MAX + 1];
    size_t i;
    for (i = 0; normalized_dev[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)normalized_dev[i]);
    lower[i] = '\0';
    snprintf(mp, sizeof(mp), "/mnt/%s", lower);

    if (!mount_table_has(mp))
        return SS$_NOSUCHDEV;

    strncpy(mount_point, mp, size - 1);
    mount_point[size - 1] = '\0';
    return SS$_NORMAL;
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

    /*
     * Per-process table miss -- consult the EXECUTIVE (kernel) mount table so a
     * unit mounted by ANOTHER process (e.g. the DCL parent that forked this
     * one) still resolves (vms-8b6, INV-6). Done outside the device_mutex: it
     * reads only /proc/mounts and the pure-function name convention, touching
     * none of device_table.
     */
    if (result == SS$_NOSUCHDEV)
        result = vmsfs_device_resolve_executive(normalized, mount_point, size);

    return result;
}

/*
 * vmsfs_device_spec_kernel_mounted - vms-b3e (cross-process mount visibility
 * under the atomic flip, epic vms-208).
 *
 * Does the DEVICE named at the head of VMS filespec `spec` correspond to a unit
 * genuinely mounted RIGHT NOW in the KERNEL's own mount table (/proc/mounts) as
 * /mnt/<lowercase dev> -- i.e. a real, cross-process `mount -t vmsfs` volume
 * some OTHER process mounted (the install target vms-718 MOUNTs then RUNs
 * AUTHORIZE against; a DKA100: loop image; ...)? Returns 1 iff so.
 *
 * This reads the SAME global, cross-process /proc/mounts truth
 * vmsfs_device_resolve_executive() consults (INV-6: the kernel is the source of
 * truth for an executive facility). It exists so a caller that has just failed
 * to reach a unit through the Files-11 ODS-2 ACP (which only owns volumes IT
 * mounted) can tell a genuinely-mounted vmsfs unit apart from an absent one --
 * and, crucially, from the seeded SYS$DISK passthrough (DKA0: -> SYSDISK_MOUNT,
 * "/vms"): that unit is NOT a /mnt/<dev> mount, so this returns 0 for it and the
 * caller never resurrects the /vms masquerade the atomic flip exists to kill.
 * An UNMOUNTED unit (nothing in /proc/mounts) also returns 0 -- fail-honest, no
 * fabricated mount.
 */
int vmsfs_device_spec_kernel_mounted(const char *spec)
{
    if (!spec || !spec[0])
        return 0;

    char normalized[VMS_DEVNAM_MAX + 1];
    normalize_devname(spec, normalized, sizeof(normalized));
    if (!normalized[0])
        return 0;

    /* /mnt/<lowercase devnam> -- the vmsfs mount-point convention (matches
     * vmsfs_device_resolve_executive() above and mount_point_for_device in
     * src/vmsdcl/dcl_cmd_misc.c). */
    char mp[VMS_MOUNT_POINT_MAX];
    char lower[VMS_DEVNAM_MAX + 1];
    size_t i;
    for (i = 0; normalized[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)normalized[i]);
    lower[i] = '\0';
    snprintf(mp, sizeof(mp), "/mnt/%s", lower);

    return mount_table_has(mp);
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
