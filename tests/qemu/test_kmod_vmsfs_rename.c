/*
 * test_kmod_vmsfs_rename.c - Prove vmsfs.ko implements a REAL rename(2)
 * on a block-device (mount_bdev) volume, and that the result PERSISTS
 * across a remount (rd vms-8b3).
 *
 * ROOT CAUSE THIS GUARDS: vmsfs_blkdev_dir_iops had create/mkdir/unlink/
 * rmdir but NO .rename, so the Linux VFS returned EPERM ("Operation not
 * permitted") for every rename() on a vmsfs.ko volume. That broke the
 * universal write-tmp + rename(2) save path -- AUTHORIZE save_sysuaf()
 * (%UAF-E-RENAMEFAIL, blocking the install-set SYSTEM password, vms-963),
 * SET PASSWORD, DCL RENAME, RMS $RENAME, and SYSGEN .PAR WRITE (vms-597).
 *
 * RED-FIRST: on a vmsfs.ko built WITHOUT vmsfs_blkdev_rename, the first
 * assertion below ("rename() succeeds") FAILS with errno=EPERM (1), the
 * suite exits nonzero, and .github/workflows/ci.yml's kernel-executive job
 * reddens on this suite's "=== SUITE test_kmod_vmsfs_rename rc=1 ===" line.
 *
 * The test reuses /dev/loop0 (attached to the mastered vmsfs image by
 * tests/qemu/init.sh, exactly like test_kmod_vmsfs_blkdev.c). It uses its
 * own uniquely-named files so it neither depends on nor disturbs sibling
 * suites sharing the same image, and it cleans up after the persistence
 * check so it leaves the image as it found it.
 *
 * Phases:
 *   1. RW mount: simple rename (no target), honest-ENOENT negative control,
 *      POSIX replace of an existing target (the save path), cross-directory
 *      move.
 *   2. Remount RO: every rename result persisted; every old name is gone.
 *   3. RW mount: delete everything this suite created (leave image clean).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while(0)

#define LOOP_DEV    "/dev/loop0"
#define MOUNT_POINT "/mnt/vmsfs_blkdev"

/* Write `data` to a fresh file at MOUNT_POINT/name. Returns 0 on success. */
static int make_file(const char *name, const char *data)
{
    char path[256];
    int fd;
    ssize_t n;
    size_t len = strlen(data);

    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, name);
    fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0)
        return -1;
    n = write(fd, data, len);
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

/* Read MOUNT_POINT/name into buf (NUL-terminated). Returns bytes read, or -1. */
static ssize_t read_file(const char *name, char *buf, size_t bufsz)
{
    char path[256];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, name);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    memset(buf, 0, bufsz);
    n = read(fd, buf, bufsz - 1);
    close(fd);
    return n;
}

/*
 * Does MOUNT_POINT/name open, read back exactly `want`, and match byte-for-byte?
 * Compares the full expected string (length AND content) so there is no
 * hardcoded length to get wrong.
 */
static int content_is(const char *name, const char *want)
{
    char buf[256];
    ssize_t n = read_file(name, buf, sizeof(buf));

    return n == (ssize_t)strlen(want) && memcmp(buf, want, strlen(want)) == 0;
}

/* Does MOUNT_POINT/name exist? */
static int exists(const char *name)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, name);
    return stat(path, &st) == 0;
}

/* Build a MOUNT_POINT-relative path into a caller buffer. */
static const char *mp(char *buf, size_t sz, const char *name)
{
    snprintf(buf, sz, "%s/%s", MOUNT_POINT, name);
    return buf;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int rc;
    char p1[256], p2[256];
    struct stat st;

    printf("=== test_kmod_vmsfs_rename ===\n");

    rc = stat(LOOP_DEV, &st);
    if (rc != 0) {
        printf("  SKIP: %s not found (loop device not set up)\n", LOOP_DEV);
        printf("=== test_kmod_vmsfs_rename: %d passed, %d failed ===\n",
               pass, fail);
        return 0;
    }
    CHECK(S_ISBLK(st.st_mode), "loop device is a block device");

    mkdir(MOUNT_POINT, 0755);

    /* ============================================================
     * PHASE 1: read-write mount -- the actual rename behavior
     * ============================================================ */
    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", 0, NULL);
    CHECK(rc == 0, "mount vmsfs read-write");
    if (rc != 0) {
        printf("  FATAL: rw mount failed (errno=%d)\n", errno);
        printf("=== test_kmod_vmsfs_rename: %d passed, %d failed ===\n",
               pass, fail);
        return 1;
    }

    /* --- T1: simple rename, no pre-existing target ------------------ */
    CHECK(make_file("RNSRC.DAT", "RENAME ME\n") == 0, "create RNSRC.DAT");

    rc = rename(mp(p1, sizeof(p1), "RNSRC.DAT"),
                mp(p2, sizeof(p2), "RNDST.DAT"));
    /*
     * THE load-bearing assertion. Pre-fix (vmsfs.ko with no .rename) this
     * returns -1/EPERM -- the whole point of the bead.
     */
    CHECK(rc == 0, "rename(RNSRC.DAT, RNDST.DAT) succeeds (no EPERM)");

    CHECK(content_is("RNDST.DAT", "RENAME ME\n"),
          "RNDST.DAT holds the renamed file's content");
    CHECK(!exists("RNSRC.DAT"), "RNSRC.DAT is gone after rename");

    /* --- T2: negative control -- rename of a nonexistent source ----- */
    errno = 0;
    rc = rename(mp(p1, sizeof(p1), "NOSUCH.DAT"),
                mp(p2, sizeof(p2), "WHEREVER.DAT"));
    CHECK(rc == -1 && errno == ENOENT,
          "rename(missing source) fails honestly with ENOENT (not fake success)");
    CHECK(!exists("WHEREVER.DAT"),
          "no phantom target created by the failed rename");

    /* --- T3: POSIX replace of an existing target (the save path) ----
     * Mirrors AUTHORIZE save_sysuaf(): write a new copy under a temp name,
     * then rename it OVER the live file. The target must end up holding the
     * source bytes; the source name must be gone.
     */
    CHECK(make_file("SAVE.NEW", "NEWDB\n") == 0, "create SAVE.NEW (new content)");
    CHECK(make_file("SAVE.DAT", "OLDDB\n") == 0, "create SAVE.DAT (target to replace)");

    rc = rename(mp(p1, sizeof(p1), "SAVE.NEW"),
                mp(p2, sizeof(p2), "SAVE.DAT"));
    CHECK(rc == 0, "rename(SAVE.NEW, SAVE.DAT) over existing target succeeds");

    CHECK(content_is("SAVE.DAT", "NEWDB\n"),
          "SAVE.DAT now holds the SOURCE bytes (target replaced in place)");
    CHECK(!exists("SAVE.NEW"), "SAVE.NEW is gone after the replacing rename");

    /* --- T4: cross-directory move ----------------------------------- */
    CHECK(mkdir(mp(p1, sizeof(p1), "RNDIR"), 0755) == 0, "mkdir RNDIR");
    CHECK(make_file("TOMOVE.DAT", "MOVED\n") == 0, "create TOMOVE.DAT in root");

    rc = rename(mp(p1, sizeof(p1), "TOMOVE.DAT"),
                mp(p2, sizeof(p2), "RNDIR/TOMOVE.DAT"));
    CHECK(rc == 0, "rename(TOMOVE.DAT, RNDIR/TOMOVE.DAT) across directories");

    CHECK(content_is("RNDIR/TOMOVE.DAT", "MOVED\n"),
          "RNDIR/TOMOVE.DAT holds the moved content");
    CHECK(!exists("TOMOVE.DAT"), "TOMOVE.DAT gone from root after the move");

    rc = umount(MOUNT_POINT);
    CHECK(rc == 0, "umount read-write mount");

    /* ============================================================
     * PHASE 2: remount read-only -- prove it PERSISTED to the device
     * ============================================================ */
    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", MS_RDONLY, NULL);
    CHECK(rc == 0, "remount read-only for persistence check");
    if (rc == 0) {
        CHECK(content_is("RNDST.DAT", "RENAME ME\n"),
              "RNDST.DAT persisted with correct content across remount");
        CHECK(!exists("RNSRC.DAT"), "RNSRC.DAT stays gone across remount");

        CHECK(content_is("SAVE.DAT", "NEWDB\n"),
              "replaced SAVE.DAT persisted with SOURCE bytes across remount");
        CHECK(!exists("SAVE.NEW"), "SAVE.NEW stays gone across remount");

        CHECK(content_is("RNDIR/TOMOVE.DAT", "MOVED\n"),
              "moved RNDIR/TOMOVE.DAT persisted across remount");
        CHECK(!exists("TOMOVE.DAT"),
              "TOMOVE.DAT stays gone from root across remount");

        umount(MOUNT_POINT);
    }

    /* ============================================================
     * PHASE 3: read-write mount -- clean up what this suite created
     * ============================================================ */
    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", 0, NULL);
    if (rc == 0) {
        unlink(mp(p1, sizeof(p1), "RNDST.DAT"));
        unlink(mp(p1, sizeof(p1), "SAVE.DAT"));
        unlink(mp(p1, sizeof(p1), "RNDIR/TOMOVE.DAT"));
        rmdir(mp(p1, sizeof(p1), "RNDIR"));
        umount(MOUNT_POINT);
    }

    printf("=== test_kmod_vmsfs_rename: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
