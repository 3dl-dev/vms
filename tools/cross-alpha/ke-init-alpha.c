/*
 * ke-init-alpha.c -- PID 1 for the OVMX-on-Alpha kernel-executive TEST-SUITE
 * proof (rd vms-bc4, executive rung A4b: full Rule-7 coverage for the
 * tests/qemu/ raw-ioctl/kernel-interface tier).
 *
 * Boots as init under qemu-system-alpha (-M clipper = the DS10 compute stack:
 * 21264/EV6 + Tsunami + OSF/1 PALcode) on a Linux/Alpha kernel, loads the
 * cross-compiled vms.ko executive module, confirms /dev/vms
 * appears, attaches the fixtures the raw-ioctl suites need (a loop device
 * over a VMSFS test image, mirroring tests/qemu/init.sh's own convention),
 * then runs EVERY tests/qemu/test_kmod_*.c suite (all 29) -- the SAME
 * binaries the x86_64 Kernel Executive CI job runs -- against the REAL
 * /dev/vms. INV-6 / Rule 9: a facility is only proven when a second process
 * observes state the first wrote THROUGH the kernel.
 *
 * Freestanding of busybox on purpose: no alpha busybox is in the container,
 * so this static C init does the mounts, the module loads (finit_module),
 * the loop-device attach (LOOP_SET_FD), the fork/exec of every suite binary,
 * the PASS/FAIL tally, and the power-off itself.
 *
 * Per-suite verdict lines mirror tests/qemu/init.sh's own convention
 * ("=== SUITE <name> rc=<code> ===", "  PASS: ...", "  FAIL: ...") so a human
 * reading the transcript recognizes the shape immediately; the AGGREGATE
 * verdict this script's own caller (boot-vmsko-qemu-alpha.sh) asserts on is
 * the OVMX-ALPHA-KE: lines below, not init.sh's "FINAL RESULTS:" text (a
 * SEPARATE harness with its own SEPARATE verdict convention -- this one is
 * not consumed by tests/qemu/lib/harness_verdict.sh and does not need to be).
 *
 *   OVMX-ALPHA-KE: /dev/vms PRESENT
 *   OVMX-ALPHA-KE: SUITE <name> rc=<n>
 *   OVMX-ALPHA-KE: TOTAL suites=<n> passed=<n> failed=<n>
 *   OVMX-ALPHA-KE: ALL-PROVEN   (only if every suite exited 0)
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
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <linux/loop.h>
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

/* vms-165: the loop_attach() helper (which backed /dev/loop0 with the deleted
 * vmsfs_test.img fixture for the retired vmsfs VFS-driver suites) was removed
 * with those suites. The surviving test_kmod_* suites drive the executive
 * directly over /dev/vms and need no loop device. */

/*
 * Run a suite binary at /tests/<name>; capture its stdout through a pipe so
 * this init can count "  PASS:"/"  FAIL:" lines (mirroring init.sh's own
 * ASSERT_PASS/ASSERT_FAIL tally) while ALSO forwarding every byte to the
 * console verbatim (so the transcript carries full suite output, matching
 * every other OVMX QEMU harness). Returns the suite's exit code, or -1 if it
 * could not run.
 */
static int run_suite(const char *name, int *out_pass, int *out_fail)
{
    char path[128];
    snprintf(path, sizeof(path), "/tests/%s", name);

    int pfd[2];
    if (pipe(pfd) != 0) { printf("OVMX-ALPHA-KE: pipe failed for %s\n", name); return -1; }

    pid_t pid = fork();
    if (pid < 0) { printf("OVMX-ALPHA-KE: fork failed for %s\n", name); close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[1]);
        char *argv[] = { path, NULL };
        char *envp[] = { NULL };
        execve(path, argv, envp);
        printf("OVMX-ALPHA-KE: execve %s failed: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(pfd[1]);

    /* Drain the pipe line-by-line: echo to console, tally PASS/FAIL. */
    char buf[4096];
    size_t buflen = 0;
    ssize_t n;
    while ((n = read(pfd[0], buf + buflen, sizeof(buf) - 1 - buflen)) > 0) {
        buflen += (size_t)n;
        buf[buflen] = '\0';
        char *start = buf, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            printf("%s\n", start);
            if (!strncmp(start, "  PASS:", 7)) (*out_pass)++;
            else if (!strncmp(start, "  FAIL:", 7)) (*out_fail)++;
            start = nl + 1;
        }
        size_t rem = buflen - (size_t)(start - buf);
        memmove(buf, start, rem);
        buflen = rem;
    }
    if (buflen > 0) { buf[buflen] = '\0'; printf("%s\n", buf); }
    close(pfd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Every tests/qemu/test_kmod_*.c suite -- all 29, the full raw-ioctl/
 * kernel-interface tier -- this rung (vms-bc4) ported to alpha. */
static const char *const kSuites[] = {
    "test_kmod_access",
    "test_kmod_ast",
    "test_kmod_bind",
    "test_kmod_devtab",
    "test_kmod_disk",
    "test_kmod_eflag",
    "test_kmod_eflag_mproc",
    "test_kmod_ident",
    "test_kmod_lock",
    "test_kmod_lock_mproc",
    "test_kmod_lock_sync",
    "test_kmod_mbx",
    "test_kmod_modeswitch",
    "test_kmod_p0",
    "test_kmod_p1",
    "test_kmod_pin",
    "test_kmod_procacct",
    "test_kmod_procnam",
    "test_kmod_resdir",
    "test_kmod_rundown",
    "test_kmod_setterm",
    /* vms-165 retired the vmsfs VFS driver + its test suites
     * (test_kmod_vmsfs* / test_kmod_ods2_codec); the Files-11 ODS-2 ACP now
     * lives in vms.ko and is covered by the test_syssvc_* / ACP suites. */
};
#define N_SUITES (sizeof(kSuites) / sizeof(kSuites[0]))

int main(void)
{
    /* Kernel opened fd 0/1/2 on the console before exec'ing us, so stdio works.*/
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    mkdir("/test_data", 0755);
    mkdir("/mnt", 0755);

    printf("OVMX-ALPHA-KE: init up, loading executive\n");

    if (load_module("/vms.ko") != 0) {
        printf("OVMX-ALPHA-KE: FATAL vms.ko load failed\n");
        goto out;
    }
    /* vms-165: the Files-11 ODS-2 ACP is inside vms.ko now; there is no separate
     * vmsfs.ko VFS module to load. */

    if (access("/dev/vms", F_OK) == 0) {
        printf("OVMX-ALPHA-KE: /dev/vms PRESENT\n");
    } else {
        printf("OVMX-ALPHA-KE: /dev/vms ABSENT -- executive did not register\n");
        goto out;
    }

    /* vms-165 retired the vmsfs VFS driver and its mkimage_vmsfs / vmsfs_test.img
     * loop0 fixture; the remaining test_kmod_* suites exercise the executive
     * directly over /dev/vms and stage no loop device here. */

    int suites_pass = 0, suites_fail = 0;
    int assert_pass = 0, assert_fail = 0;
    for (size_t i = 0; i < N_SUITES; i++) {
        printf("OVMX-ALPHA-KE: === running %s ===\n", kSuites[i]);
        int p = 0, f = 0;
        int rc = run_suite(kSuites[i], &p, &f);
        assert_pass += p;
        assert_fail += f;
        printf("OVMX-ALPHA-KE: SUITE %s rc=%d (pass=%d fail=%d)\n", kSuites[i], rc, p, f);
        if (rc == 0 && f == 0) suites_pass++; else suites_fail++;
    }

    printf("OVMX-ALPHA-KE: TOTAL suites=%zu passed=%d failed=%d assertions_pass=%d assertions_fail=%d\n",
           N_SUITES, suites_pass, suites_fail, assert_pass, assert_fail);

    if (suites_fail == 0 && assert_fail == 0)
        printf("OVMX-ALPHA-KE: ALL-PROVEN\n");
    else
        printf("OVMX-ALPHA-KE: NOT-PROVEN (suites_failed=%d assertions_failed=%d)\n", suites_fail, assert_fail);

out:
    printf("OVMX-ALPHA-KE: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    /* If power-off returns, spin so the kernel does not panic on init exit. */
    for (;;) pause();
    return 0;
}
