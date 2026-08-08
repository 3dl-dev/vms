/*
 * test_kmod_vmsfs_exepath.c - a vmsfs dentry must stay HASHED across re-lookup
 *
 * WHAT DEFECT THIS PINS (rd vms-00e)
 *
 * vmsfs.ko's ->d_revalidate used to answer "invalid" for EVERY positive
 * regular-file dentry, unconditionally. A d_revalidate() of 0 makes the VFS
 * call d_invalidate(), which UNHASHES the dentry; an unhashed non-root dentry
 * satisfies d_unlinked(), and d_path() renders any d_unlinked() path with a
 * " (deleted)" suffix. /proc/<pid>/exe and /proc/<pid>/fd/<n> are both
 * d_path() readers.
 *
 * So the FIRST time anything re-walked the path of a file on vmsfs, every
 * /proc link naming that file started reading "... (deleted)" -- while the
 * file was present, open, and unmodified. The product symptom was DCL's
 * SPAWN: cmd_spawn() (src/vmsdcl/dcl_cmd_process.c) re-execs DCL via
 * readlink("/proc/self/exe"), so it execl()'d a path with " (deleted)" glued
 * on the end, got ENOENT, and answered %DCL-E-CREPRC.
 *
 * The two observed shapes differed only in WHEN the second walk happened:
 *   - static DCL.EXE: the first SPAWN's own execl() was the second walk, so
 *     spawn #1 worked and spawn #2 onward failed.
 *   - VMS-native DCL.EXE: IMGACT.EXE re-opens the executable by AT_EXECFN to
 *     read its .vms$sv/.vms$imp sections before the image ever runs
 *     (src/imgact/imgact.c, activate_symbol_vector()), so the dentry was
 *     already unhashed at the first line of main() and spawn #1 failed.
 *
 * WHAT THIS TEST ASSERTS, AND WHY IT IS NOT VACUOUS
 *
 * Phase 1 (block-device mode -- the mount the bootable image actually runs
 * DCL.EXE from) and phase 2 (overlay mode) both hold an open fd and then
 * re-walk the same path, asserting /proc/self/fd/<n> never grows
 * " (deleted)". Both fail against the pre-fix module.
 *
 * Phase 2 also carries the POSITIVE CONTROL that stops "always return valid"
 * from passing this file: after a NEW VERSION of the same base name is
 * created, the old fd's path MUST become "(deleted)", because the unversioned
 * name genuinely no longer names that file. A d_revalidate that just answers
 * "valid" unconditionally passes every other assertion here and fails that
 * one.
 *
 * Phase 3 reproduces the product symptom directly: it copies this very
 * program onto vmsfs, execs it from there, and has the child do exactly what
 * IMGACT does -- re-open its own executable by path -- then read
 * /proc/self/exe and re-exec itself through it, which is the SPAWN operation.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while(0)

#define LOOP_DEV      "/dev/loop0"
#define BLK_MOUNT     "/mnt/vmsfs_exepath_blk"
#define OVL_MOUNT     "/mnt/vmsfs_exepath_ovl"
#define OVL_BACKING   "/tmp/vmsfs_exepath_backing"

#define DELETED_SUFFIX " (deleted)"

/*
 * Read a /proc magic link and report whether d_path() marked it deleted.
 * Returns 1 = marked deleted, 0 = clean, -1 = readlink failed.
 */
static int proclink_deleted(const char *proclink, char *out, size_t outsz)
{
    ssize_t n = readlink(proclink, out, outsz - 1);

    if (n < 0) {
        snprintf(out, outsz, "<readlink failed errno=%d>", errno);
        return -1;
    }
    out[n] = '\0';
    return strstr(out, DELETED_SUFFIX) != NULL;
}

static int fd_deleted(int fd, char *out, size_t outsz)
{
    char proclink[64];

    snprintf(proclink, sizeof(proclink), "/proc/self/fd/%d", fd);
    return proclink_deleted(proclink, out, outsz);
}

static int backing_has(const char *dir, const char *name)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, name) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

/* ================================================================
 * Phase 3 child: the IMGACT + SPAWN sequence, running FROM vmsfs
 * ================================================================ */

/*
 * argv[1] == "--child", argv[2] == the path this image was exec'd from.
 * Exit codes are distinct so the parent can say WHICH step regressed.
 */
static int child_main(const char *execfn)
{
    char link[4096];
    int d, fd;

    /* 1. Straight out of exec, before anything else touches the path. */
    d = proclink_deleted("/proc/self/exe", link, sizeof(link));
    printf("    child: /proc/self/exe after exec = %s\n", link);
    if (d != 0)
        return 11;

    /*
     * 2. What IMGACT.EXE does during activation: re-open the executable by
     *    its own AT_EXECFN path to read the symbol vector out of it. This is
     *    the second path walk, and it is what used to unhash the dentry.
     */
    fd = open(execfn, O_RDONLY);
    if (fd < 0)
        return 12;
    close(fd);

    /* 3. The regression: /proc/self/exe must STILL be a usable path. */
    d = proclink_deleted("/proc/self/exe", link, sizeof(link));
    printf("    child: /proc/self/exe after re-open = %s\n", link);
    if (d != 0)
        return 13;

    /*
     * 4. The product operation itself: DCL's SPAWN re-execs the image named
     *    by /proc/self/exe. If the link carries " (deleted)", this ENOENTs --
     *    which is exactly %DCL-E-CREPRC.
     */
    execl(link, "vmsfs-exepath-grandchild", "--grandchild", (char *)NULL);
    printf("    child: execl(\"%s\") failed errno=%d\n", link, errno);
    return 14;
}

/* ================================================================
 * Probe shared by both mount modes
 * ================================================================ */

/*
 * Hold an open fd on @path, then re-walk @path @walks times (each walk runs
 * ->d_revalidate on the cached dentry) and assert the fd's d_path never grows
 * " (deleted)".
 */
static void probe_relookup_stability(const char *path, const char *label,
                                     int walks)
{
    char msg[160];
    char link[4096];
    struct stat st;
    int fd, d, i;

    fd = open(path, O_RDONLY);
    snprintf(msg, sizeof(msg), "%s: open %s", label, path);
    CHECK(fd >= 0, msg);
    if (fd < 0)
        return;

    d = fd_deleted(fd, link, sizeof(link));
    snprintf(msg, sizeof(msg),
             "%s: fresh open is not marked deleted (%s)", label, link);
    CHECK(d == 0, msg);

    for (i = 0; i < walks; i++) {
        int fd2;

        /* Two different kinds of second walk: stat(), then open(). */
        if (stat(path, &st) != 0)
            break;
        fd2 = open(path, O_RDONLY);
        if (fd2 >= 0)
            close(fd2);
    }

    d = fd_deleted(fd, link, sizeof(link));
    snprintf(msg, sizeof(msg),
             "%s: still not marked deleted after %d re-lookups (%s)",
             label, walks, link);
    CHECK(d == 0, msg);

    close(fd);
}

/* ================================================================
 * Phase 1: block-device mode (what the bootable image runs DCL from)
 * ================================================================ */

static void phase_blkdev(void)
{
    struct stat st;
    char path[256];
    int rc;

    printf("--- phase 1: block-device mode ---\n");

    if (stat(LOOP_DEV, &st) != 0 || !S_ISBLK(st.st_mode)) {
        printf("  FAIL: %s absent -- block-device mode is the mount the "
               "product boots from, so this is not skippable\n", LOOP_DEV);
        fail++;
        return;
    }

    mkdir(BLK_MOUNT, 0755);
    rc = mount(LOOP_DEV, BLK_MOUNT, "vmsfs", 0, NULL);
    CHECK(rc == 0, "phase1: mount vmsfs block device");
    if (rc != 0)
        return;

    /* HELLO.TXT;1 is baked into the image by mkimage_vmsfs.c. */
    snprintf(path, sizeof(path), "%s/HELLO.TXT;1", BLK_MOUNT);
    probe_relookup_stability(path, "phase1 versioned", 4);

    /* The unversioned form is the one an exec/AT_EXECFN path uses. */
    snprintf(path, sizeof(path), "%s/HELLO.TXT", BLK_MOUNT);
    probe_relookup_stability(path, "phase1 unversioned", 4);

    rc = umount(BLK_MOUNT);
    CHECK(rc == 0, "phase1: umount");
}

/* ================================================================
 * Phase 2: overlay mode + the positive control
 * ================================================================ */

static void phase_overlay(void)
{
    char path[256];
    char link[4096];
    char msg[160];
    int rc, fd, d;

    printf("--- phase 2: overlay mode ---\n");

    mkdir(OVL_BACKING, 0755);
    mkdir(OVL_MOUNT, 0755);

    rc = mount("none", OVL_MOUNT, "vmsfs", 0,
               "backing=" OVL_BACKING ",case_blind=1");
    CHECK(rc == 0, "phase2: mount vmsfs overlay");
    if (rc != 0)
        return;

    /* Seed PROBE.TXT;1 */
    snprintf(path, sizeof(path), "%s/PROBE.TXT", OVL_MOUNT);
    fd = open(path, O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "phase2: create PROBE.TXT (becomes ;1)");
    if (fd >= 0) {
        write(fd, "one\n", 4);
        close(fd);
    }
    CHECK(backing_has(OVL_BACKING, "PROBE.TXT;1"),
          "phase2: backing dir holds PROBE.TXT;1");

    probe_relookup_stability(path, "phase2", 4);

    /*
     * POSITIVE CONTROL. Hold an fd on the CURRENT highest version, then cut a
     * new version. The unversioned name now resolves somewhere else, so the
     * held dentry is genuinely stale and MUST be invalidated -- d_path is
     * then correct to say "(deleted)".
     *
     * A ->d_revalidate that unconditionally answers "valid" (the naive way to
     * make every other assertion in this file pass) fails right here.
     */
    fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "phase2 control: open PROBE.TXT (resolves to ;1)");
    if (fd >= 0) {
        int fd2;

        d = fd_deleted(fd, link, sizeof(link));
        snprintf(msg, sizeof(msg),
                 "phase2 control: clean before a new version exists (%s)",
                 link);
        CHECK(d == 0, msg);

        fd2 = open(path, O_WRONLY | O_CREAT, 0644);
        CHECK(fd2 >= 0, "phase2 control: create PROBE.TXT again (becomes ;2)");
        if (fd2 >= 0) {
            write(fd2, "two\n", 4);
            close(fd2);
        }
        CHECK(backing_has(OVL_BACKING, "PROBE.TXT;2"),
              "phase2 control: backing dir holds PROBE.TXT;2");

        /* Force a walk so the VFS asks ->d_revalidate about the old dentry. */
        fd2 = open(path, O_RDONLY);
        if (fd2 >= 0)
            close(fd2);

        d = fd_deleted(fd, link, sizeof(link));
        snprintf(msg, sizeof(msg),
                 "phase2 control: stale ;1 dentry IS invalidated once ;2 "
                 "exists (%s)", link);
        CHECK(d == 1, msg);

        close(fd);
    }

    /* And the new highest version is itself stable across re-lookup. */
    probe_relookup_stability(path, "phase2 post-version", 4);

    rc = umount(OVL_MOUNT);
    CHECK(rc == 0, "phase2: umount");
}

/* ================================================================
 * Phase 3: exec from vmsfs, IMGACT-style, then SPAWN-style re-exec
 * ================================================================ */

static int copy_file(const char *src, const char *dst)
{
    char buf[65536];
    int in, out;
    ssize_t n;

    in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0) {
        close(in);
        return -1;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;

        while (off < n) {
            ssize_t w = write(out, buf + off, n - off);

            if (w <= 0) {
                close(in);
                close(out);
                return -1;
            }
            off += w;
        }
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

static void phase_exec(void)
{
    struct stat st;
    char self[4096];
    char dst[256];
    ssize_t n;
    pid_t pid;
    int rc, status;

    printf("--- phase 3: exec from vmsfs (IMGACT + SPAWN shape) ---\n");

    /*
     * BLOCK-DEVICE MODE, DELIBERATELY -- not the overlay used above.
     * Overlay mode's ->mmap just forwards to the backing file while leaving
     * vma->vm_file pointing at the vmsfs file, and overlay inodes carry no
     * address_space_operations, so execve() cannot map an image out of an
     * overlay mount at all. Block-device mode is where vmsfs implements
     * ->read_folio/->get_block ("Required for execve to mmap ELF segments
     * from the filesystem", vmsfs_blkdev.c) -- and it is the mount
     * distro/Dockerfile.bootable's system disk actually runs DCL.EXE from,
     * so it is the one this phase has to use to mean anything.
     */
    if (stat(LOOP_DEV, &st) != 0 || !S_ISBLK(st.st_mode)) {
        printf("  FAIL: %s absent -- cannot exec from a real vmsfs\n",
               LOOP_DEV);
        fail++;
        return;
    }

    mkdir(BLK_MOUNT, 0755);
    rc = mount(LOOP_DEV, BLK_MOUNT, "vmsfs", 0, NULL);
    CHECK(rc == 0, "phase3: mount vmsfs block device read-write");
    if (rc != 0)
        return;

    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    CHECK(n > 0, "phase3: read own image path");
    if (n <= 0)
        goto out;
    self[n] = '\0';

    snprintf(dst, sizeof(dst), "%s/CHILD.EXE", BLK_MOUNT);
    rc = copy_file(self, dst);
    CHECK(rc == 0, "phase3: copy this image onto vmsfs as CHILD.EXE");
    if (rc != 0)
        goto out;
    chmod(dst, 0755);

    pid = fork();
    if (pid == 0) {
        execl(dst, "vmsfs-exepath-child", "--child", dst, (char *)NULL);
        _exit(10);
    }
    CHECK(pid > 0, "phase3: fork");
    if (pid > 0) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 10) {
            printf("  FAIL: phase3: could not exec CHILD.EXE from vmsfs\n");
            fail++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 11) {
            printf("  FAIL: phase3: /proc/self/exe was already '(deleted)' "
                   "immediately after exec\n");
            fail++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 12) {
            printf("  FAIL: phase3: child could not re-open its own image "
                   "by path\n");
            fail++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 13) {
            printf("  FAIL: phase3: /proc/self/exe became '(deleted)' after "
                   "the IMGACT-style re-open -- THE vms-00e DEFECT\n");
            fail++;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 14) {
            printf("  FAIL: phase3: SPAWN-style re-exec via /proc/self/exe "
                   "failed -- this is %%DCL-E-CREPRC\n");
            fail++;
        } else {
            char msg[160];

            snprintf(msg, sizeof(msg),
                     "phase3: exec from vmsfs -> IMGACT re-open -> SPAWN "
                     "re-exec (wait status 0x%x)", status);
            CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, msg);
        }
    }

out:
    umount(BLK_MOUNT);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so an unflushed fork() cannot splice output */
    if (argc >= 2 && strcmp(argv[1], "--grandchild") == 0) {
        /* Reached only if the SPAWN-style re-exec worked. */
        printf("    grandchild: re-exec via /proc/self/exe succeeded\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--child") == 0)
        return child_main(argv[2]);

    printf("=== test_kmod_vmsfs_exepath ===\n");

    /*
     * ORDER MATTERS, AND IT IS NOT COSMETIC. The exec phase runs FIRST so
     * that it is REACHED when this file is run against the pre-fix module.
     * The blanket-invalidate behaviour does not just mis-render d_path: it
     * also leaves an open file holding a dentry the VFS then unlinks from
     * its inode, and closing that fd oopses the kernel in __fput()
     * (NULL d_inode, measured under this harness). Once that happens the
     * test process is killed and any later phase is never executed -- so a
     * phase ordered after the overlay one could never be shown to fail on
     * the defect it exists to pin.
     */
    phase_exec();
    phase_blkdev();
    phase_overlay();

    printf("=== test_kmod_vmsfs_exepath: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
