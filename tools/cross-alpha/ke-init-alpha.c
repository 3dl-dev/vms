/*
 * ke-init-alpha.c -- PID 1 for the OVMX-on-Alpha kernel-executive proof
 * (rd vms-89dd, executive rung A4).
 *
 * Boots as init under qemu-system-alpha (-M clipper = the DS10 compute stack:
 * 21264/EV6 + Tsunami + OSF/1 PALcode) on a Linux/Alpha kernel, loads the
 * cross-compiled vms.ko executive, confirms /dev/vms appears, then runs the
 * SAME cross-process (A-writes/B-reads) executive suites the x86_64 Kernel
 * Executive CI job runs -- test_kmod_eflag_mproc and test_kmod_lock_mproc --
 * against the REAL /dev/vms. INV-6 / Rule 9: a facility is only proven when a
 * second process observes state the first wrote THROUGH the kernel.
 *
 * Freestanding of busybox on purpose: no alpha busybox is in the container, so
 * this static C init does the three mounts, the module load (finit_module), the
 * fork/exec of the suite binaries, and the power-off itself.
 *
 * Emits machine-greppable verdict lines the boot script asserts:
 *   OVMX-ALPHA-KE: /dev/vms PRESENT
 *   OVMX-ALPHA-KE: eflag_mproc rc=<n>
 *   OVMX-ALPHA-KE: lock_mproc rc=<n>
 *   OVMX-ALPHA-KE: ALL-PROVEN   (only if both suites returned 0)
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

static int load_module(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("OVMX-ALPHA-KE: open %s failed: %s\n", path, strerror(errno)); return -1; }
    long rc = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    if (rc != 0) { printf("OVMX-ALPHA-KE: finit_module %s failed: %s\n", path, strerror(errno)); return -1; }
    printf("OVMX-ALPHA-KE: loaded %s\n", path);
    return 0;
}

/* Run a suite binary; return its exit code (or -1 if it could not run). */
static int run_suite(const char *path)
{
    pid_t pid = fork();
    if (pid < 0) { printf("OVMX-ALPHA-KE: fork failed for %s\n", path); return -1; }
    if (pid == 0) {
        char *argv[] = { (char *)path, NULL };
        char *envp[] = { NULL };
        execve(path, argv, envp);
        printf("OVMX-ALPHA-KE: execve %s failed: %s\n", path, strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int main(void)
{
    /* Kernel opened fd 0/1/2 on the console before exec'ing us, so stdio works.*/
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("OVMX-ALPHA-KE: init up, loading executive\n");

    if (load_module("/vms.ko") != 0) {
        printf("OVMX-ALPHA-KE: FATAL vms.ko load failed\n");
        goto out;
    }
    /* vmsfs.ko is the ACP-bearing FS module; load it too so its init path is
     * exercised on alpha (it is not required for the eflag/lock proof). */
    load_module("/vmsfs.ko");

    if (access("/dev/vms", F_OK) == 0) {
        printf("OVMX-ALPHA-KE: /dev/vms PRESENT\n");
    } else {
        printf("OVMX-ALPHA-KE: /dev/vms ABSENT -- executive did not register\n");
        goto out;
    }

    int rc_ef = run_suite("/test_eflag");
    printf("OVMX-ALPHA-KE: eflag_mproc rc=%d\n", rc_ef);
    int rc_lk = run_suite("/test_lock");
    printf("OVMX-ALPHA-KE: lock_mproc rc=%d\n", rc_lk);

    if (rc_ef == 0 && rc_lk == 0)
        printf("OVMX-ALPHA-KE: ALL-PROVEN\n");
    else
        printf("OVMX-ALPHA-KE: NOT-PROVEN (eflag=%d lock=%d; 0=pass 77=skip)\n", rc_ef, rc_lk);

out:
    printf("OVMX-ALPHA-KE: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    /* If power-off returns, spin so the kernel does not panic on init exit. */
    for (;;) pause();
    return 0;
}
