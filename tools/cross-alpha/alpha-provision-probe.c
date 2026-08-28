/*
 * alpha-provision-probe.c -- isolate WHERE PROVISION.EXE stalls on Alpha
 * (rd vms-989, rung A5a diagnostic; NOT a runtime -- a boot-context probe).
 *
 * The real OVMX/Alpha boot reaches %STDRV-I-STARTUP and then run_startup()
 * execs PROVISION.EXE, which produces no further output.  PROVISION's first
 * post-mount operations are, in order:
 *   (1) provision_home_directories() -- reads SYS$SYSTEM:SYSUAF.DAT (a vmsfs
 *       READ, known good -- OVMXVMSSYS.PAR already read at boot) and CREATES
 *       each account's home directory (a vmsfs WRITE, not yet exercised on
 *       Alpha at boot);
 *   (2) vms_kif_establish_system() -- one /dev/vms ioctl
 *       (VMS_IOCTL_ESTABLISH_SYSTEM);
 *   (3) vms_kif_getjpi_self() -- read the identity back, then print.
 *
 * This probe replays those three primitive operations under the booted kernel
 * with a print bracketing EACH, so the stall is attributed to exactly one:
 * vmsfs read, vmsfs write, or the establish-system ioctl.  Machine-greppable:
 *   PROBE: mounted /vms rc=<n>
 *   PROBE: sysuaf read <n> bytes            (vmsfs READ)
 *   PROBE: vmsfs mkdir rc=<n> errno=<e>     (vmsfs WRITE)
 *   PROBE: calling establish_system
 *   PROBE: establish_system returned st=<x> (the ioctl returned -- not hung)
 *   PROBE: getjpi_self st=<x> user=<s>
 *   PROBE: DONE
 * Whichever "PROBE:" line is LAST before silence names the stalling primitive.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

#include "vms_kif.h"

static int load_module(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long rc = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    return (rc != 0 && errno != EEXIST) ? -1 : 0;
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("PROBE: init up\n");
    load_module("/vms.ko");
    printf("PROBE: /dev/vms %s\n", access("/dev/vms", F_OK) == 0 ? "PRESENT" : "ABSENT");

    /* vms-165: SYS$DISK is reached through the vms.ko Files-11 ODS-2 ACP (over
     * /dev/vms), not a legacy `mount -t vmsfs`; the separate vmsfs.ko VFS driver
     * is gone. This dev probe's raw /vms/... reads below predate the ACP cutover
     * and are retained only as historical diagnostics. */

    /* (1a) vmsfs READ of SYSUAF */
    int fd = open("/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf));
        printf("PROBE: sysuaf read %zd bytes\n", n);
        close(fd);
    } else {
        printf("PROBE: sysuaf open failed errno=%d\n", errno);
    }

    /* (1b) vmsfs WRITE -- create a directory, the provision_home_directories
     * primitive.  mkdir on the vmsfs mount. */
    fflush(stdout);
    int drc = mkdir("/vms/SYS0/SYSCOMMON/SYSMGR/PROBE.DIR", 0755);
    printf("PROBE: vmsfs mkdir rc=%d errno=%d\n", drc, drc ? errno : 0);

    /* (1c) vmsfs WRITE -- create + write a file (the other write primitive) */
    fflush(stdout);
    int wfd = open("/vms/SYS0/SYSCOMMON/SYSMGR/PROBE.TMP", O_CREAT | O_WRONLY, 0644);
    if (wfd >= 0) {
        ssize_t w = write(wfd, "hello-alpha\n", 12);
        printf("PROBE: vmsfs write %zd bytes\n", w);
        close(wfd);
    } else {
        printf("PROBE: vmsfs create failed errno=%d\n", errno);
    }

    /* (1d) vmsfs SETATTR -- lchown, the own_object() primitive PROVISION runs
     * on every home directory (provision_home_directories -> own_object ->
     * lchown).  This is the one write primitive the first probe did not cover. */
    fflush(stdout);
    int crc = lchown("/vms/SYS0/SYSCOMMON/SYSMGR/PROBE.TMP", 4, 1);
    printf("PROBE: vmsfs lchown rc=%d errno=%d\n", crc, crc ? errno : 0);

    /* (1e) the actual home-directory primitive: mkdir under [USERS] + lchown,
     * exactly provision_home_directories()'s body for a real account. */
    fflush(stdout);
    int hrc = mkdir("/vms/USERS/PROBE", 0755);
    printf("PROBE: vmsfs home mkdir rc=%d errno=%d\n", hrc, hrc ? errno : 0);
    fflush(stdout);
    int hcrc = lchown("/vms/USERS/PROBE", 4, 1);
    printf("PROBE: vmsfs home lchown rc=%d errno=%d\n", hcrc, hcrc ? errno : 0);

    /* (2) establish_system ioctl */
    fflush(stdout);
    printf("PROBE: calling establish_system\n");
    fflush(stdout);
    uint32_t st = vms_kif_establish_system();
    printf("PROBE: establish_system returned st=%u (%s)\n",
           (unsigned)st, (st & 1) ? "success" : "FAIL");

    /* (3) getjpi self */
    fflush(stdout);
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t st2 = vms_kif_getjpi_self(&info);
    printf("PROBE: getjpi_self st=%u user=%s\n", (unsigned)st2, info.username);

    printf("PROBE: DONE\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
    return 0;
}
