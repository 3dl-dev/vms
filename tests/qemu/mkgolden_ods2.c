/*
 * mkgolden_ods2.c - produce the BYTE-IDENTICAL oracle for test_kmod_ods2_codec.
 *
 * Reads [OVMXDIR]HELLO.TXT out of a genuine ODS-2 volume using the USERSPACE
 * genuine ODS-2 codec (ods2_bdev_open/_resolve_file/_read_file, RAW content),
 * and writes those bytes to a golden file. The in-guest QEMU test then mounts
 * the SAME volume via the KERNEL-RESIDENT codec (ods2ro) and asserts its read
 * is byte-for-byte equal to this golden -- proving the one codec produces
 * identical bytes in both worlds. rd vms-dcd, epic vms-208.
 *
 * A thin CLI over the already-reviewed codec (mirrors mkimage_ods2_real.c); it
 * adds no ODS-2 knowledge (Rule 8).
 *
 * Usage: mkgolden_ods2 <ods2-volume> <golden-out>
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: mkgolden_ods2 <ods2-volume> <golden-out>\n");
        return 2;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 1; }

    ods2_bdev_t bv;
    ods2_status_t st = ods2_bdev_open(&bv, fd, 0);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkgolden_ods2: not a genuine ODS-2 volume: %s\n",
                ods2_strerror(st));
        close(fd);
        return 1;
    }

    const char *comps[] = { "OVMXDIR" };
    ods2_fid_t fid;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    st = ods2_bdev_resolve_file(&bv, comps, 1, "HELLO.TXT", /*highest*/0,
                                &fid, hdr, sizeof(hdr));
    if (st != ODS2_OK) {
        fprintf(stderr, "mkgolden_ods2: resolve [OVMXDIR]HELLO.TXT: %s\n",
                ods2_strerror(st));
        close(fd);
        return 1;
    }

    static uint8_t buf[1u << 20];   /* 1 MB: far more than the fixture file */
    size_t len = 0;
    st = ods2_bdev_read_file(&bv, hdr, buf, sizeof(buf), &len);
    close(fd);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkgolden_ods2: read_file: %s\n", ods2_strerror(st));
        return 1;
    }

    int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror(argv[2]); return 1; }
    if (write(out, buf, len) != (ssize_t)len) {
        perror("write golden");
        close(out);
        return 1;
    }
    close(out);

    printf("mkgolden_ods2: wrote %s (%zu bytes) from [OVMXDIR]HELLO.TXT\n",
           argv[2], len);
    return 0;
}
