/*
 * alpha-contention-probe.c -- pinpoint WHICH executive/vmsfs primitive
 * deadlocks on Alpha when a SECOND process concurrently holds a /dev/vms
 * executive attachment (rd vms-989, rung A5a diagnostic).
 *
 * Findings so far:
 *   - alpha-provision-probe: as a lone PID-1 process, every primitive
 *     PROVISION uses succeeds (vmsfs read/write/mkdir/lchown, establish_system,
 *     getjpi).
 *   - alpha-exec-provision-init: exec'ing the REAL PROVISION.EXE as a child of
 *     an init that HOLDS a /dev/vms attachment reproduces the boot stall
 *     (PROVISION hangs after exec).
 * The differentiator is the concurrent attachment.  This probe reproduces
 * exactly that shape and reports which primitive hangs:
 *   PARENT opens + HOLDS /dev/vms, then forks; the CHILD runs the primitive
 *   sequence with a print before each.  The child's LAST printed line before
 *   silence names the primitive that deadlocks under a concurrent attachment.
 *
 * Greppable (child lines prefixed CHILD:):
 *   HOLD: parent holding /dev/vms fd=<n>
 *   CHILD: mkdir rc=<n> / lchown rc=<n> / write rc=<n>
 *   CHILD: calling establish_system
 *   CHILD: establish_system st=<x>
 *   CHILD: getjpi st=<x> user=<s>
 *   CHILD: DONE
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/wait.h>
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

static void child_run(void)
{
    /* A fresh /dev/vms attachment for the child is opened lazily by the kif on
     * the first KIF_CALL, exactly as PROVISION gets its own. */
    printf("CHILD: start\n"); fflush(stdout);

    int drc = mkdir("/vms/USERS/CONTEND", 0755);
    printf("CHILD: mkdir rc=%d errno=%d\n", drc, drc ? errno : 0); fflush(stdout);

    int crc = lchown("/vms/USERS/CONTEND", 4, 1);
    printf("CHILD: lchown rc=%d errno=%d\n", crc, crc ? errno : 0); fflush(stdout);

    printf("CHILD: calling establish_system\n"); fflush(stdout);
    uint32_t st = vms_kif_establish_system();
    printf("CHILD: establish_system st=%u (%s)\n", (unsigned)st, (st & 1) ? "ok" : "FAIL"); fflush(stdout);

    struct vms_procinfo info; memset(&info, 0, sizeof(info));
    uint32_t st2 = vms_kif_getjpi_self(&info);
    printf("CHILD: getjpi st=%u user=%s\n", (unsigned)st2, info.username); fflush(stdout);

    printf("CHILD: DONE\n"); fflush(stdout);
    _exit(0);
}

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    load_module("/vms.ko");
    load_module("/vmsfs.ko");
    int mrc = mount("/dev/vda", "/vms", "vmsfs", 0, NULL);
    printf("HOLD: mounted rc=%d\n", mrc);

    /* Parent holds a /dev/vms executive attachment across the child's run --
     * the concurrent attachment that the lone-process probe lacked. */
    int hold = open("/dev/vms", O_RDWR);
    printf("HOLD: parent holding /dev/vms fd=%d\n", hold); fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) child_run();

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        printf("HOLD: child exited status=%d\n", WEXITSTATUS(status));

    printf("HOLD: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
    return 0;
}
