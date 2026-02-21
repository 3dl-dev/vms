/*
 * test_kmod_vmsfs_blkdev.c - Test vmsfs block-device mount from QEMU initramfs
 *
 * Exercises the block-device mount path (mount_bdev) in vmsfs.ko:
 *   1. Mount a pre-formatted vmsfs image via loop device (read-only)
 *   2. Verify readdir lists HELLO.TXT;1
 *   3. Stat the file and verify size
 *   4. Read file content and verify it matches
 *   5. Test case-insensitive lookup (hello.txt)
 *   6. Test version-less lookup (HELLO.TXT → highest version)
 *   7. Verify statfs reports correct values
 *   8. Unmount
 *
 * The test image is created by mkimage_vmsfs at Docker build time and
 * placed at /test_data/vmsfs_test.img in the initramfs. init.sh sets
 * up the loop device at /dev/loop0 before running this test.
 *
 * Statically linked, runs inside a minimal BusyBox initramfs.
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

int main(void)
{
    int rc, fd;
    ssize_t n;
    char buf[256];
    struct stat st;

    printf("=== test_kmod_vmsfs_blkdev ===\n");

    /* 1. Verify loop device exists (set up by init.sh) */
    rc = stat(LOOP_DEV, &st);
    if (rc != 0) {
        printf("  SKIP: %s not found (loop device not set up)\n", LOOP_DEV);
        printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
               pass, fail);
        return 0;  /* skip, don't fail */
    }
    CHECK(S_ISBLK(st.st_mode), "loop device is a block device");

    /* 2. Create mount point */
    mkdir(MOUNT_POINT, 0755);

    /* 3. Mount vmsfs on loop device (read-only, block-device mode) */
    rc = mount(LOOP_DEV, MOUNT_POINT, "vmsfs", MS_RDONLY, NULL);
    CHECK(rc == 0, "mount vmsfs on block device (read-only)");
    if (rc != 0) {
        printf("  FATAL: mount failed (errno=%d), cannot continue\n", errno);
        printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
               pass, fail);
        return 1;
    }

    /* 4. List root directory — should contain HELLO.TXT;1 */
    {
        DIR *d = opendir(MOUNT_POINT);
        int found_hello = 0;
        int entry_count = 0;

        CHECK(d != NULL, "opendir mount point");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                /* Skip . and .. */
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

    /* 5. Stat the file — verify size */
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
    }

    /* 6. Read file content — verify it matches */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/HELLO.TXT;1", MOUNT_POINT);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "open HELLO.TXT;1 for reading");
        if (fd >= 0) {
            memset(buf, 0, sizeof(buf));
            n = read(fd, buf, sizeof(buf) - 1);
            CHECK(n == (ssize_t)EXPECTED_LEN,
                  "read correct number of bytes");
            CHECK(memcmp(buf, EXPECTED_CONTENT, EXPECTED_LEN) == 0,
                  "file content matches expected");
            close(fd);
        }
    }

    /* 7. Case-insensitive lookup — open as "hello.txt;1" */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/hello.txt;1", MOUNT_POINT);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0, "case-insensitive lookup (hello.txt;1)");
        if (fd >= 0) close(fd);
    }

    /* 8. Version-less lookup — "HELLO.TXT" resolves to highest version */
    {
        char path[256];
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

    /* 9. Verify statfs */
    {
        struct statfs sfs;
        rc = statfs(MOUNT_POINT, &sfs);
        CHECK(rc == 0, "statfs succeeds");
        if (rc == 0) {
            CHECK((uint32_t)sfs.f_type == VMSFS_MAGIC,
                  "statfs f_type is VMSFS_MAGIC");
            CHECK(sfs.f_bsize == 512,
                  "statfs block size is 512");
            CHECK(sfs.f_blocks == 2048,
                  "statfs reports 2048 total blocks");
        }
    }

    /* 10. Unmount */
    rc = umount(MOUNT_POINT);
    CHECK(rc == 0, "umount vmsfs block device");

    printf("=== test_kmod_vmsfs_blkdev: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
