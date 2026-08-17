/*
 * mkimage_ods2_search.c - Master a GENUINE ODS-2 (Files-11 L2, "DECFILE11B")
 * test volume carrying a subdirectory [SRCH] with MULTIPLE names AND MULTIPLE
 * versions, so tests/qemu/test_syssvc_acp_search.c can prove the executive
 * Files-11 ACP's IO$_ACPCONTROL wildcard directory search ($SEARCH primitive,
 * vms-a0b, epic vms-208) against a real /dev/vms: successive one-per-call
 * matches in genuine ODS-2 directory order (name ascending, version descending),
 * terminating SS$_NOMOREFILES.
 *
 * The real-VAX fixture tests/ods2/real_vax_ods2.dsk carries only single
 * versions (HELLO.TXT;1, WORLD.TXT;1); the $SEARCH proof needs a directory with
 * a name at several versions, which this generated volume provides. It is built
 * with the SAME byte-genuine writer (src/vmsfs/ods2/ods2_writer.c) the
 * `vms_initialize --ods2` tool and mkimage_ods2_real.c use -- it adds no new
 * ODS-2 knowledge (CLAUDE.md Rule 8; see vmsfs/ods2.h's provenance citations).
 *
 * DIRECTORY [SRCH] contents (deterministic -- the writer allocates FIDs
 * sequentially from ODS2_RESFILES+1):
 *
 *     A.TXT;3   A.TXT;2   A.TXT;1     (one name, three versions)
 *     B.LOG;1                          (B.LOG sorts before B.TXT)
 *     B.TXT;1
 *     README.DAT;1
 *
 * The tool PRINTS every {name;version -> FID} it lays down (see below), so the
 * test's ground-truth constants are the writer's own deterministic output, not
 * values the test invents.
 *
 * Usage: mkimage_ods2_search <output-file> [size-in-MB]
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

/* Create a file (RFM=VAR text) AND enter it in the directory under
 * {name, version}. Prints the resulting FID. Exits on any writer error. */
static void add_file(ods2_wvolume_t *wvol, ods2_fid_t dir_fid,
                     const char *name, uint16_t version, const char *text)
{
    ods2_fid_t fid;
    ods2_status_t st;

    st = ods2_wvolume_create_file(wvol, name, version,
                                  (const uint8_t *)text, strlen(text),
                                  dir_fid, &fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_search: create_file %s;%u failed: %s\n",
                name, version, ods2_strerror(st));
        exit(1);
    }
    st = ods2_wvolume_dir_insert(wvol, dir_fid, name, version, fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_search: dir_insert %s;%u failed: %s\n",
                name, version, ods2_strerror(st));
        exit(1);
    }
    printf("  %-14s;%u -> FID (%u,%u,%u)\n", name, version,
           (unsigned)ods2_fid_number(&fid), fid.fid_seq, fid.fid_rvn);
}

int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: mkimage_ods2_search <output-file> [size-in-MB]\n");
        return 1;
    }
    const char *outpath = argv[1];
    uint64_t size_mb = (argc == 3) ? (uint64_t)strtoul(argv[2], NULL, 10) : DEFAULT_SIZE_MB;
    uint64_t size_bytes = size_mb * 1024ULL * 1024ULL;
    uint32_t total_blocks = (uint32_t)(size_bytes / ODS2_BLOCK_SIZE);

    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "mkimage_ods2_search: volume too small (%u blocks, need %d)\n",
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
        fprintf(stderr, "mkimage_ods2_search: cannot allocate %zu bytes\n", image_len);
        return 1;
    }

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles     = maxfiles;
    params.volname      = "OVMXSEARCH";

    ods2_wvolume_t wvol;
    ods2_status_t st = ods2_volume_format(image, image_len, &params, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_search: ods2_volume_format failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }

    /* Subdirectory [SRCH] under the MFD [000000]. */
    ods2_fid_t srch_fid;
    st = ods2_wvolume_create_dir(&wvol, "SRCH.DIR", 1, wvol.mfd_fid, &srch_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_search: create_dir SRCH.DIR failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    st = ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "SRCH.DIR", 1, srch_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "mkimage_ods2_search: dir_insert SRCH.DIR failed: %s\n",
                ods2_strerror(st));
        free(image);
        return 1;
    }
    printf("mkimage_ods2_search: [000000]SRCH.DIR;1 -> FID (%u,%u,%u)\n",
           (unsigned)ods2_fid_number(&srch_fid), srch_fid.fid_seq, srch_fid.fid_rvn);
    printf("mkimage_ods2_search: [SRCH] entries:\n");

    /* A.TXT at three versions (inserted 1,2,3 -> stored descending 3,2,1). */
    add_file(&wvol, srch_fid, "A.TXT", 1, "alpha version one\n");
    add_file(&wvol, srch_fid, "A.TXT", 2, "alpha version two\n");
    add_file(&wvol, srch_fid, "A.TXT", 3, "alpha version three\n");
    /* B.LOG and B.TXT (B.LOG sorts before B.TXT), single versions. */
    add_file(&wvol, srch_fid, "B.LOG", 1, "bravo log\n");
    add_file(&wvol, srch_fid, "B.TXT", 1, "bravo text\n");
    /* README.DAT, single version. */
    add_file(&wvol, srch_fid, "README.DAT", 1, "readme data\n");

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

    printf("mkimage_ods2_search: wrote %s (%u blocks, %u max files, genuine ODS-2/DECFILE11B)\n",
           outpath, total_blocks, maxfiles);
    return 0;
}
