/*
 * alpha-exec-provision-init.c -- definitive reproduction of the Alpha boot
 * frontier stall (rd vms-989, rung A5a diagnostic).
 *
 * Every primitive PROVISION.EXE uses (vmsfs read/write/mkdir/lchown,
 * establish_system, getjpi) succeeds standalone on Alpha (alpha-provision-probe).
 * Yet the real boot stalls after %STDRV-I-STARTUP, i.e. inside PROVISION.EXE
 * when STARTUP.EXE fork+execl's it.  This init reproduces STARTUP's exact
 * pre-PROVISION context minimally:
 *   1. mount proc/sys/dev, load vms.ko (the Files-11 ODS-2 ACP is inside it,
 *      vms-165 -- no separate vmsfs.ko / vmsfs mount);
 *   2. HOLD an executive attachment (open /dev/vms, keep the fd) -- what
 *      STARTUP's executive_attach() does before it forks;
 *   3. fork + execl the REAL SYS$SYSTEM:PROVISION.EXE off the mounted disk,
 *      exactly as run_startup() does;
 *   4. wait up to 60s (SIGALRM) and report whether PROVISION printed its
 *      identity line, exited, or hung.
 *
 * If PROVISION.EXE hangs here too -> the stall is in the PROVISION.EXE binary
 * itself in the fork+exec-from-init context (independent of the rest of
 * STARTUP).  If it completes here -> the stall is something else in STARTUP's
 * fuller state.  Either way the boundary is pinned honestly (INV-6: no fake).
 *
 * Greppable:
 *   XPROV: init up / modules / mounted rc=<n> / holding /dev/vms fd=<n>
 *   XPROV: exec PROVISION.EXE
 *   (PROVISION's own %OVMX-I-EXEC ... established line appears here IF it runs)
 *   XPROV: PROVISION exited status=<n>   OR   XPROV: PROVISION HUNG (60s)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

static int load_module(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long rc = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    return (rc != 0 && errno != EEXIST) ? -1 : 0;
}

static volatile int timed_out = 0;
static void on_alarm(int sig) { (void)sig; timed_out = 1; }

int main(void)
{
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("XPROV: init up\n");
    load_module("/vms.ko");
    printf("XPROV: module; /dev/vms %s\n", access("/dev/vms", F_OK) == 0 ? "PRESENT" : "ABSENT");

    /* vms-165: SYS$DISK is reached through the vms.ko Files-11 ODS-2 ACP (over
     * /dev/vms), not a legacy `mount -t vmsfs`; the vmsfs.ko VFS driver is gone.
     * The /vms/... reads below predate the ACP cutover (historical diagnostic). */

    /* Mirror STARTUP's executive_attach(): hold an executive attachment open
     * across the fork/exec of PROVISION. */
    int exec_fd = open("/dev/vms", O_RDWR | O_CLOEXEC);
    printf("XPROV: holding /dev/vms fd=%d\n", exec_fd);
    fflush(stdout);

    const char *prov = "/vms/SYS0/SYSCOMMON/SYSEXE/PROVISION.EXE";
    printf("XPROV: exec %s\n", prov);
    fflush(stdout);

    signal(SIGALRM, on_alarm);
    pid_t pid = fork();
    if (pid == 0) {
        execl(prov, "PROVISION", (char *)NULL);
        printf("XPROV: execl failed: %s\n", strerror(errno));
        _exit(127);
    }
    alarm(60);
    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    if (w < 0 && timed_out) {
        printf("XPROV: PROVISION HUNG (60s) -- reproduces the Alpha boot frontier stall\n");
        kill(pid, SIGKILL);
    } else if (w == pid) {
        if (WIFEXITED(status))
            printf("XPROV: PROVISION exited status=%d\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            printf("XPROV: PROVISION killed by signal %d\n", WTERMSIG(status));
    }

    printf("XPROV: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
    return 0;
}
