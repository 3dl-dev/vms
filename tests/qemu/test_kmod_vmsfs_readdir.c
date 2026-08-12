/*
 * test_kmod_vmsfs_readdir.c - a BACKING-DIRECTORY (overlay) vmsfs mount is
 * fully iterable and its children are openable by name from a process OTHER
 * than the one that mounted it (rd vms-93a, found by vms-8b6).
 *
 * THE BUG THIS GATES.  vmsfs.ko's overlay-mode readdir (vmsfs_iterate_shared
 * in src/kernel/vmsfs/vmsfs_dir.c) ignored the directory file's persistent
 * cursor (ctx->pos).  On every getdents() it re-opened the backing directory
 * and re-scanned it from position 0, re-emitting the whole listing, so
 * getdents() NEVER returned 0 (EOF) and readdir() spun forever -- for every
 * reader.  A cross-process open of a child name that has to list the directory
 * hung with it.  (test_kmod_vmsfs_mountvis's header calls this out as "an
 * orthogonal vmsfs.ko defect, filed separately" and deliberately uses a
 * BLOCK-DEVICE vmsfs to sidestep it; this suite is that separate defect.  The
 * block-device on-disk directory path -- vmsfs_blkdev_iterate, a different
 * fops table -- is unaffected, which is why test_kmod_vmsfs / _blkdev, driven
 * off a loop-mounted image, never trip it.)
 *
 * WHY A CHILD PROCESS.  The bug is a kernel readdir defect and is process-
 * independent, but the reported symptom and the fix are about cross-process
 * use: this suite mounts the overlay in the PARENT and then does the iteration
 * and the open in FORKED CHILDREN -- separate processes, exactly like the
 * AUTHORIZE.EXE / RUN'd images that reach a volume DCL mounted.
 *
 * WHY EVERY READDIR IS TIMEOUT-GUARDED.  Non-termination IS the bug, so the
 * child that iterates the directory must never be run unbounded.  The child
 * loops readdir() with NO internal cap (so pre-fix it genuinely spins) and the
 * PARENT enforces a hard wall-clock bound: if the child has not finished
 * within READDIR_TIMEOUT_S it is SIGKILLed and the run records a NAMED failure
 * ("readdir did not terminate") instead of hanging the whole QEMU harness.
 * Red before the fix is therefore a clean timeout-kill; green after it is the
 * child completing on its own and reporting each entry seen exactly once.
 *
 * ASSERTIONS
 *   1. Cross-process readdir of the overlay mount TERMINATES (child not killed
 *      by the parent's timeout).  RED on pre-fix vmsfs.ko: the child spins and
 *      is killed.
 *   2. That readdir lists each of the four backing entries EXACTLY ONCE (no
 *      duplicates, none missing) -- proves the cursor advances to EOF rather
 *      than restarting.
 *   3. Cross-process open of a child by unversioned name (highest version) and
 *      by explicit version SUCCEEDS, does not hang, and reads the child's own
 *      distinctive content -- proves lookup resolves and the open completes.
 *
 * Needs only vmsfs.ko (loaded by init.sh in every rig); it opens no /dev/vms
 * and reaches no executive facility, so it is a test_kmod_vmsfs_* suite whose
 * verdict is rc=0 with or without an insmod'd vms.ko (see ci.yml job 3c and
 * tests/qemu/facility_defects.sh's SCOPE_OUT_SUITES).
 *
 * Statically linked, runs inside the minimal BusyBox initramfs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

#define BACKING          "/tmp/vmsfs_readdir_backing"
#define MOUNTPT          "/mnt/vmsfs_readdir"
#define READDIR_TIMEOUT_S 20          /* parent's hard bound on the child */
#define CHILD_RESULT     "/tmp/vmsfs_readdir_result"

/* The four entries staged into the backing directory (VMS versioned names). */
static const char *const ENTRIES[] = {
    "ALPHA.TXT;1", "BETA.TXT;1", "BETA.TXT;2", "GAMMA.TXT;1",
};
#define N_ENTRIES ((int)(sizeof(ENTRIES) / sizeof(ENTRIES[0])))

/* Distinctive per-file content -- BETA.TXT;2 is the highest BETA version, the
 * one an unversioned open("BETA.TXT") must resolve to. */
#define BETA2_CONTENT  "READDIR-CHILD-BETA-V2-8f3a"
#define ALPHA1_CONTENT "READDIR-CHILD-ALPHA-V1-1c22"

static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    size_t len = strlen(content);
    ssize_t n = write(fd, content, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/*
 * child_readdir - runs in a SEPARATE process. Iterates MOUNTPT with NO
 * internal iteration cap (so a non-terminating readdir genuinely spins and is
 * left for the parent to kill), tallying how many times each expected entry is
 * seen and whether any unexpected/duplicate entry shows up. Writes a one-line
 * result to CHILD_RESULT and exits 0 ONLY if it got all the way to EOF.
 *
 * Result line: "DONE seen=<a,b,c,d> dups=<n> unexpected=<n>"
 */
static void child_readdir(void)
{
    DIR *d = opendir(MOUNTPT);
    if (!d)
        _exit(10);

    int seen[N_ENTRIES];
    int i, dups = 0, unexpected = 0;
    for (i = 0; i < N_ENTRIES; i++)
        seen[i] = 0;

    struct dirent *de;
    errno = 0;
    /* Unbounded on purpose: readdir() returns NULL only at real EOF. Pre-fix
     * this never happens and the parent's timeout kills us -- that IS the red
     * signal. */
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        int matched = -1;
        for (i = 0; i < N_ENTRIES; i++) {
            if (!strcmp(de->d_name, ENTRIES[i])) { matched = i; break; }
        }
        if (matched < 0) {
            unexpected++;
        } else {
            if (seen[matched] > 0)
                dups++;
            seen[matched]++;
        }
    }
    closedir(d);

    char line[256];
    int off = snprintf(line, sizeof(line), "DONE seen=");
    for (i = 0; i < N_ENTRIES; i++)
        off += snprintf(line + off, sizeof(line) - off, "%s%d",
                        i ? "," : "", seen[i]);
    snprintf(line + off, sizeof(line) - off, " dups=%d unexpected=%d\n",
             dups, unexpected);
    (void)write_file(CHILD_RESULT, line);
    _exit(0);
}

/*
 * run_child_bounded - fork `fn` into a child and wait at most timeout_s for it,
 * SIGKILLing it on expiry. Returns the child's exit status via *wstatus, or
 * sets *timed_out = 1 if it had to be killed.
 */
static pid_t run_child_bounded(void (*fn)(void), int timeout_s,
                               int *wstatus, int *timed_out)
{
    *timed_out = 0;
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(99);  /* fn() is expected to _exit() itself */
    }
    if (pid < 0)
        return -1;

    for (int waited = 0; waited < timeout_s * 50; waited++) {
        pid_t r = waitpid(pid, wstatus, WNOHANG);
        if (r == pid)
            return pid;
        if (r < 0)
            return -1;
        usleep(20000);  /* 20ms */
    }
    /* Timed out -- the defining symptom of the bug. */
    kill(pid, 9);
    waitpid(pid, wstatus, 0);
    *timed_out = 1;
    return pid;
}

/* child_open - separate process: open children by name and stash what it read
 * so the parent can assert on it. */
static void child_open(void)
{
    char buf[256];
    FILE *out = fopen(CHILD_RESULT, "w");
    if (!out)
        _exit(10);

    /* Unversioned name must resolve to the HIGHEST version (BETA.TXT;2). */
    int fd = open(MOUNTPT "/BETA.TXT", O_RDONLY);
    ssize_t n = (fd >= 0) ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    buf[(n > 0) ? (size_t)n : 0] = '\0';
    fprintf(out, "BETA=%s\n", buf);

    /* Explicit version. */
    fd = open(MOUNTPT "/ALPHA.TXT;1", O_RDONLY);
    n = (fd >= 0) ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    buf[(n > 0) ? (size_t)n : 0] = '\0';
    fprintf(out, "ALPHA=%s\n", buf);

    fclose(out);
    _exit(0);
}

static int read_result(char *buf, size_t sz)
{
    int fd = open(CHILD_RESULT, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sz - 1);
    close(fd);
    buf[(n > 0) ? (size_t)n : 0] = '\0';
    return (n > 0) ? 0 : -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_vmsfs_readdir (overlay readdir terminates + child open works, vms-93a) ===\n");

    /* 0. Precondition: vmsfs registered. */
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
        if (!found) goto done;
    }

    /* 1. Stage a backing directory with four versioned files. */
    mkdir(BACKING, 0755);
    {
        char p[512];
        int ok = 1;
        for (int i = 0; i < N_ENTRIES; i++) {
            snprintf(p, sizeof(p), "%s/%s", BACKING, ENTRIES[i]);
            const char *content =
                !strcmp(ENTRIES[i], "BETA.TXT;2")  ? BETA2_CONTENT  :
                !strcmp(ENTRIES[i], "ALPHA.TXT;1") ? ALPHA1_CONTENT : "x";
            if (write_file(p, content) != 0) ok = 0;
        }
        CHECK(ok, "staged four versioned files in the backing directory");
        if (!ok) goto done;
    }

    /* 2. Mount the overlay vmsfs (backing-directory mode). */
    mkdir(MOUNTPT, 0755);
    if (mount("none", MOUNTPT, "vmsfs", 0,
              "backing=" BACKING) != 0) {
        printf("  FAIL: mount overlay vmsfs at %s failed (errno=%d)\n",
               MOUNTPT, errno);
        fail++;
        goto done;
    }
    CHECK(1, "mounted a backing-directory vmsfs at " MOUNTPT " (this process is the mounter)");

    /* 3. PRIMARY PROOF: a SEPARATE process iterates the directory. Pre-fix
     *    this spins forever and the parent kills it (RED); post-fix it reaches
     *    EOF and reports each entry once. */
    unlink(CHILD_RESULT);
    {
        int wstatus = 0, timed_out = 0;
        run_child_bounded(child_readdir, READDIR_TIMEOUT_S, &wstatus, &timed_out);

        CHECK(!timed_out,
              "cross-process readdir of the overlay mount TERMINATES (child not killed by the timeout)");
        if (timed_out) {
            printf("  INFO: readdir child killed after %ds -- the non-terminating "
                   "backing-dir readdir bug (vms-93a) is present\n", READDIR_TIMEOUT_S);
        } else {
            char res[256];
            if (read_result(res, sizeof(res)) == 0) {
                printf("  INFO: readdir child result: %s", res);
                /* Every count must be exactly 1, no dups, no unexpected. */
                char expect[128];
                int off = snprintf(expect, sizeof(expect), "DONE seen=");
                for (int i = 0; i < N_ENTRIES; i++)
                    off += snprintf(expect + off, sizeof(expect) - off,
                                    "%s1", i ? "," : "");
                snprintf(expect + off, sizeof(expect) - off,
                         " dups=0 unexpected=0\n");
                CHECK(strcmp(res, expect) == 0,
                      "readdir listed each of the four entries EXACTLY ONCE (no duplicates, none missing)");
            } else {
                CHECK(0, "readdir child produced a result (it completed and wrote its tally)");
            }
        }
    }

    /* 4. Cross-process open of children by name. */
    unlink(CHILD_RESULT);
    {
        int wstatus = 0, timed_out = 0;
        run_child_bounded(child_open, READDIR_TIMEOUT_S, &wstatus, &timed_out);

        CHECK(!timed_out,
              "cross-process open of a child by name does not hang (child not killed by the timeout)");
        if (!timed_out) {
            char res[512];
            if (read_result(res, sizeof(res)) == 0) {
                printf("  INFO: open child result:\n----8<----\n%s----8<----\n", res);
                CHECK(strstr(res, "BETA=" BETA2_CONTENT) != NULL,
                      "unversioned open(\"BETA.TXT\") resolved to the highest version (;2) and read its content");
                CHECK(strstr(res, "ALPHA=" ALPHA1_CONTENT) != NULL,
                      "explicit-version open(\"ALPHA.TXT;1\") read its content");
            } else {
                CHECK(0, "open child produced a result");
            }
        }
    }

    umount(MOUNTPT);

done:
    unlink(CHILD_RESULT);
    printf("=== test_kmod_vmsfs_readdir: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
