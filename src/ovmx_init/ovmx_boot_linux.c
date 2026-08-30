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
 *   ovmx_boot_power_off                  -> sync(); reboot(RB_POWER_OFF)
 *
 * Clean-room (CLAUDE.md Rule 8): these call only public, documented Linux
 * syscalls. No code is copied from the Linux source.
 */

/* _POSIX_C_SOURCE / _DEFAULT_SOURCE come from the ovmx_init target's own
 * compile definitions (CMakeLists.txt) -- not redefined here. */
#include "ovmx_boot.h"
#include "opcom_kmsg.h"
#include "vms_kif.h"
#include "ovmx_layout.h"        /* SYSDISK_DEVICE -- the substrate default unit */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>             /* getenv */
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/reboot.h>

/* The Linux host path for the DEFAULT system/boot disk (vda) -- the fallback
 * when boot discovery selects no other unit; ovmx_boot_system_disk_dev() derives
 * the actual node from the discovered unit (vms-9f5, boot_sysdev_dev below). The
 * executive device path "/dev/vms" is NOT hidden behind a macro: it is written
 * literally in ovmx_boot_open_executive() below, exactly as executive_attach()
 * opened it before the seam, so the Rule 9 gate
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

void ovmx_boot_mute_kernel_console(void)
{
    /* SYSLOG_ACTION_CONSOLE_LEVEL = 8 (syslog(2)/klogctl(2) man page).
     * Issued as a raw syscall rather than through <sys/klog.h>'s klogctl()
     * wrapper so this file depends on no header beyond what it already
     * includes (<sys/syscall.h>, already pulled in above for
     * SYS_finit_module). Level 3 lets EMERG(0)/ALERT(1)/CRIT(2) through --
     * everything a real kernel bugcheck-class fault would use -- and blocks
     * pr_info/pr_warn (levels 4-6), which is exactly the vms.ko/vmsfs.ko
     * lifecycle noise vms-300 reported. */
    long rc = syscall(SYS_syslog, 8 /* SYSLOG_ACTION_CONSOLE_LEVEL */, NULL, 3);
    if (rc == 0)
        return;

    /* Fallback: /proc/sys/kernel/printk's first whitespace-separated field
     * is console_loglevel (proc(5)). Needs /proc mounted, which
     * ovmx_boot_mount_kernel_filesystems() has already done by the time
     * bare_metal_init() calls this op. */
    int fd = open("/proc/sys/kernel/printk", O_WRONLY);
    if (fd >= 0) {
        ssize_t w = write(fd, "3\n", 2);
        close(fd);
        if (w == 2)
            return;
    }

    /* Best-effort (this op's header comment in ovmx_boot.h): neither path
     * worked, so boot proceeds with the kernel's default console level --
     * but say so, since a silent failure here is exactly how this leak went
     * unnoticed in the first place. */
    fprintf(stderr,
            "%%OVMX-W-CONSOLELVL, could not lower kernel console log level\n");
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

/* Parse an "ovmx.sysdev=<unit>" token from the kernel command line into `out`
 * (NUL-terminated, e.g. "VDA100:"). Returns 1 if found, else 0. This is how a
 * real boot selects a non-default system disk -- the console/loader names it,
 * exactly as VMB hands VMS the boot device -- and the channel the alternate-disk
 * boot regression test (tests/qemu/test_boot_alternate_disk.sh) drives. */
static int boot_cmdline_sysdev(char *out, size_t outsz)
{
    char buf[1024];
    ssize_t n;
    int fd;
    const char *key = "ovmx.sysdev=";
    char *p;
    size_t i;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    p = strstr(buf, key);
    if (!p)
        return 0;
    p += strlen(key);
    for (i = 0; p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n'
                && i + 1 < outsz; i++)
        out[i] = p[i];
    out[i] = '\0';
    return i > 0;
}

/* DEVICE-NATIVE BOOT DISCOVERY (vms-9f5). WHICH unit is the system/boot disk is
 * DISCOVERED, not compile-frozen. Precedence, highest first:
 *   1. the OVMX_SYSDEVICE environment variable (a test, or a wrapper that
 *      already knows the unit -- imgact.c / lnm_defaults.c read the same var);
 *   2. an "ovmx.sysdev=<unit>" token on the kernel command line;
 *   3. the substrate compile-time default SYSDISK_DEVICE ":" (VDA0: on virtio).
 * Resolved ONCE and cached: PID 1 calls the accessors several times (present
 * check, ACP $MOUNT, console messages) and the answer must not change mid-boot. */
static const char *boot_sysdev_unit(void)
{
    static char unit[32];
    static int resolved;
    const char *env;

    if (resolved)
        return unit;

    env = getenv("OVMX_SYSDEVICE");
    if (env && env[0])
        snprintf(unit, sizeof(unit), "%s", env);
    else if (!boot_cmdline_sysdev(unit, sizeof(unit)))
        snprintf(unit, sizeof(unit), "%s:", SYSDISK_DEVICE);
    resolved = 1;
    return unit;
}

/* Derive the Linux virtio backing node for the discovered unit by its unit
 * NUMBER: the executive names the Nth virtio disk VDA(N*100): (vms_devtab.c), so
 * unit number K maps to /dev/vd[a + K/100]. The device-code prefix is not
 * inspected (a legacy DKA<k>: resolves the same), only the number selects the
 * disk. Out-of-range or no-digit falls back to vda; an absent disk then fails
 * honestly in present() rather than being invented (INV-6). */
static const char *boot_sysdev_dev(void)
{
    static char dev[32];
    static int resolved;
    const char *unit, *d;
    unsigned num = 0, idx;

    if (resolved)
        return dev;

    unit = boot_sysdev_unit();
    for (d = unit; *d; d++) {
        if (*d >= '0' && *d <= '9') {
            num = (unsigned)strtoul(d, NULL, 10);
            break;
        }
    }
    idx = num / 100u;
    if (idx > 25u)
        idx = 0u;               /* beyond vd[a-z]: default vda, present() fails honestly */
    snprintf(dev, sizeof(dev), "/dev/vd%c", (char)('a' + idx));
    resolved = 1;
    return dev;
}

const char *ovmx_boot_system_disk_dev(void)
{
    return boot_sysdev_dev();
}

int ovmx_boot_system_disk_present(void)
{
    struct stat st;
    const char *dev = boot_sysdev_dev();
    return stat(dev, &st) == 0 && S_ISBLK(st.st_mode);
}

/* The VMS device name of the boot/system unit as the executive enumerates it
 * (vms_devtab.c: the Nth virtio disk -> VDA(N*100):). This is what $ASSIGN / the
 * Files-11 ACP $MOUNT name, NOT the Linux "/dev/vd?" path -- the ACP owns the
 * genuine ODS-2 block device directly (vms-5f0, epic vms-208). Device-native and
 * boot-discovered (vms-9f5): the default is the substrate's SYSDISK_DEVICE, and
 * the boot may select another disk via OVMX_SYSDEVICE / ovmx.sysdev=. */
const char *ovmx_boot_system_disk_unit(void)
{
    return boot_sysdev_unit();
}

/* ATOMIC FLIP (vms-5f0): $MOUNT the boot unit through the Files-11 (ODS-2) ACP
 * in the executive, recording it executive-global so SYS$DISK is served by the
 * ACP -- replacing the Linux vmsfs.ko VFS mount of a bespoke-VMFS volume at
 * /vms. Requires /dev/vms already open (executive_attach()). Returns 0 on a
 * successful ACP mount, -1 otherwise (VMS status is odd == success); a caller
 * that gets -1 halts honestly exactly as the old vmsfs mount(2) failure did. */
int ovmx_boot_acp_mount_system_disk(void)
{
    uint32_t st = vms_kif_acp_mount(ovmx_boot_system_disk_unit());
    if (st & 1)          /* odd VMS status == success */
        return 0;
    errno = 0;           /* not an errno; the caller reports the VMS unit name */
    return -1;
}

/* The flagless boot path's whole system-disk mount, Linux side (vms-5f0): the
 * ATOMIC FLIP $MOUNTs the boot unit through the executive Files-11 (ODS-2) ACP
 * -- there is no vmsfs.ko VFS mount on Linux any more. Relocated out of
 * ovmx_init.c so the boot sequence stays ONE substrate-neutral source. */
int ovmx_boot_mount_system_disk_native(void)
{
    return ovmx_boot_acp_mount_system_disk();
}

/* The ACP-read boot bridge's writable staging directory, Linux side (vms-5f0).
 * PID 1 runs on an initramfs that is already a tmpfs, so this is exactly the
 * two mkdir(2) calls stage_boot_images() made inline before the op existed --
 * same paths, same 0755 mode, same EEXIST tolerance. */
int ovmx_boot_prepare_stage_dir(const char *dir)
{
    if (mkdir("/run", 0755) != 0 && errno != EEXIST)
        return -1;
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

void ovmx_boot_power_off(void)
{
    sync();
    reboot(RB_POWER_OFF);
    /* Only reached without CAP_SYS_BOOT; the caller then _exit()s. */
}
