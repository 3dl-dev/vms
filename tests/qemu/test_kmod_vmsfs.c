/*
 * test_kmod_vmsfs.c - Test vmsfs.ko from userspace inside QEMU initramfs
 *
 * Tests:
 *   1. Verify vmsfs is registered in /proc/filesystems
 *   2. Mount vmsfs with a backing directory
 *   3. Create a file - auto-creates as ;1 in backing
 *   4. Verify backing directory has the versioned file
 *   5. Create same file again - auto-creates as ;2
 *   6. Verify both versions exist in backing
 *   7. Read back file content and verify it matches
 *   8. Unlink a specific version and verify it's gone
 *   9. Unmount
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
#include <stdint.h>

/* Must match src/kernel/vmsfs/vmsfs.h (vms-1c9). */
#define VMSFS_MAGIC          0x564D5346  /* "VMSF" — structured volume    */
#define VMSFS_OVERLAY_MAGIC  0x564D4F56  /* "VMOV" — overlay passthrough  */

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while(0)

/*
 * Check whether a file exists in a directory by scanning with readdir.
 * Returns 1 if found, 0 if not.
 */
static int file_exists_in_dir(const char *dirpath, const char *filename)
{
    DIR *d = opendir(dirpath);
    struct dirent *ent;

    if (!d)
        return 0;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, filename) == 0) {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/*
 * Check whether a file exists at the given path via stat.
 */
static int path_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so a still-buffered write cannot splice into a child process output */
    int fd, fd2, rc;
    ssize_t n;
    char buf[256];
    const char *backing = "/tmp/vmsfs_backing";
    const char *mountpt = "/mnt/vmsfs";
    const char *test_data_1 = "hello version one\n";
    const char *test_data_2 = "hello version two\n";

    printf("=== test_kmod_vmsfs ===\n");

    /* 1. Verify vmsfs is registered in /proc/filesystems */
    {
        FILE *f = fopen("/proc/filesystems", "r");
        int found = 0;

        if (f) {
            while (fgets(buf, sizeof(buf), f)) {
                if (strstr(buf, "vmsfs")) {
                    found = 1;
                    break;
                }
            }
            fclose(f);
        }
        CHECK(found, "vmsfs registered in /proc/filesystems");
        if (!found) {
            printf("  FATAL: vmsfs not registered, cannot continue\n");
            printf("=== test_kmod_vmsfs: %d passed, %d failed ===\n",
                   pass, fail);
            return 1;
        }
    }

    /* 2. Create backing directory and mount point */
    mkdir(backing, 0755);
    mkdir(mountpt, 0755);

    /* 3. Mount vmsfs */
    rc = mount("none", mountpt, "vmsfs", 0,
               "backing=/tmp/vmsfs_backing");
    CHECK(rc == 0, "mount vmsfs with backing directory");
    if (rc != 0) {
        printf("  FATAL: mount failed (errno=%d), cannot continue\n", errno);
        printf("=== test_kmod_vmsfs: %d passed, %d failed ===\n",
               pass, fail);
        return 1;
    }

    /*
     * 3a. vms-1c9: an overlay mount is a passthrough over a host directory —
     * it has no home block, storage bitmap, file headers or FIDs. It MUST
     * report itself honestly, distinguishable from a mastered structured
     * vmsfs volume, and must NOT stamp the structured-volume identity onto
     * the host filesystem's numbers. The block-device (structured) suite
     * asserts f_type == VMSFS_MAGIC; here we assert the overlay reports the
     * DISTINCT overlay magic and NOT the structured magic.
     */
    {
        struct statfs sfs;
        rc = statfs(mountpt, &sfs);
        CHECK(rc == 0, "statfs on overlay mount succeeds");
        if (rc == 0) {
            CHECK((uint32_t)sfs.f_type == VMSFS_OVERLAY_MAGIC,
                  "overlay statfs f_type is VMSFS_OVERLAY_MAGIC (honest passthrough identity)");
            CHECK((uint32_t)sfs.f_type != VMSFS_MAGIC,
                  "overlay does NOT report the structured VMSFS_MAGIC (not LARPing as a structured volume)");
        }
    }

    /* 4. Create a file - should auto-create as ;1 in backing */
    fd = open("/mnt/vmsfs/TEST.TXT", O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0, "create TEST.TXT (should become ;1)");
    if (fd >= 0) {
        n = write(fd, test_data_1, strlen(test_data_1));
        CHECK(n == (ssize_t)strlen(test_data_1),
              "write data to TEST.TXT;1");
        close(fd);
    }

    /* 5. Verify backing directory has TEST.TXT;1 */
    CHECK(file_exists_in_dir(backing, "TEST.TXT;1"),
          "backing has TEST.TXT;1");

    /* 6. Create the same file again - should auto-create as ;2 */
    fd2 = open("/mnt/vmsfs/TEST.TXT", O_CREAT | O_WRONLY, 0644);
    CHECK(fd2 >= 0, "create TEST.TXT again (should become ;2)");
    if (fd2 >= 0) {
        n = write(fd2, test_data_2, strlen(test_data_2));
        CHECK(n == (ssize_t)strlen(test_data_2),
              "write data to TEST.TXT;2");
        close(fd2);
    }

    /* 7. Verify both versions exist in backing */
    CHECK(file_exists_in_dir(backing, "TEST.TXT;1"),
          "backing still has TEST.TXT;1");
    CHECK(file_exists_in_dir(backing, "TEST.TXT;2"),
          "backing has TEST.TXT;2");

    /* 8. Read back the file content from version 1 and verify */
    fd = open("/mnt/vmsfs/TEST.TXT;1", O_RDONLY);
    CHECK(fd >= 0, "open TEST.TXT;1 for reading");
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, sizeof(buf) - 1);
        CHECK(n == (ssize_t)strlen(test_data_1),
              "read correct number of bytes from TEST.TXT;1");
        CHECK(strcmp(buf, test_data_1) == 0,
              "content of TEST.TXT;1 matches written data");
        close(fd);
    }

    /* 9. Read back the file content from version 2 and verify */
    fd = open("/mnt/vmsfs/TEST.TXT;2", O_RDONLY);
    CHECK(fd >= 0, "open TEST.TXT;2 for reading");
    if (fd >= 0) {
        memset(buf, 0, sizeof(buf));
        n = read(fd, buf, sizeof(buf) - 1);
        CHECK(n == (ssize_t)strlen(test_data_2),
              "read correct number of bytes from TEST.TXT;2");
        CHECK(strcmp(buf, test_data_2) == 0,
              "content of TEST.TXT;2 matches written data");
        close(fd);
    }

    /* 10. Unlink version 1 and verify it's gone */
    rc = unlink("/mnt/vmsfs/TEST.TXT;1");
    CHECK(rc == 0, "unlink TEST.TXT;1");

    CHECK(!path_exists("/tmp/vmsfs_backing/TEST.TXT;1"),
          "TEST.TXT;1 gone from backing after unlink");
    CHECK(path_exists("/tmp/vmsfs_backing/TEST.TXT;2"),
          "TEST.TXT;2 still in backing after unlink of ;1");

    /* 11. Unmount */
    rc = umount(mountpt);
    CHECK(rc == 0, "umount vmsfs");

    printf("=== test_kmod_vmsfs: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
