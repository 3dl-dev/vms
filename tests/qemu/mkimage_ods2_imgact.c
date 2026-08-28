/*
 * mkimage_ods2_imgact.c - Master a GENUINE ODS-2 (Files-11 L2, "DECFILE11B")
 * test volume carrying a subdirectory [IMGACT] with a real ELF image
 * TESTIMG.EXE, so tests/qemu/test_syssvc_imgact_acp.c can prove that IMGACT's
 * executive Files-11 ACP reader (src/imgact/imgact_acp.c, vms-3e8e) activates
 * an image by reading its header + PT_LOAD segments over IO$_ACCESS +
 * IO$_READVBLK against a real /dev/vms -- byte-for-byte the on-disk image.
 *
 * The image bytes are the deterministic fixture ELF from
 * tests/qemu/imgact_acp_fixture_elf.h, which the test rebuilds in memory as its
 * golden, so the ground truth is the builder's own output (as mkimage_ods2_search
 * does for the $SEARCH proof). Built with the SAME byte-genuine ODS-2 writer
 * (src/vmsfs/ods2/ods2_writer.c) the other mkimage_ods2_* fixtures use -- it
 * adds no new ODS-2 knowledge (CLAUDE.md Rule 8; provenance in vmsfs/ods2.h).
 *
 *     [IMGACT]TESTIMG.EXE;1     (FID printed below; a well-formed ELF64 with
 *                                two PT_LOAD segments, 1424 bytes)
 *
 * Usage: mkimage_ods2_imgact <output-file> [size-in-MB]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "vmsfs/ods2.h"
#include "imgact_acp_fixture_elf.h"

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
        fprintf(stderr, "Usage: mkimage_ods2_imgact <output-file> [size-in-MB]\n");
        return 1;
    }
    const char *outpath = argv[1];
    uint64_t size_mb = (argc == 3) ? (uint64_t)strtoul(argv[2], NULL, 10) : DEFAULT_SIZE_MB;
    uint64_t size_bytes = size_mb * 1024ULL * 1024ULL;
    uint32_t total_blocks = (uint32_t)(size_bytes / ODS2_BLOCK_SIZE);

    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "mkimage_ods2_imgact: volume too small (%u blocks, need %d)\n",
                total_blocks, MIN_BLOCKS);
        return 1;
    }

    uint32_t maxfiles = total_blocks / 100;
    if (maxfiles < 32)               maxfiles = 32;
    if (maxfiles > 65535)            maxfiles = 65535;
    if (maxfiles < ODS2_RESFILES)    maxfiles = ODS2_RESFILES;

    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, image_len);
    if (!image) {
        fprintf(stderr, "mkimage_ods2_imgact: cannot allocate %zu bytes\n", image_len);
        return 1;
    }

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles     = maxfiles;
    params.volname      = "OVMXIMGACT";

    ods2_wvolume_t wvol;
    ods2_status_t st = ods2_volume_format(image, image_len, &params, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_imgact: ods2_volume_format failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }

    /* Subdirectory [IMGACT] under the MFD [000000]. */
    ods2_fid_t dir_fid;
    st = ods2_wvolume_create_dir(&wvol, "IMGACT.DIR", 1, wvol.mfd_fid, &dir_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_imgact: create_dir IMGACT.DIR failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "IMGACT.DIR", 1, dir_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_imgact: dir_insert IMGACT.DIR failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    printf("mkimage_ods2_imgact: [000000]IMGACT.DIR;1 -> FID (%u,%u,%u)\n",
           (unsigned)ods2_fid_number(&dir_fid), dir_fid.fid_seq, dir_fid.fid_rvn);

    /* The image file: the deterministic fixture ELF, laid down verbatim. */
    uint8_t elf[IMGACT_FIX_TOTAL];
    size_t elf_len = imgact_acp_fixture_elf_build(elf, sizeof(elf));
    if (elf_len != IMGACT_FIX_TOTAL) {
        fprintf(stderr, "mkimage_ods2_imgact: fixture ELF build failed\n");
        free(image);
        return 1;
    }

    /* create_file_raw (RFM=FIXED, VERBATIM bytes), NOT create_file: a real .EXE
     * image is stored unframed. create_file frames its input as RMS VAR text
     * records (2-byte length words per line), which corrupts a binary -- the
     * writer's own docs warn a boot master built on create_file "cannot store
     * binaries". IMGACT then reads these bytes back byte-for-byte. */
    ods2_fid_t img_fid;
    st = ods2_wvolume_create_file_raw(&wvol, "TESTIMG.EXE", 1, elf, elf_len,
                                      dir_fid, &img_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_imgact: create_file_raw TESTIMG.EXE failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    st = ods2_wvolume_dir_insert(&wvol, dir_fid, "TESTIMG.EXE", 1, img_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_imgact: dir_insert TESTIMG.EXE failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    printf("mkimage_ods2_imgact: [IMGACT]TESTIMG.EXE;1 -> FID (%u,%u,%u), %zu bytes\n",
           (unsigned)ods2_fid_number(&img_fid), img_fid.fid_seq, img_fid.fid_rvn,
           elf_len);

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

    printf("mkimage_ods2_imgact: wrote %s (%u blocks, %u max files, genuine ODS-2/DECFILE11B)\n",
           outpath, total_blocks, maxfiles);
    return 0;
}
