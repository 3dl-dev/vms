/*
 * mkimage_ods2_real.c - Create a GENUINE ODS-2 (Files-11 L2, "DECFILE11B")
 * test image via ods2_volume_format() (src/vmsfs/ods2/ods2_writer.c) -- the
 * SAME byte-genuine writer `vms_initialize --ods2` uses (tools/vms_initialize.c
 * format_volume_ods2()), reduced to a standalone host CLI so it can be built
 * with a single `cc` invocation (mirroring tests/qemu/mkimage_vmsfs.c) inside
 * the ovmx-cross-vax container without pulling in vmssys/libvms.
 *
 * Purpose (rd vms-1c7): master a real-ODS2-formatted volume distinct from the
 * OVMX bespoke-VMFS format tests/qemu/mkimage_vmsfs.c masters, so
 * tests/lab-vax/drive_vmsfs_reject_vax.py can prove vmsfs.kmod's mount-
 * rejection path (bad home-block magic) returns a clean EINVAL instead of
 * panicking ("vrelel: bad ref count") when NetBSD/vax's vmsfs.kmod --
 * understanding only the OVMX VMFS format -- is asked to mount it.
 *
 * Clean-room (CLAUDE.md Rule 8): this file adds no new ODS-2 knowledge -- it
 * is a thin CLI over the existing, already-reviewed ods2_volume_format()
 * writer (src/vmsfs/include/vmsfs/ods2.h; see that header's own provenance
 * citations).
 *
 * Usage: mkimage_ods2_real <output-file> [size-in-MB]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "vmsfs/ods2.h"

#define DEFAULT_SIZE_MB 1
#define MIN_BLOCKS      64

static int write_block(int fd, uint32_t lbn, const void *buf)
{
    off_t offset = (off_t)lbn * ODS2_BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    if (write(fd, buf, ODS2_BLOCK_SIZE) != ODS2_BLOCK_SIZE)
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: mkimage_ods2_real <output-file> [size-in-MB]\n");
        return 1;
    }
    const char *outpath = argv[1];
    uint64_t size_mb = (argc == 3) ? (uint64_t)strtoul(argv[2], NULL, 10) : DEFAULT_SIZE_MB;
    uint64_t size_bytes = size_mb * 1024ULL * 1024ULL;
    uint32_t total_blocks = (uint32_t)(size_bytes / ODS2_BLOCK_SIZE);

    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "mkimage_ods2_real: volume too small (%u blocks, need %d)\n",
                total_blocks, MIN_BLOCKS);
        return 1;
    }

    uint32_t maxfiles = total_blocks / 100;
    if (maxfiles < 16)               maxfiles = 16;
    if (maxfiles > 65535)            maxfiles = 65535;
    if (maxfiles < ODS2_RESFILES)    maxfiles = ODS2_RESFILES;

    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, image_len);
    if (!image) {
        fprintf(stderr, "mkimage_ods2_real: cannot allocate %zu bytes\n", image_len);
        return 1;
    }

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles      = maxfiles;
    params.volname        = "OVMXREALODS2";

    ods2_wvolume_t wvol;
    ods2_status_t st = ods2_volume_format(image, image_len, &params, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_real: ods2_volume_format failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }

    int fd = open(outpath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(outpath);
        free(image);
        return 1;
    }
    if (ftruncate(fd, (off_t)image_len) < 0) {
        perror("ftruncate");
        close(fd);
        free(image);
        return 1;
    }

    /* Write only the populated metadata region -- matches vms_initialize.c's
     * format_volume_ods2(); the data area is already accounted as free in
     * the on-disk storage bitmap, and ftruncate() above already sparse-zeros
     * the rest of the file. */
    for (uint32_t lbn = 0; lbn < wvol.next_free_lbn; lbn++) {
        if (write_block(fd, lbn, image + (size_t)lbn * ODS2_BLOCK_SIZE) < 0) {
            perror("write_block");
            close(fd);
            free(image);
            return 1;
        }
    }

    close(fd);
    free(image);

    printf("mkimage_ods2_real: wrote %s (%u blocks, %u max files, genuine ODS-2/DECFILE11B)\n",
           outpath, total_blocks, maxfiles);
    return 0;
}
