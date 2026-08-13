/*
 * ovmx_boot_linux.c - the LINUX realization of the OVMX boot-plumbing seam
 * (rd vms-28f, epic vms-8e8).
 *
 * DO NOT call these directly from ovmx_init.c through anything but
 * "ovmx_boot.h", which declares the op contract. See that header for what
 * each op means and how it maps to NetBSD; this file is the Linux side.
 *
 * Every op here is the EXACT syscall bare_metal_init()/executive_attach()/
 * halt_now() made inline before this seam existed, moved behind the contract
 * with its arguments, flags and order preserved, so promoting PID 1 onto the
 * seam is behaviour-identical (rd vms-28f, zero behaviour change):
 *
 *   ovmx_boot_kernel_filesystems_mounted -> stat("/proc/version") == 0
 *   ovmx_boot_mount_kernel_filesystems   -> the proc/sys/dev/tmp/pts/shm
 *                                           mount(2) sequence, same order
 *   ovmx_boot_start_console_log_bridge   -> opcom_kmsg_start()  (/dev/kmsg)
 *   ovmx_boot_load_module                -> /lib/modules/<name>.ko via
 *                                           open + SYS_finit_module + close
 *   ovmx_boot_open_executive             -> open("/dev/vms", O_RDWR|O_CLOEXEC)
 *   ovmx_boot_system_disk_dev            -> "/dev/vda"
 *   ovmx_boot_system_disk_present        -> stat("/dev/vda") && S_ISBLK
 *   ovmx_boot_mount_system_disk          -> mount(dev, mp, "vmsfs", 0, NULL)
 *   ovmx_boot_power_off                  -> sync(); reboot(RB_POWER_OFF)
 *
 * Clean-room (CLAUDE.md Rule 8): these call only public, documented Linux
 * syscalls. No code is copied from the Linux source.
 */

/* _POSIX_C_SOURCE / _DEFAULT_SOURCE come from the ovmx_init target's own
 * compile definitions (CMakeLists.txt) -- not redefined here. */
#include "ovmx_boot.h"
#include "opcom_kmsg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/reboot.h>

/* The Linux host path for the system/boot disk -- the substrate-specific name
 * the seam exists to hide from ovmx_init.c; the NetBSD backend supplies its
 * own (vms-f2e). The executive device path "/dev/vms" is NOT hidden behind a
 * macro: it is written literally in ovmx_boot_open_executive() below, exactly
 * as executive_attach() opened it before the seam, so the Rule 9 gate
 * (tests/integration/test_runtime_target.sh, check 3b-backend) can verify the
 * backend really opens the executive device and does not fake a descriptor. */
#define OVMX_BOOT_SYSDISK_DEV   "/dev/vda"

int ovmx_boot_kernel_filesystems_mounted(void)
{
    struct stat st;
    return stat("/proc/version", &st) == 0;
}

int ovmx_boot_mount_kernel_filesystems(void)
{
    /* Byte-for-byte the sequence bare_metal_init() ran inline before the
     * seam: proc, sysfs, devtmpfs, tmpfs, devpts (mkdir first), tmpfs on
     * /dev/shm (mkdir first). Best-effort -- returns are ignored today. */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", "devpts", 0, NULL);
    mkdir("/dev/shm", 0755);
    mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
    return 0;
}

void ovmx_boot_start_console_log_bridge(void)
{
    opcom_kmsg_start();
}

int ovmx_boot_load_module(const char *name)
{
    char path[256];
    struct stat st;
    int fd;
    long rc;
    int saved;

    snprintf(path, sizeof(path), "/lib/modules/%s.ko", name);

    if (stat(path, &st) != 0)
        return -1;                 /* errno from stat (ENOENT if absent) */
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;                 /* errno from open */
    rc = syscall(SYS_finit_module, fd, "", 0);
    saved = errno;
    close(fd);
    if (rc != 0) {
        errno = saved;             /* EEXIST if already loaded, etc. */
        return -1;
    }
    return 0;
}

int ovmx_boot_open_executive(void)
{
    return open("/dev/vms", O_RDWR | O_CLOEXEC);
}

const char *ovmx_boot_system_disk_dev(void)
{
    return OVMX_BOOT_SYSDISK_DEV;
}

int ovmx_boot_system_disk_present(void)
{
    struct stat st;
    return stat(OVMX_BOOT_SYSDISK_DEV, &st) == 0 && S_ISBLK(st.st_mode);
}

int ovmx_boot_mount_system_disk(const char *mountpoint)
{
    return mount(OVMX_BOOT_SYSDISK_DEV, mountpoint, "vmsfs", 0, NULL);
}

void ovmx_boot_power_off(void)
{
    sync();
    reboot(RB_POWER_OFF);
    /* Only reached without CAP_SYS_BOOT; the caller then _exit()s. */
}
