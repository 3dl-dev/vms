/*
 * test_kmod_vmsfs_mountvis.c - a device MOUNTed by one process is resolvable
 * and readable by a SEPARATE process, proven by driving the shipped DCL.EXE
 * against a real vmsfs mount (vms-8b6).
 *
 * THE BUG THIS GATES. On OpenVMS a mounted volume is EXECUTIVE / system-wide
 * state: any process on the node addresses the unit. OVMX's VMS-device -> Linux
 * mount-point map (src/vmsfs/vmsfs_device.c) was a PER-PROCESS static table.
 * MOUNT (src/vmsdcl/dcl_cmd_misc.c) does a real mount(2) of the unit at
 * /mnt/<devnam> AND records the mapping -- but only in the MOUNTing process's
 * copy of that table. A forked/exec'd child (AUTHORIZE.EXE, SYSGEN.EXE, any
 * RUN'd image) starts with a fresh table and so could NOT resolve a device its
 * DCL parent had mounted: the faithful install (vms-718) MOUNTs the target
 * device then RUNs AUTHORIZE against SYSUAF on it, and AUTHORIZE reported
 * %UAF-E-NOSUCHUSER even though the volume was genuinely mounted. That is the
 * exact INV-6 defect class (a per-process view of an executive facility);
 * vms-8b6 makes vmsfs_device_resolve() consult the kernel's own mount table
 * (/proc/mounts) on a per-process miss, so every process sees the same mount.
 *
 * WHY DRIVING DCL.EXE IS THE PROOF, and why fork() alone is not. fork() COPIES
 * the parent's memory, including that static device table, so a forked child
 * would wrongly "see" the mapping. The real callers fork()+execve() -- execve
 * REPLACES the image, resetting the table to only the bootstrap DKA0:. So this
 * test mounts a vmsfs volume at /mnt/dka100 in ITS OWN process (never telling
 * DCL), then execs the shipped DCL.EXE -- a genuinely separate process with a
 * fresh table, exactly like the AUTHORIZE.EXE the bead is about -- and asks it
 * to READ a file on DKA100:. Before the fix DCL cannot resolve DKA100: (it
 * falls back to the system disk, where the file does not exist); after it, DCL
 * resolves DKA100: through /proc/mounts, the same shared kernel truth the mount
 * lives in, and reads the file.
 *
 * WHY A BLOCK-DEVICE vmsfs, NOT A BACKING-DIRECTORY ONE. The proof has to be a
 * real cross-process FILE read (that is what AUTHORIZE does to SYSUAF), and the
 * content read has to be a value only the mounted volume carries -- so a main-
 * build fallback to the system disk cannot accidentally satisfy it. This uses
 * the harness's loop-mounted vmsfs image (/dev/loop0, /test_data/vmsfs_test.img,
 * the same one test_kmod_vmsfs_blkdev drives), whose HELLO.TXT;1 holds a fixed,
 * distinctive string. (A backing-directory vmsfs mount cannot be used here: on
 * this kernel its readdir/lookup does not terminate for a process other than
 * the mounter -- an orthogonal vmsfs.ko defect, filed separately -- so any
 * cross-process open of a child name on such a mount hangs. The block-device
 * on-disk directory format has no such issue: test_kmod_vmsfs_blkdev exercises
 * its readdir and version-less lookup and they terminate.)
 *
 * ASSERTIONS
 *   1. DCL TYPE DKA100:[000000]HELLO.TXT;1 -- DCL, a separate process, resolves
 *      DKA100: (mounted by THIS process, never registered in any table DCL
 *      inherited) and prints the volume's OWN HELLO.TXT content. That exact
 *      string exists nowhere the main-build fallback (the system disk) could
 *      supply it, so its presence is a true differential -- RED on origin/main.
 *   2. NEGATIVE CONTROL: DCL TYPE DKA200:[000000]HELLO.TXT;1 against a unit
 *      nothing mounted must NOT print that content -- the fix resolves genuinely
 *      mounted units through the kernel mount table; it does not fabricate a
 *      mount for an absent one (SS$_NOSUCHDEV honestly, INV-6).
 *
 * Needs vmsfs.ko + the loop-mounted image (for the mount(2)) and the shipped
 * /tests/DCL.EXE; it does NOT need /dev/vms (the fix reads the kernel mount
 * table, not the executive), so it passes in both the normal and the
 * NEGATIVE_CONTROL rigs -- which is why it is named test_kmod_vmsfs_*, the one
 * ci.yml class whose verdict is rc=0 with or without an insmod'd vms.ko.
 *
 * Statically linked, runs inside the minimal BusyBox initramfs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

#define DCL_PATH_DEFAULT "/tests/DCL.EXE"
#define DCL_TIMEOUT_MS   20000
#define LOOP_DEV         "/dev/loop0"
#define MOUNTPT          "/mnt/dka100"
/* HELLO.TXT;1's content in tests/qemu/mkimage_vmsfs.c (== EXPECTED_CONTENT in
 * test_kmod_vmsfs_blkdev.c). A distinctive string the system disk never holds. */
#define HELLO_CONTENT    "Hello from VMSFS block device!"

static const char *dcl_path(void)
{
    const char *p = getenv("OVMX_DCL");
    return (p && p[0]) ? p : DCL_PATH_DEFAULT;
}

/*
 * run_dcl - feed one DCL command line to the shipped DCL.EXE (a SEPARATE
 * process) and capture stdout+stderr merged into `out`. Returns 0 on success,
 * -1 if DCL could not be run or did not finish in time. Streams are merged
 * deliberately: TYPE prints content to stdout and RMS errors to stderr, and
 * this test wants to see either. Modeled on the run_dcl harness in
 * tests/qemu/test_syssvc_showdev.c.
 */
static int run_dcl(const char *cmdline, char *out, size_t outsz)
{
    char script[]  = "/tmp/mountvis_in.XXXXXX";
    char capture[] = "/tmp/mountvis_out.XXXXXX";
    char text[512];
    int sfd, cfd, wstatus, len;
    pid_t pid;
    ssize_t n;

    out[0] = '\0';

    sfd = mkstemp(script);
    if (sfd < 0)
        return -1;
    /* A liveness marker so an empty capture is never mistaken for "the command
     * printed nothing". */
    len = snprintf(text, sizeof(text),
                   "%s\nWRITE SYS$OUTPUT \"OVMX-PROBE-ALIVE\"\n", cmdline);
    if (len < 0 || write(sfd, text, (size_t)len) != len) {
        close(sfd); unlink(script); return -1;
    }
    lseek(sfd, 0, SEEK_SET);

    cfd = mkstemp(capture);
    if (cfd < 0) {
        close(sfd); unlink(script); return -1;
    }

    pid = fork();
    if (pid == 0) {
        dup2(sfd, 0);
        dup2(cfd, 1);
        dup2(cfd, 2);
        execl(dcl_path(), dcl_path(), (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        close(sfd); close(cfd);
        unlink(script); unlink(capture);
        return -1;
    }

    /* Bounded wait: a hung DCL becomes a NAMED failure, not a VM-wide timeout. */
    for (int waited = 0; waited < DCL_TIMEOUT_MS; waited += 20) {
        pid_t r = waitpid(pid, &wstatus, WNOHANG);
        if (r == pid)
            goto reaped;
        if (r < 0)
            break;
        usleep(20000);
    }
    kill(pid, 9);
    waitpid(pid, &wstatus, 0);
    lseek(cfd, 0, SEEK_SET);
    n = read(cfd, out, outsz - 1);
    out[(n > 0) ? (size_t)n : 0] = '\0';
    close(sfd); close(cfd);
    unlink(script); unlink(capture);
    printf("  INFO: DCL timed out after %d ms; partial capture follows:\n"
           "----8<----\n%s----8<----\n", DCL_TIMEOUT_MS, out);
    return -1;

reaped:
    lseek(cfd, 0, SEEK_SET);
    n = read(cfd, out, outsz - 1);
    out[(n > 0) ? (size_t)n : 0] = '\0';
    close(sfd); close(cfd);
    unlink(script); unlink(capture);
    return 0;
}

static void show_capture(const char *label, const char *out)
{
    printf("  INFO: %s:\n----8<----\n%s----8<----\n", label, out);
}

int main(void)
{
    /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into
     * a child process's output. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    char out[8192];
    struct stat st;

    printf("=== test_kmod_vmsfs_mountvis (a mounted unit resolves cross-process, vms-8b6) ===\n");

    /* 0. Preconditions: vmsfs registered, the loop image is set up (init.sh),
     *    and DCL.EXE is staged. */
    {
        FILE *f = fopen("/proc/filesystems", "r");
        int found = 0;
        char buf[256];
        if (f) {
            while (fgets(buf, sizeof(buf), f))
                if (strstr(buf, "vmsfs")) { found = 1; break; }
            fclose(f);
        }
        CHECK(found, "vmsfs registered in /proc/filesystems");
        if (!found) goto done_fatal;
    }
    if (stat(LOOP_DEV, &st) != 0) {
        printf("  FAIL: %s absent -- init.sh did not set up the loop image this suite mounts\n", LOOP_DEV);
        fail++;
        goto done_fatal;
    }
    if (stat(dcl_path(), &st) != 0) {
        printf("  FAIL: %s is not staged in the initramfs -- cannot drive the cross-process resolver\n",
               dcl_path());
        fail++;
        goto done_fatal;
    }

    /* 1. THIS process mounts the vmsfs volume at /mnt/dka100 -- read-only (the
     *    image is shared with test_kmod_vmsfs_blkdev) -- and never registers it
     *    in any userspace device table DCL could inherit. /mnt/dka100 ==
     *    mount_point_for_device("DKA100"). */
    mkdir(MOUNTPT, 0755);
    if (mount(LOOP_DEV, MOUNTPT, "vmsfs", MS_RDONLY, NULL) != 0) {
        printf("  FAIL: mount vmsfs %s at %s failed (errno=%d)\n", LOOP_DEV, MOUNTPT, errno);
        fail++;
        goto done_fatal;
    }
    CHECK(1, "mounted the vmsfs volume at /mnt/dka100 in this process (DCL is never told)");

    /* 1b. Self-check: THIS process (the mounter) reads the volume's HELLO.TXT so
     *     a later cross-process miss is attributable to VISIBILITY, not a broken
     *     mount. Explicit version, a direct open -- never a readdir. INFO. */
    {
        char rb[128];
        int rfd = open(MOUNTPT "/HELLO.TXT;1", O_RDONLY);
        ssize_t rn = (rfd >= 0) ? read(rfd, rb, sizeof(rb) - 1) : -1;
        if (rfd >= 0) close(rfd);
        rb[(rn > 0) ? (size_t)rn : 0] = '\0';
        printf("  INFO: mounter reads DKA100:HELLO.TXT;1 -> [%.*s]\n",
               (int)((rn > 0) ? rn - 1 : 0), rb);
        CHECK(strstr(rb, HELLO_CONTENT) != NULL,
              "the mounter itself reads the volume's HELLO.TXT content (mount is healthy)");
    }

    /* 2. PRIMARY PROOF -- cross-process file read. DCL.EXE (the AUTHORIZE.EXE
     *    analog), a separate process, resolves DKA100: -- mounted by THIS
     *    process, never in any table DCL inherited -- and reads HELLO.TXT off
     *    it. The content is distinctive to this volume; the origin/main fallback
     *    to the system disk holds no such file, so this is a true differential. */
    if (run_dcl("TYPE DKA100:[000000]HELLO.TXT;1", out, sizeof(out)) == 0) {
        show_capture("DCL TYPE DKA100:[000000]HELLO.TXT;1 (separate process)", out);
        CHECK(strstr(out, "OVMX-PROBE-ALIVE") != NULL,
              "DCL.EXE ran the script to completion");
        CHECK(strstr(out, HELLO_CONTENT) != NULL,
              "cross-process: DCL resolves DKA100: (mounted by another process) and reads HELLO.TXT off it -- the mounted unit is visible and readable");
    } else {
        CHECK(0, "DCL.EXE could not be run for the cross-process read probe");
    }

    /* 3. NEGATIVE CONTROL: DKA200: is a unit nothing mounted. The fix reads the
     *    kernel mount table -- it must NOT fabricate a mount for an absent unit
     *    (INV-6), so DCL cannot read HELLO.TXT through DKA200:. */
    if (run_dcl("TYPE DKA200:[000000]HELLO.TXT;1", out, sizeof(out)) == 0) {
        show_capture("DCL TYPE DKA200:[000000]HELLO.TXT;1 (never mounted)", out);
        CHECK(strstr(out, "OVMX-PROBE-ALIVE") != NULL,
              "DCL.EXE ran the negative-control script to completion");
        CHECK(strstr(out, HELLO_CONTENT) == NULL,
              "an UNMOUNTED unit (DKA200:) yields NO volume content -- the fix does not fabricate success for a unit that is not mounted");
    } else {
        CHECK(0, "DCL.EXE could not be run for the negative-control probe");
    }

    umount(MOUNTPT);

    printf("=== test_kmod_vmsfs_mountvis: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;

done_fatal:
    printf("=== test_kmod_vmsfs_mountvis: %d passed, %d failed ===\n", pass, fail);
    return 1;
}
