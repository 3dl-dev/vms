/*
 * test_kmod_ods2_codec.c - Rule-9 QEMU proof that the GENUINE ODS-2 codec runs
 * KERNEL-RESIDENT (rd vms-dcd, epic vms-208, the Files-11 ODS-2 ACP foundation
 * rung).
 *
 * Mounts the REAL-VAX fixture tests/ods2/real_vax_ods2.dsk (staged into the
 * initramfs at /test_data/ods2_real.img, attached to a loop device) via the
 * read-only "ods2ro" filesystem in vmsfs.ko, and reads a file back. The mount
 * and every read run the genuine ODS-2 codec (src/vmsfs/ods2/) compiled
 * KERNEL-RESIDENT (-DOVMX_ODS2_KERNEL), off a real struct block_device, through
 * the shared FS engine's vmsfs_bio.h backend -- with NO userspace codec and NO
 * POSIX in the kernel path.
 *
 * The byte-identical oracle is /test_data/hello.golden, produced at image-build
 * time by the USERSPACE codec (tests/qemu/mkgolden_ods2, ods2_bdev_read_file
 * over the same fixture). This test only mounts + read()s + memcmp()s: the
 * kernel read must equal the userspace-codec read, byte for byte.
 *
 * SKIPs honestly (0 failures) if the fixture/golden are absent, matching
 * test_kmod_vmsfs_blkdev's convention -- never a fabricated pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <errno.h>
#include <linux/loop.h>
#include <sys/ioctl.h>

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s (errno=%d)\n", msg, errno); fail++; } \
} while (0)

#define FIXTURE    "/test_data/ods2_real.img"
#define GOLDEN     "/test_data/hello.golden"
#define LOOP_DEV   "/dev/loop1"
#define MOUNT_PT   "/mnt/ods2ro"
#define HELLO_PATH MOUNT_PT "/OVMXDIR.DIR/HELLO.TXT"

/* Attach FIXTURE to LOOP_DEV. Returns loop fd (>=0) held open, or -1. */
static int loop_attach(void)
{
    int backing, loopfd;

    (void)mknod(LOOP_DEV, S_IFBLK | 0600, makedev(7, 1));

    backing = open(FIXTURE, O_RDONLY);
    if (backing < 0)
        return -1;
    loopfd = open(LOOP_DEV, O_RDONLY);
    if (loopfd < 0) { close(backing); return -1; }
    if (ioctl(loopfd, LOOP_SET_FD, backing) < 0) {
        close(loopfd); close(backing); return -1;
    }
    close(backing);
    return loopfd;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int loopfd, rc, fd;
    struct stat st;
    long golden_len;
    uint8_t *golden = NULL, *kbuf = NULL;

    printf("=== test_kmod_ods2_codec ===\n");

    if (stat(FIXTURE, &st) != 0 || stat(GOLDEN, &st) != 0) {
        printf("  SKIP: %s or %s not staged\n", FIXTURE, GOLDEN);
        printf("=== test_kmod_ods2_codec: %d passed, %d failed ===\n", pass, fail);
        return 0;
    }

    /* Load the userspace-codec golden (the byte-identical oracle). */
    fd = open(GOLDEN, O_RDONLY);
    if (fd >= 0 && fstat(fd, &st) == 0) {
        golden_len = st.st_size;
        golden = malloc(golden_len ? golden_len : 1);
        if (golden && read(fd, golden, golden_len) != golden_len) {
            free(golden); golden = NULL;
        }
        close(fd);
    } else {
        golden_len = 0;
    }
    CHECK(golden != NULL, "loaded userspace-codec golden");

    loopfd = loop_attach();
    CHECK(loopfd >= 0, "attach fixture to loop device");
    if (loopfd < 0) {
        printf("=== test_kmod_ods2_codec: %d passed, %d failed ===\n", pass, fail);
        free(golden);
        return fail > 0 ? 1 : 0;
    }

    mkdir(MOUNT_PT, 0755);

    /* Mount the genuine ODS-2 volume via the kernel-resident codec. This
     * validates the home block (checksums + DECFILE11B) IN-KERNEL. */
    rc = mount(LOOP_DEV, MOUNT_PT, "ods2ro", MS_RDONLY, NULL);
    CHECK(rc == 0, "mount -t ods2ro (kernel-resident ODS-2 codec)");
    if (rc != 0) {
        printf("  FATAL: mount failed (errno=%d)\n", errno);
        ioctl(loopfd, LOOP_CLR_FD, 0); close(loopfd); free(golden);
        printf("=== test_kmod_ods2_codec: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    /* MFD listing: the reserved files + OVMXDIR.DIR (codec directory decode). */
    {
        DIR *d = opendir(MOUNT_PT);
        int found_ovmxdir = 0, found_indexf = 0;
        CHECK(d != NULL, "opendir [000000] (MFD)");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, "OVMXDIR.DIR;1") == 0) found_ovmxdir = 1;
                if (strcmp(e->d_name, "INDEXF.SYS;1") == 0)  found_indexf = 1;
            }
            closedir(d);
        }
        CHECK(found_ovmxdir, "MFD lists OVMXDIR.DIR;1");
        CHECK(found_indexf,  "MFD lists INDEXF.SYS;1");
    }

    /* Subdirectory listing: HELLO.TXT + WORLD.TXT inside [OVMXDIR]. */
    {
        DIR *d = opendir(MOUNT_PT "/OVMXDIR.DIR");
        int found_hello = 0, found_world = 0;
        CHECK(d != NULL, "opendir [OVMXDIR]");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, "HELLO.TXT;1") == 0) found_hello = 1;
                if (strcmp(e->d_name, "WORLD.TXT;1") == 0) found_world = 1;
            }
            closedir(d);
        }
        CHECK(found_hello, "[OVMXDIR] lists HELLO.TXT;1");
        CHECK(found_world, "[OVMXDIR] lists WORLD.TXT;1");
    }

    /* THE PROOF: read [OVMXDIR]HELLO.TXT in-kernel; must equal the golden. */
    rc = stat(HELLO_PATH, &st);
    CHECK(rc == 0, "stat [OVMXDIR]HELLO.TXT");
    CHECK(rc == 0 && st.st_size == golden_len,
          "kernel file size == userspace-codec byte count");

    fd = open(HELLO_PATH, O_RDONLY);
    CHECK(fd >= 0, "open [OVMXDIR]HELLO.TXT");
    if (fd >= 0 && golden) {
        kbuf = malloc(golden_len ? golden_len : 1);
        ssize_t n = kbuf ? read(fd, kbuf, golden_len) : -1;
        CHECK(n == golden_len, "read full file content in-kernel");
        CHECK(n == golden_len && memcmp(kbuf, golden, golden_len) == 0,
              "BYTE-IDENTICAL: kernel-resident codec read == userspace codec read");
        /* EOF at exactly the valid byte count (no trailing block padding). */
        {
            uint8_t extra;
            CHECK(read(fd, &extra, 1) == 0, "EOF at end of valid data");
        }
        free(kbuf);
    }
    if (fd >= 0) close(fd);

    umount(MOUNT_PT);
    ioctl(loopfd, LOOP_CLR_FD, 0);
    close(loopfd);
    free(golden);

    printf("=== test_kmod_ods2_codec: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
