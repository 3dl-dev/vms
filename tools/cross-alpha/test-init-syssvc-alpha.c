/*
 * test-init-syssvc-alpha.c -- PID 1 for the OVMX-on-Alpha USERSPACE syssvc
 * test-suite proof (rd vms-341, epic vms-8954). The Alpha analog of
 * tests/qemu/init.sh's suite loop, but a static C init (no busybox on Alpha --
 * same reason ke-init-alpha.c is C, tools/cross-alpha/ke-init-alpha.c).
 *
 * Boots as init under qemu-system-alpha -M clipper. Loads the cross-built
 * vms.ko executive module (which enumerates the attached virtio
 * disks into DKA0:/DKA100:/DKA200:/DKA300:/DKA400: -- the ODS-2 fixtures the
 * harness attaches, mirroring tests/qemu/run_tests.sh's vda..vde mapping),
 * confirms /dev/vms, then fork/exec's EVERY /tests/test_syssvc_* and
 * /tests/test_imgact_* binary (the `qemu_syssvc_tests` target, cross-built
 * EM_ALPHA), counting each suite's own "  PASS:" / "  FAIL:" lines (a CHECK()
 * macro in the C sources prints them; this init counts by grep, never trusting
 * the suite's self-report -- same discipline as init.sh).
 *
 * OUTPUT CONTRACT (so tests/qemu/lib/harness_verdict.sh works VERBATIM on the
 * captured serial log): the final summary is exactly
 *     === FINAL RESULTS: <N> suites passed, <M> suites failed ===
 * harness_verdict.sh greps FINAL RESULTS:.*[^0-9]0 suites failed for green.
 * Per-suite lines "=== SUITE <name> rc=<code> ===" mirror init.sh.
 *
 * INV-6 / Rule 9: a facility is proven only when a real second process reaches
 * the executive over /dev/vms. These are separate userspace processes hitting
 * the SAME /dev/vms + the SAME ODS-2 volumes over the ACP -- not the init's own
 * calls. The init never fakes a result: a missing binary or a nonzero exit is a
 * genuine suite failure, never a skip.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <signal.h>

/* The production ACP $MOUNT (src/ovmx_init/ovmx_boot_linux.c's
 * ovmx_boot_acp_mount_system_disk calls vms_kif_acp_mount on boot). Linked in
 * from libvmssys (vms_kif.c + kif_transport_linux.c + vms_string.c +
 * arch/alpha/syscall.S) so this runner-init mounts the sysdevice the SAME way
 * production does -- no hand-rolled ioctl, no ABI-drift copy of the 24-byte
 * mount arg struct. vms_kif.h pulls ../kernel/vms_ioctl.h -> vms_acp.h. */
#include "vms_kif.h"

/* Per-suite wall bound: a suite that hangs (e.g. an exec'd DCL waiting on
 * input) must not eat the whole boot budget -- SIGALRM kills the child, the
 * pipe closes, and the suite is recorded as a genuine failure (never a skip).*/
#ifndef SUITE_TIMEOUT_SECS
#define SUITE_TIMEOUT_SECS 150
#endif
static volatile pid_t g_child = 0;
static void on_alrm(int sig) { (void)sig; if (g_child > 0) kill(g_child, SIGKILL); }

/* finit_module(2) -- load a .ko by fd (no libkmod on the static init). */
static int load_module(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { printf("SYSSVC-ALPHA: open %s failed: %s\n", path, strerror(errno)); return -1; }
    int rc = (int)syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    if (rc != 0) { printf("SYSSVC-ALPHA: finit_module %s failed: %s\n", path, strerror(errno)); return -1; }
    printf("SYSSVC-ALPHA: loaded %s\n", path);
    return 0;
}

/*
 * Run one suite binary at /tests/<name>; capture its stdout through a pipe so
 * this init can count "  PASS:"/"  FAIL:" lines (mirroring init.sh's own
 * ASSERT tally) while ALSO forwarding every byte to the console. Returns the
 * child's exit code (or -1 on a harness error).
 */
static int run_suite(const char *name, int *out_pass, int *out_fail)
{
    char path[512];
    int is_seam = (strcmp(name, "activate_seam") == 0);
    if (is_seam)
        /* vms-341/vms-f60d option (c): the seam subject is exec'd from the
         * boot-stage tmpfs path; IMGACT's map_staged remaps it to the DKA300
         * SYSEXE ODS-2 path and re-reads the genuine volume bytes over the ACP. */
        snprintf(path, sizeof(path), "/run/ovmx-boot/ACTIVATE.EXE");
    else
        snprintf(path, sizeof(path), "/tests/%s", name);

    int pfd[2];
    if (pipe(pfd) != 0) { printf("SYSSVC-ALPHA: pipe failed for %s\n", name); return -1; }

    pid_t pid = fork();
    if (pid < 0) { printf("SYSSVC-ALPHA: fork failed for %s\n", name); close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]); close(pfd[1]);
        char *argv[] = { path, NULL };
        /* The subject-image activation seam (vms-341/vms-f60d): exec'ing the
         * VMS-std subject triggers IMGACT.EXE via PT_INTERP; OVMX_IMGACT_SEAM=1
         * enables IMGACT.EXE's test-mode OVMX-SEAM print (getexit(SEL_SELF) ->
         * the executive-recorded $STATUS). Gated to this subject only, so no
         * other suite's IMGACT activations emit the seam line. */
        char *envp_plain[] = { (char *)"PATH=/tests:/bin", (char *)"HOME=/", NULL };
        char *envp_seam[]  = { (char *)"PATH=/tests:/bin", (char *)"HOME=/",
                               (char *)"OVMX_IMGACT_SEAM=1",
                               /* option (c): resolve the subject over the ACP on
                                * the writable DKA300 sysvol (default DKA0: is the
                                * immutable clean-room disk). */
                               (char *)"OVMX_SYSDEVICE=DKA300:", NULL };
        char **envp = is_seam ? envp_seam : envp_plain;
        execve(path, argv, envp);
        printf("SYSSVC-ALPHA: execve %s failed: %s\n", path, strerror(errno));
        _exit(127);
    }
    close(pfd[1]);

    /* Arm the per-suite watchdog: SIGALRM kills a hung child so the drain
     * loop below sees EOF and this suite is scored a failure, not a hang. */
    g_child = pid;
    signal(SIGALRM, on_alrm);
    alarm(SUITE_TIMEOUT_SECS);

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
        size_t rem = (size_t)(buf + buflen - start);
        memmove(buf, start, rem);
        buflen = rem;
    }
    if (buflen > 0) { buf[buflen] = '\0'; printf("%s\n", buf); }
    close(pfd[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { alarm(0); g_child = 0; return -1; }
    alarm(0);
    g_child = 0;
    if (WIFSIGNALED(status)) {
        printf("  FAIL: suite %s killed by signal %d (per-suite %ds watchdog or crash)\n",
               name, WTERMSIG(status), SUITE_TIMEOUT_SECS);
        (*out_fail)++;
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * $MOUNT an ODS-2 volume through the executive Files-11 (ODS-2) ACP -- the
 * EXACT production mount (vms_kif_acp_mount, the same entry point
 * ovmx_boot_acp_mount_system_disk drives on a real boot). It honors the mount
 * protocol guard (acp_bind_ok: open /dev/vms + register the PCB) internally, so
 * this init does not hand-roll register/ioctl. Records the unit executive-global
 * (cross-process), so the seam subject's genuine ACTIVATE.EXE bytes on DKA300
 * resolve over the ACP: without this, imgact_acp_open's $ASSIGN hits an
 * unmounted unit (SS$_DEVNOTMOUNT) and the subject fails IMGNOTFND. Returns 0 on
 * an odd (success) VMS status, -1 otherwise. Non-fatal by design: on failure the
 * seam suite fails HONESTLY (IMGNOTFND), never skipped (INV-6). */
static int mount_acp_sysdevice(const char *unit)
{
    uint32_t st = vms_kif_acp_mount(unit);
    printf("SYSSVC-ALPHA: $MOUNT %s -> status=0x%08x (%s)\n",
           unit, (unsigned)st, (st & 1u) ? "MOUNTED" : "FAILED");
    return (st & 1u) ? 0 : -1;
}

/* qsort comparator for stable, deterministic suite ordering. */
static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void)
{
    /* Kernel opened fd 0/1/2 on the console before exec'ing us, so stdio works. */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    mkdir("/tmp", 01777);

    printf("SYSSVC-ALPHA: init up, loading executive\n");

    if (load_module("/vms.ko") != 0) {
        printf("SYSSVC-ALPHA: FATAL vms.ko load failed\n");
        printf("=== FINAL RESULTS: 0 suites passed, 1 suites failed ===\n");
        goto out;
    }
    /* vms-165: the Files-11 ODS-2 ACP is inside vms.ko; no separate vmsfs.ko. */

    if (access("/dev/vms", F_OK) == 0) {
        printf("SYSSVC-ALPHA: /dev/vms PRESENT\n");
    } else {
        printf("SYSSVC-ALPHA: /dev/vms ABSENT -- executive did not register\n");
        printf("=== FINAL RESULTS: 0 suites passed, 1 suites failed ===\n");
        goto out;
    }

    /* vms-341/vms-f60d option (c): the subject-image activation seam resolves
     * ACTIVATE.EXE's GENUINE bytes off the DKA300 sysvol over the ACP. Production
     * $MOUNTs the sysdevice on boot (ovmx_boot_acp_mount_system_disk); mirror
     * that here BEFORE the seam suite runs so imgact_acp_open's $ASSIGN reaches a
     * MOUNTED unit instead of SS$_DEVNOTMOUNT. Gated to the seam run only -- the
     * normal syssvc suites $MOUNT the volumes they need themselves, so the plain
     * run is unaffected. Non-fatal: on failure the seam suite fails honestly. */
    if (access("/run/ovmx-boot/ACTIVATE.EXE", F_OK) == 0)
        mount_acp_sysdevice("DKA300:");

    /* Discover every /tests/test_syssvc_* and /tests/test_imgact_* binary
     * (the qemu_syssvc_tests target), sorted for deterministic order. The
     * suites $ASSIGN the ODS-2 units the harness attached as virtio disks;
     * the executive enumerated them into DKA0:/100:/200:/300:/400: at insmod,
     * so no per-disk setup is needed here (unlike the kmod loop harness). */
    DIR *d = opendir("/tests");
    if (!d) {
        printf("SYSSVC-ALPHA: /tests missing: %s\n", strerror(errno));
        printf("=== FINAL RESULTS: 0 suites passed, 1 suites failed ===\n");
        goto out;
    }
    char *names[512];
    size_t n_names = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n_names < 512) {
        if (strncmp(de->d_name, "test_syssvc_", 12) != 0 &&
            strncmp(de->d_name, "test_imgact_", 12) != 0 &&
            strncmp(de->d_name, "test_arith_", 11) != 0)    /* vms-db3: Alpha-only arith-trap suites */
            continue;
        names[n_names++] = strdup(de->d_name);
    }
    closedir(d);

    /* vms-341/vms-f60d option (c): the seam subject is NOT a /tests binary -- its
     * genuine bytes live on the DKA300 ODS-2 volume, with a boot-stage tmpfs copy
     * at /run/ovmx-boot/ACTIVATE.EXE. Run it as the "activate_seam" suite when
     * that copy is present (SUBJECT_IMAGE seam mode); run_suite execs the tmpfs
     * copy so IMGACT map_staged resolves the genuine bytes off DKA300 (the ACP). */
    if (access("/run/ovmx-boot/ACTIVATE.EXE", F_OK) == 0 && n_names < 512)
        names[n_names++] = strdup("activate_seam");

    qsort(names, n_names, sizeof(names[0]), cmpstr);

    if (n_names == 0) {
        printf("SYSSVC-ALPHA: no test_syssvc_/test_imgact_ binaries staged in /tests\n");
        printf("=== FINAL RESULTS: 0 suites passed, 1 suites failed ===\n");
        goto out;
    }
    printf("SYSSVC-ALPHA: discovered %zu suites in /tests\n", n_names);

    int suites_pass = 0, suites_fail = 0;
    int assert_pass = 0, assert_fail = 0;
    for (size_t i = 0; i < n_names; i++) {
        printf("SYSSVC-ALPHA: === running %s ===\n", names[i]);
        int p = 0, f = 0;
        int rc = run_suite(names[i], &p, &f);
        assert_pass += p;
        assert_fail += f;
        printf("=== SUITE %s rc=%d (pass=%d fail=%d) ===\n", names[i], rc, p, f);
        if (rc == 0 && f == 0) suites_pass++; else suites_fail++;
    }

    printf("=== ASSERTIONS: %d passed, %d failed ===\n", assert_pass, assert_fail);
    printf("=== FINAL RESULTS: %d suites passed, %d suites failed ===\n",
           suites_pass, suites_fail);

out:
    printf("SYSSVC-ALPHA: powering off\n");
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) pause();
    return 0;
}
