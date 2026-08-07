/*
 * test_kmod_vmsfs_blkdev.c - Test vmsfs block-device mount from QEMU initramfs
 *
 * Exercises the block-device mount path (mount_bdev) in vmsfs.ko:
 *
 * Phase 1 — Read-only mount (verify pre-existing content):
 *   readdir, stat, read, case-insensitive lookup, version-less lookup, statfs
 *
 * Phase 2 — Read-write mount (test write operations):
 *   create file + write + read back, explicit versioned creation,
 *   mkdir + create file in subdir, statfs free blocks, unlink, rmdir
 *
 * Phase 3 — Remount read-only (verify persistence):
 *   Files created in phase 2 survive remount, deleted items stay gone
 *
 * The test image is created by mkimage_vmsfs at Docker build time.
 * init.sh sets up the loop device at /dev/loop0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <dirent.h>
#include <errno.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while(0)

#define LOOP_DEV    "/dev/loop0"
#define MOUNT_POINT "/mnt/vmsfs_blkdev"
#define VMSFS_MAGIC 0x564D5346  /* "VMSF" */

/* Must match mkimage_vmsfs.c */
#define EXPECTED_CONTENT "Hello from VMSFS block device!\n"
#define EXPECTED_LEN     (sizeof(EXPECTED_CONTENT) - 1)

/* Test data for write tests */
#define WRITE_CONTENT    "Written from test program!\n"
#define WRITE_LEN        (sizeof(WRITE_CONTENT) - 1)

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: keep stdout line-buffered even when init.sh redirects it to a file, so an unflushed fork() cannot splice output */
    int rc, fd;
    ssize_t n;
    char buf[256];
    struct stat st;

    printf("=== test_kmod_vmsfs_blkdev ===\n");

    /* Verify loop device exists (set up by init.sh) */
    rc = stat(LOOP_DEV, &st);
    if (rc != 0) {
        printf("  SKIP: %s not found (loop device not set up)\n", LOOP_DEV);
        printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
               pass, fail);
        return 0;
    }
    CHECK(S_ISBLK(st.st_mode), "loop device is a block device");

    mkdir(MOUNT_POINT, 0755);

    /* ============================================================
     * PHASE 1: Read-only mount — verify pre-existing content
     * ============================================================ */

    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", MS_RDONLY, NULL);
    CHECK(rc == 0, "mount vmsfs read-only");
    if (rc != 0) {
        printf("  FATAL: mount failed (errno=%d)\n", errno);
        printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
               pass, fail);
        return 1;
    }

    /* List root directory */
    {
        DIR *d = opendir(MOUNT_POINT);
        int found_hello = 0;
        int entry_count = 0;

        CHECK(d != NULL, "opendir mount point");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                    continue;
                entry_count++;
                printf("    dir entry: %s\n", ent->d_name);
                if (strcmp(ent->d_name, "HELLO.TXT;1") == 0)
                    found_hello = 1;
            }
            closedir(d);
        }
        CHECK(found_hello, "readdir lists HELLO.TXT;1");
        CHECK(entry_count == 1, "root directory has exactly 1 user entry");
    }

    /* Stat and read file */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/HELLO.TXT;1", MOUNT_POINT);
        rc = stat(path, &st);
        CHECK(rc == 0, "stat HELLO.TXT;1");
        if (rc == 0) {
            CHECK(st.st_size == (off_t)EXPECTED_LEN,
                  "file size matches expected content length");
            CHECK(S_ISREG(st.st_mode), "HELLO.TXT;1 is a regular file");
        }

        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "open HELLO.TXT;1 for reading");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(n == (ssize_t)EXPECTED_LEN, "read correct number of bytes");
            CHECK(memcmp(buf, EXPECTED_CONTENT, EXPECTED_LEN) == 0,
                  "file content matches expected");
            close(fd);
        }
    }

    /* Case-insensitive and version-less lookup */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/hello.txt;1", MOUNT_POINT);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "case-insensitive lookup (hello.txt;1)");
        if (fd >= 0) close(fd);

        snprintf(path, sizeof(path), "%s/HELLO.TXT", MOUNT_POINT);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "version-less lookup (HELLO.TXT -> ;1)");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(n == (ssize_t)EXPECTED_LEN,
                  "version-less read returns correct content");
            close(fd);
        }
    }

    /* Statfs */
    {
        struct statfs sfs;
        rc = statfs(MOUNT_POINT, &sfs);
        CHECK(rc == 0, "statfs succeeds");
        if (rc == 0) {
            CHECK((uint32_t)sfs.f_type == VMSFS_MAGIC,
                  "statfs f_type is VMSFS_MAGIC");
            CHECK(sfs.f_bsize == 512, "statfs block size is 512");
            CHECK(sfs.f_blocks == 2048, "statfs reports 2048 total blocks");
        }
    }

    rc = umount(MOUNT_POINT);
    CHECK(rc == 0, "umount read-only mount");

    /* ============================================================
     * PHASE 2: Read-write mount — test write operations
     * ============================================================ */

    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", 0, NULL);
    CHECK(rc == 0, "mount vmsfs read-write");
    if (rc != 0) {
        printf("  FATAL: rw mount failed (errno=%d)\n", errno);
        printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
               pass, fail);
        return fail > 0 ? 1 : 0;
    }

    /* Record initial free blocks */
    long initial_free = 0;
    {
        struct statfs sfs;
        if (statfs(MOUNT_POINT, &sfs) == 0)
            initial_free = sfs.f_bfree;
        printf("    initial free blocks: %ld\n", initial_free);
    }

    /* --- Test: create new file, write data, read back --- */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/TEST.DAT", MOUNT_POINT);

        fd = open(path, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0, "create TEST.DAT (new file)");
        if (fd >= 0) {
            n = write(fd, WRITE_CONTENT, WRITE_LEN);
            CHECK(n == (ssize_t)WRITE_LEN, "write data to TEST.DAT");
            close(fd);
        }

        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "open TEST.DAT for reading");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(n == (ssize_t)WRITE_LEN, "read back correct length");
            CHECK(memcmp(buf, WRITE_CONTENT, WRITE_LEN) == 0,
                  "read-back content matches written data");
            close(fd);
        }
    }

    /* --- Test: explicit versioned creation --- */
    {
        char path_v1[256], path_v2[256];
        const char *v1data = "Version one\n";
        const char *v2data = "Version two\n";

        snprintf(path_v1, sizeof(path_v1),
                 "%s/RESULT.TXT;1", MOUNT_POINT);
        snprintf(path_v2, sizeof(path_v2),
                 "%s/RESULT.TXT;2", MOUNT_POINT);

        /* Create version 1 */
        fd = open(path_v1, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0, "create RESULT.TXT;1");
        if (fd >= 0) {
            write(fd, v1data, strlen(v1data));
            close(fd);
        }

        /* Create version 2 */
        fd = open(path_v2, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0, "create RESULT.TXT;2");
        if (fd >= 0) {
            write(fd, v2data, strlen(v2data));
            close(fd);
        }

        /* Read back version 1 */
        fd = open(path_v1, O_RDONLY);
        CHECK(fd >= 0, "open RESULT.TXT;1 for reading");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(strcmp(buf, v1data) == 0, "RESULT.TXT;1 content correct");
            close(fd);
        }

        /* Read back version 2 */
        fd = open(path_v2, O_RDONLY);
        CHECK(fd >= 0, "open RESULT.TXT;2 for reading");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(strcmp(buf, v2data) == 0, "RESULT.TXT;2 content correct");
            close(fd);
        }

        /* Readdir should show both versions */
        {
            DIR *d = opendir(MOUNT_POINT);
            int found_v1 = 0, found_v2 = 0;

            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (strcmp(ent->d_name, "RESULT.TXT;1") == 0)
                        found_v1 = 1;
                    if (strcmp(ent->d_name, "RESULT.TXT;2") == 0)
                        found_v2 = 1;
                }
                closedir(d);
            }
            CHECK(found_v1 && found_v2,
                  "readdir shows both RESULT.TXT;1 and ;2");
        }
    }

    /* --- Test: mkdir --- */
    {
        char dirpath[256], fpath[256];
        snprintf(dirpath, sizeof(dirpath), "%s/SUBDIR", MOUNT_POINT);

        rc = mkdir(dirpath, 0755);
        CHECK(rc == 0, "mkdir SUBDIR");

        rc = stat(dirpath, &st);
        CHECK(rc == 0 && S_ISDIR(st.st_mode), "SUBDIR is a directory");

        /* Create a file inside the subdirectory */
        snprintf(fpath, sizeof(fpath), "%s/SUBDIR/INNER.TXT", MOUNT_POINT);
        fd = open(fpath, O_CREAT | O_WRONLY, 0644);
        CHECK(fd >= 0, "create INNER.TXT in SUBDIR");
        if (fd >= 0) {
            n = write(fd, "inner\n", 6);
            CHECK(n == 6, "write to INNER.TXT");
            close(fd);
        }
    }

    /* --- Check free blocks decreased (before any deletes) --- */
    {
        struct statfs sfs;
        rc = statfs(MOUNT_POINT, &sfs);
        if (rc == 0 && initial_free > 0) {
            /*
             * We created: TEST.DAT (1 data block), RESULT.TXT;1 (1),
             * RESULT.TXT;2 (1), SUBDIR (1 data block), INNER.TXT (1).
             * Total: 5 data blocks used.
             */
            CHECK(sfs.f_bfree < (unsigned long)initial_free,
                  "free blocks decreased after writes");
            printf("    initial=%ld, now=%ld (delta=%ld)\n",
                   initial_free, (long)sfs.f_bfree,
                   initial_free - (long)sfs.f_bfree);
        }
    }

    /* --- Test: unlink --- */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/TEST.DAT", MOUNT_POINT);

        rc = unlink(path);
        CHECK(rc == 0, "unlink TEST.DAT");

        rc = stat(path, &st);
        CHECK(rc != 0 && errno == ENOENT, "TEST.DAT gone after unlink");
    }

    /* --- Test: rmdir (must be empty) --- */
    {
        char fpath[256], dirpath[256];
        snprintf(fpath, sizeof(fpath), "%s/SUBDIR/INNER.TXT", MOUNT_POINT);
        snprintf(dirpath, sizeof(dirpath), "%s/SUBDIR", MOUNT_POINT);

        rc = unlink(fpath);
        CHECK(rc == 0, "unlink INNER.TXT before rmdir");

        rc = rmdir(dirpath);
        CHECK(rc == 0, "rmdir SUBDIR (now empty)");

        rc = stat(dirpath, &st);
        CHECK(rc != 0 && errno == ENOENT, "SUBDIR gone after rmdir");
    }

    rc = umount(MOUNT_POINT);
    CHECK(rc == 0, "umount read-write mount");

    /* ============================================================
     * PHASE 3: Remount read-only — verify persistence
     * ============================================================ */

    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", MS_RDONLY, NULL);
    CHECK(rc == 0, "remount read-only for persistence check");
    if (rc == 0) {
        /* RESULT.TXT;1 and ;2 should persist (we didn't delete them) */
        {
            char path[256];
            snprintf(path, sizeof(path), "%s/RESULT.TXT;1", MOUNT_POINT);
            fd = open(path, O_RDONLY);
            CHECK(fd >= 0, "RESULT.TXT;1 persisted across remount");
            if (fd >= 0) {
                memset(buf, 0, sizeof(buf));
                read(fd, buf, sizeof(buf) - 1);
                CHECK(strcmp(buf, "Version one\n") == 0,
                      "RESULT.TXT;1 content correct after remount");
                close(fd);
            }

            snprintf(path, sizeof(path), "%s/RESULT.TXT;2", MOUNT_POINT);
            fd = open(path, O_RDONLY);
            CHECK(fd >= 0, "RESULT.TXT;2 persisted across remount");
            if (fd >= 0) {
                memset(buf, 0, sizeof(buf));
                read(fd, buf, sizeof(buf) - 1);
                CHECK(strcmp(buf, "Version two\n") == 0,
                      "RESULT.TXT;2 content correct after remount");
                close(fd);
            }
        }

        /* HELLO.TXT;1 should still exist (unchanged) */
        {
            char path[256];
            snprintf(path, sizeof(path), "%s/HELLO.TXT;1", MOUNT_POINT);
            fd = open(path, O_RDONLY);
            CHECK(fd >= 0, "HELLO.TXT;1 still exists after remount");
            if (fd >= 0) close(fd);
        }

        /* TEST.DAT and SUBDIR should be gone */
        {
            char path[256];
            snprintf(path, sizeof(path), "%s/TEST.DAT", MOUNT_POINT);
            rc = stat(path, &st);
            CHECK(rc != 0, "TEST.DAT stays deleted after remount");

            snprintf(path, sizeof(path), "%s/SUBDIR", MOUNT_POINT);
            rc = stat(path, &st);
            CHECK(rc != 0, "SUBDIR stays deleted after remount");
        }

        umount(MOUNT_POINT);
    }

    printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
